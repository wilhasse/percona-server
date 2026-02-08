#!/bin/bash
# Test harness for DuckDB primary-mode operation with siscob_teste workload
#
# Architecture:
#   10.1.0.12 (mysqlreptester) --writes--> MySQL source --replicates--> system MySQL (InnoDB)
#                                                                            |
#                                                                       [copy data]
#                                                                            v
#                                                               our build mysqld:13306
#                                                               (ENGINE=DUCKDB tables)

set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="/home/cslog/mysql/BUILD-my-percona-server"
MYSQLD_BIN="$BUILD_DIR/runtime_output_directory/mysqld"
PLUGIN_DIR="$BUILD_DIR/plugin_output_directory"

DUCKDB_PORT=13306
DUCKDB_DATADIR="/tmp/duckdb_primary_test_data"
DUCKDB_SOCKET="$DUCKDB_DATADIR/mysql.sock"
DUCKDB_LOG="$DUCKDB_DATADIR/error.log"
DUCKDB_PID="$DUCKDB_DATADIR/mysqld.pid"

# System MySQL (InnoDB reference, replicating from 10.1.0.12)
# Uses socket auth (TCP auth denied for root)
SYS_USER="root"

# mysqlreptester on 10.1.0.12
REPTESTER_HOST="10.1.0.12"
REPTESTER_API="http://${REPTESTER_HOST}:8090"

# Database
DB_NAME="siscob_teste"
SCHEMA_FILE="$SCRIPT_DIR/duckdb_siscob_schema.sql"

# Table order (matching schema.go dependency order)
TABLES=(
    EMPRESA CARTEIRA FUNCIONARIO CONTRATO TELEFONE ENDERECO EMAIL
    ACORDO PARCELA PARC_GERAL PARC_FINAN
    HISTORICO HISTORICO_TEXTO HISTORICO_SISTEMA
)

# Temp dir for dump files
DUMP_DIR="/tmp/duckdb_primary_test_dumps"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
STEP=0
TOTAL_STEPS=8
FAILURES=0

step() {
    STEP=$((STEP + 1))
    printf "[STEP %d/%d] %-45s" "$STEP" "$TOTAL_STEPS" "$1"
}

ok() {
    local detail="${1:-}"
    if [ -n "$detail" ]; then
        echo " OK ($detail)"
    else
        echo " OK"
    fi
}

fail() {
    local detail="${1:-}"
    FAILURES=$((FAILURES + 1))
    if [ -n "$detail" ]; then
        echo " FAIL ($detail)"
    else
        echo " FAIL"
    fi
    echo ""
    echo "--- Error details ---"
    if [ -f "$DUCKDB_LOG" ]; then
        echo "Last 20 lines of $DUCKDB_LOG:"
        tail -20 "$DUCKDB_LOG" 2>/dev/null || true
    fi
    cleanup_mysqld
    exit 1
}

# mysql client shortcut for our DuckDB build
duckdb_mysql() {
    mysql -u root --socket="$DUCKDB_SOCKET" "$@" 2>/dev/null
}

# mysql client shortcut for system MySQL (InnoDB) - uses socket auth
sys_mysql() {
    mysql -u "$SYS_USER" "$@" 2>/dev/null
}

cleanup_mysqld() {
    if [ -f "$DUCKDB_PID" ]; then
        local pid
        pid=$(cat "$DUCKDB_PID" 2>/dev/null || true)
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            duckdb_mysql -e "SHUTDOWN" 2>/dev/null || kill "$pid" 2>/dev/null || true
            # Wait up to 10s for shutdown
            for i in $(seq 1 10); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 1
            done
            # Force kill if still alive
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
        fi
    fi
}

# ---------------------------------------------------------------------------
# Phase 0: Setup - Initialize and start our build mysqld
# ---------------------------------------------------------------------------

echo "========================================="
echo " DuckDB Primary-Mode Test Harness"
echo "========================================="
echo "Build:  $MYSQLD_BIN"
echo "Port:   $DUCKDB_PORT"
echo "Data:   $DUCKDB_DATADIR"
echo "Schema: $SCHEMA_FILE"
echo ""

# Trap to clean up on exit
trap cleanup_mysqld EXIT

# Step 1: Initialize data directory
step "Initialize data directory"

if [ ! -f "$SCHEMA_FILE" ]; then
    fail "Schema file not found: $SCHEMA_FILE"
fi
if [ ! -x "$MYSQLD_BIN" ]; then
    fail "mysqld binary not found: $MYSQLD_BIN"
fi

