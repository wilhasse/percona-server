#!/bin/bash
# Script to reset DuckDB replication with fresh GTID position
# Usage: ./restart-duckdb-replication.sh [source_host]

set -e

# Configuration
SOURCE_HOST="${1:-10.1.0.21}"
SOURCE_PORT=3306
SOURCE_USER=root
REPLICA_CNF="/home/cslog/mysql/replica.cnf"
REPLICA_DATADIR="/home/cslog/mysql/replica-data"
REPLICA_SOCKET="$REPLICA_DATADIR/mysql.sock"
MYSQLD_BIN="/home/cslog/mysql/my-percona-server/build/bin/mysqld"
DUCKDB_PLUGIN="/home/cslog/mysql/my-percona-server/build/lib/plugin/ha_duckdb.so"

echo "=== DuckDB Replication Reset Script ==="
echo "Source: $SOURCE_HOST:$SOURCE_PORT"
echo ""

# Step 1: Stop MySQL if running
echo "[1/5] Stopping MySQL if running..."
if mysql -u root --socket="$REPLICA_SOCKET" -e "SELECT 1" &>/dev/null; then
    mysql -u root --socket="$REPLICA_SOCKET" -e "SET GLOBAL duckdb_binlog_apply_enabled=OFF" 2>/dev/null || true
    mysql -u root --socket="$REPLICA_SOCKET" -e "SHUTDOWN" 2>/dev/null || true
    sleep 3
    echo "      MySQL stopped"
else
    echo "      MySQL not running"
fi

# Step 2: Get current GTID from source
echo "[2/5] Getting current GTID position from source..."
GTID=$(mysql -u "$SOURCE_USER" -h "$SOURCE_HOST" -P "$SOURCE_PORT" -N -e "SELECT @@gtid_executed" 2>/dev/null)
if [ -z "$GTID" ]; then
    echo "ERROR: Could not get GTID from source server"
    exit 1
fi
echo "      GTID: $GTID"

# Step 3: Clean up DuckDB files
echo "[3/5] Cleaning up DuckDB files..."
rm -f "$REPLICA_DATADIR"/*.duckdb "$REPLICA_DATADIR"/*.duckdb.wal 2>/dev/null || true
echo "      DuckDB files removed"

# Step 4: Update replica.cnf with new GTID
echo "[4/5] Updating replica configuration..."
# Use sed to replace the GTID line
sed -i "s|^duckdb-binlog-apply-start-gtid=.*|duckdb-binlog-apply-start-gtid=$GTID|" "$REPLICA_CNF"
echo "      Updated: duckdb-binlog-apply-start-gtid=$GTID"

# Step 5: Start MySQL with DuckDB plugin
echo "[5/5] Starting MySQL with DuckDB plugin..."
cd "$(dirname "$MYSQLD_BIN")"
LD_PRELOAD="$DUCKDB_PLUGIN" "$MYSQLD_BIN" --defaults-file="$REPLICA_CNF" &

# Wait for MySQL to start
echo "      Waiting for MySQL to start..."
for i in {1..30}; do
    if mysql -u root --socket="$REPLICA_SOCKET" -e "SELECT 1" &>/dev/null; then
        break
    fi
    sleep 1
done

# Verify startup
if mysql -u root --socket="$REPLICA_SOCKET" -e "SELECT 1" &>/dev/null; then
    echo ""
    echo "=== MySQL Started Successfully ==="
    mysql -u root --socket="$REPLICA_SOCKET" -e "SHOW GLOBAL STATUS LIKE 'duckdb_binlog%';"
    echo ""
    echo "Ready for replication from $SOURCE_HOST"
else
    echo "ERROR: MySQL failed to start. Check $REPLICA_DATADIR/error.log"
    exit 1
fi