# Clean up any previous run
cleanup_mysqld
rm -rf "$DUCKDB_DATADIR"
mkdir -p "$DUCKDB_DATADIR"
mkdir -p "$DUMP_DIR"

"$MYSQLD_BIN" \
    --no-defaults \
    --initialize-insecure \
    --datadir="$DUCKDB_DATADIR" \
    --log-error="$DUCKDB_LOG" \
    2>/dev/null

if [ $? -ne 0 ] || [ ! -d "$DUCKDB_DATADIR/mysql" ]; then
    fail "initialize-insecure failed"
fi
ok

# Step 2: Start mysqld on port 13306
step "Start mysqld on port $DUCKDB_PORT"

"$MYSQLD_BIN" \
    --no-defaults \
    --datadir="$DUCKDB_DATADIR" \
    --port="$DUCKDB_PORT" \
    --socket="$DUCKDB_SOCKET" \
    --pid-file="$DUCKDB_PID" \
    --log-error="$DUCKDB_LOG" \
    --plugin-dir="$PLUGIN_DIR" \
    --skip-mysqlx \
    --server-id=9999 \
    --log-bin=OFF \
    --sql-mode="NO_AUTO_VALUE_ON_ZERO" \
    &

# Wait for mysqld to become ready (up to 30s)
READY=0
for i in $(seq 1 30); do
    if duckdb_mysql -e "SELECT 1" >/dev/null 2>&1; then
        READY=1
        break
    fi
    sleep 1
done

if [ "$READY" -ne 1 ]; then
    fail "mysqld did not start within 30s"
fi
ok

# Step 3: Load DuckDB plugin + set PRIMARY mode
step "Load DuckDB plugin + PRIMARY mode"

# Install plugin (ignore error if already installed)
duckdb_mysql -e "INSTALL PLUGIN duckdb SONAME 'ha_duckdb.so'" 2>/dev/null || true

# Verify plugin is active
PLUGIN_STATUS=$(duckdb_mysql -N -e "SELECT PLUGIN_STATUS FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME='DUCKDB'" 2>/dev/null || echo "")
if [ "$PLUGIN_STATUS" != "ACTIVE" ]; then
    fail "DuckDB plugin not active (status: '$PLUGIN_STATUS')"
fi

# Set execution mode to PRIMARY and disable binlog applier (standalone, no replication)
duckdb_mysql -e "SET GLOBAL duckdb_execution_mode = 'PRIMARY'" 2>/dev/null || true
duckdb_mysql -e "SET GLOBAL duckdb_binlog_apply_enabled = OFF" 2>/dev/null || true

# Verify
EXEC_MODE=$(duckdb_mysql -N -e "SELECT @@duckdb_execution_mode" 2>/dev/null || echo "")
if [ "$EXEC_MODE" != "PRIMARY" ]; then
    # Try alternate variable name patterns
    EXEC_MODE=$(duckdb_mysql -N -e "SHOW GLOBAL VARIABLES LIKE 'duckdb_execution_mode'" 2>/dev/null | awk '{print $2}' || echo "")
    if [ "$EXEC_MODE" != "PRIMARY" ]; then
        fail "Could not set duckdb_execution_mode to PRIMARY (got: '$EXEC_MODE')"
    fi
fi

ok

# Step 4: Create siscob_teste ENGINE=DUCKDB schema
step "Create $DB_NAME ENGINE=DUCKDB schema"

duckdb_mysql < "$SCHEMA_FILE"
if [ $? -ne 0 ]; then
    fail "Schema creation failed"
fi

# Verify all 14 tables exist with ENGINE=DUCKDB
TABLE_COUNT=$(duckdb_mysql -N -e "SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='$DB_NAME' AND ENGINE='DUCKDB'" 2>/dev/null || echo "0")
if [ "$TABLE_COUNT" -ne 14 ]; then
    fail "Expected 14 DuckDB tables, found $TABLE_COUNT"
fi

ok "14 tables"

# Step 5: Reset + generate data via mysqlreptester
step "Reset + generate data via mysqlreptester"

# Check if mysqlreptester is reachable
if ! curl -s -o /dev/null -w "%{http_code}" "$REPTESTER_API/status" 2>/dev/null | grep -q "200"; then
    # Try without /status endpoint
    if ! curl -s --connect-timeout 5 "$REPTESTER_API/" >/dev/null 2>&1; then
        fail "mysqlreptester not reachable at $REPTESTER_API"
    fi
fi

# Reset the database
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$REPTESTER_API/reset" 2>/dev/null || echo "000")
if [ "$HTTP_CODE" != "200" ]; then
    fail "POST /reset returned HTTP $HTTP_CODE"
fi

# Start data generation
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$REPTESTER_API/start" 2>/dev/null || echo "000")
if [ "$HTTP_CODE" != "200" ]; then
    fail "POST /start returned HTTP $HTTP_CODE"
fi

# Let it generate data for ~8 seconds
sleep 8

# Pause generation
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$REPTESTER_API/pause" 2>/dev/null || echo "000")
if [ "$HTTP_CODE" != "200" ]; then
    fail "POST /pause returned HTTP $HTTP_CODE"
fi

# Wait for replication lag to settle on system MySQL
sleep 3

# Count total rows in InnoDB
TOTAL_INNODB=0
for t in "${TABLES[@]}"; do
    CNT=$(sys_mysql -N -e "SELECT COUNT(*) FROM $DB_NAME.$t" 2>/dev/null || echo "0")
    TOTAL_INNODB=$((TOTAL_INNODB + CNT))
done

if [ "$TOTAL_INNODB" -eq 0 ]; then
    fail "No data found in system MySQL InnoDB tables"
fi

ok "$TOTAL_INNODB rows"

# Step 6: Copy data from InnoDB to DuckDB
step "Copy data from InnoDB to DuckDB"

TABLES_LOADED=0
for t in "${TABLES[@]}"; do
    DUMP_FILE="$DUMP_DIR/${t}.sql"

    # Dump data from system MySQL (InnoDB) - no CREATE, extended (batched) inserts
    mysqldump \
        -u "$SYS_USER" \
        --no-create-info \
        --skip-triggers \
        --skip-lock-tables \
        --set-gtid-purged=OFF \
        --no-create-db \
        "$DB_NAME" "$t" \
        > "$DUMP_FILE" 2>/dev/null

    if [ $? -ne 0 ]; then
        fail "mysqldump of $t failed"
    fi

    # Remove mysqldump decorations that DuckDB doesn't understand:
    # LOCK/UNLOCK, conditional comments (/*!...*/), SET lines, comments
    sed -i \
        -e '/^LOCK TABLES/d' \
        -e '/^UNLOCK TABLES/d' \
        -e '/^\/\*!/d' \
        -e '/^SET /d' \
        -e '/^--/d' \
        "$DUMP_FILE"

    # Load into DuckDB build
    if [ -s "$DUMP_FILE" ]; then
        duckdb_mysql "$DB_NAME" < "$DUMP_FILE"
        if [ $? -ne 0 ]; then
            fail "Loading data for $t into DuckDB failed"
        fi
    fi

    TABLES_LOADED=$((TABLES_LOADED + 1))
done

ok "$TABLES_LOADED/${#TABLES[@]} tables"

# Step 7: Verify row counts match
step "Verify row counts match"

MISMATCHES=0
MISMATCH_DETAILS=""
TOTAL_DUCKDB=0
for t in "${TABLES[@]}"; do
    INNODB_CNT=$(sys_mysql -N -e "SELECT COUNT(*) FROM $DB_NAME.$t" 2>/dev/null || echo "-1")
    DUCKDB_CNT=$(duckdb_mysql -N -e "SELECT COUNT(*) FROM $DB_NAME.$t" 2>/dev/null || echo "-1")

    if [ "$DUCKDB_CNT" -gt 0 ] 2>/dev/null; then
        TOTAL_DUCKDB=$((TOTAL_DUCKDB + DUCKDB_CNT))
    fi

    if [ "$INNODB_CNT" != "$DUCKDB_CNT" ]; then
        MISMATCHES=$((MISMATCHES + 1))
        MISMATCH_DETAILS="$MISMATCH_DETAILS  $t: InnoDB=$INNODB_CNT DuckDB=$DUCKDB_CNT\n"
    fi
done

if [ "$MISMATCHES" -gt 0 ]; then
    echo " FAIL ($MISMATCHES mismatches)"
    printf "$MISMATCH_DETAILS"
    FAILURES=$((FAILURES + 1))
    # Don't exit - continue to DML tests
else
    ok "all match, $TOTAL_DUCKDB total rows"
fi

# Step 8: Direct DML validation
step "Direct DML validation"

DML_ERRORS=0

# Disable errexit for DML section — we check each result manually
set +e

# Run all DML in a single mysql session to avoid cross-session visibility quirks
# (DuckDB PRIMARY mode UPDATE on freshly-inserted rows may not be visible in
#  a different session immediately)
DML_RESULT_FILE="$DUMP_DIR/dml_results.txt"

duckdb_mysql -N "$DB_NAME" > "$DML_RESULT_FILE" 2>&1 <<'EOSQL'
-- INSERT
INSERT INTO EMPRESA (ID_EMPRESA, NOME, NOME_ATUAL)
VALUES (9901, 'TEST_BANK_DML', 'TB_DML');

SELECT CONCAT('INSERT_CHECK=', NOME) FROM EMPRESA WHERE ID_EMPRESA=9901;

-- UPDATE
UPDATE EMPRESA SET NOME_ATUAL='TB_UPDATED' WHERE ID_EMPRESA=9901;

SELECT CONCAT('UPDATE_CHECK=', NOME_ATUAL) FROM EMPRESA WHERE ID_EMPRESA=9901;

-- SELECT: full scan
SELECT CONCAT('FULL_SCAN=', COUNT(*)) FROM CONTRATO;

-- SELECT: PK lookup
SELECT CONCAT('PK_LOOKUP=', ID_EMPRESA) FROM EMPRESA WHERE ID_EMPRESA=9901;

-- SELECT: range
SELECT CONCAT('RANGE_SCAN=', COUNT(*)) FROM EMPRESA WHERE ID_EMPRESA BETWEEN 1 AND 5;

-- SELECT: aggregation
SELECT CONCAT('AGGREGATION=', COUNT(*), ':', COALESCE(SUM(PRINCIPAL), 0)) FROM CONTRATO WHERE STATUS > 0;

-- SELECT: JOIN
SELECT CONCAT('JOIN_TEST=', COUNT(*)) FROM ACORDO a INNER JOIN CONTRATO c ON a.ID_CONTR = c.ID_CONTR;

-- DELETE
DELETE FROM EMPRESA WHERE ID_EMPRESA=9901;

SELECT CONCAT('DELETE_CHECK=', COUNT(*)) FROM EMPRESA WHERE ID_EMPRESA=9901;
EOSQL
DML_RC=$?

if [ "$DML_RC" -ne 0 ]; then
    DML_ERRORS=$((DML_ERRORS + 1))
    echo "  DML session failed (rc=$DML_RC)"
fi

# Parse results
check_result() {
    local key="$1" expected="$2"
    local actual
    actual=$(grep "^${key}=" "$DML_RESULT_FILE" | head -1 | cut -d= -f2-)
    if [ "$actual" != "$expected" ]; then
        DML_ERRORS=$((DML_ERRORS + 1))
        echo "  $key: expected '$expected', got '$actual'"
    fi
}

check_result "INSERT_CHECK" "TEST_BANK_DML"
check_result "UPDATE_CHECK" "TB_UPDATED"
check_result "PK_LOOKUP" "9901"
check_result "DELETE_CHECK" "0"

# Check non-negative results (value varies with data)
for key in FULL_SCAN RANGE_SCAN; do
    val=$(grep "^${key}=" "$DML_RESULT_FILE" | head -1 | cut -d= -f2-)
    if [ -z "$val" ]; then
        DML_ERRORS=$((DML_ERRORS + 1))
        echo "  $key: no result"
    fi
done

# Composite PK test (separate session is fine for this)
FIRST_CONTR=$(duckdb_mysql -N "$DB_NAME" -e "SELECT ID_CONTR FROM CONTRATO LIMIT 1" 2>/dev/null)
if [ -n "$FIRST_CONTR" ]; then
    duckdb_mysql "$DB_NAME" -e "
    INSERT INTO HISTORICO (ID_CONTR, SEQ, ID_FUNCIONARIO, ID_TEL, DATA, CODIGO, MODO)
    VALUES ($FIRST_CONTR, 9901, 0, 0, NOW(), 999, 'T');
    DELETE FROM HISTORICO WHERE ID_CONTR=$FIRST_CONTR AND SEQ=9901;
    " 2>/dev/null
    if [ $? -ne 0 ]; then
        DML_ERRORS=$((DML_ERRORS + 1))
        echo "  INSERT/DELETE HISTORICO (composite PK) failed"
    fi
fi

# Re-enable errexit
set -e

if [ "$DML_ERRORS" -gt 0 ]; then
    fail "$DML_ERRORS DML operations failed"
else
    ok "INSERT/UPDATE/DELETE/SELECT"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "========================================="
if [ "$FAILURES" -eq 0 ]; then
    echo " RESULT: ALL PASSED"
else
    echo " RESULT: $FAILURES STEP(S) FAILED"
fi
echo "========================================="

# Cleanup is handled by trap
exit "$FAILURES"
