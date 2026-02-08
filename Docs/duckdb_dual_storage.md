# Dual Storage Architecture (InnoDB + DuckDB)

## Overview
MySQL's Secondary Engine framework enables dual storage between InnoDB
(primary) and DuckDB (secondary) without the handlers calling each other
in normal query execution. MySQL orchestrates eligibility and execution,
while DuckDB provides read-optimized analytics over a per-schema `.duckdb`
database file (default).

## Architecture
```
                    MySQL Server
                         |
         +---------------+---------------+
         |               |               |
         v               v               v
    Query Parser    Optimizer      Executor
                         |               |
                         |    +----------+----------+
                         |    |                     |
                         v    v                     v
                    +---------+             +---------+
                    | InnoDB  |             | DuckDB  |
                    |primary  |             |secondary|
                    +---------+             +---------+
                      t1.ibd               <schema>.duckdb
```

## Key Handlerton Hooks
DuckDB registers as a secondary engine in `storage/duckdb/ha_duckdb.cc`:

```
duckdb_hton->flags = HTON_IS_SECONDARY_ENGINE;
duckdb_hton->prepare_secondary_engine = PrepareSecondaryEngine;
duckdb_hton->optimize_secondary_engine = OptimizeSecondaryEngine;
```

## Query Flow
### Write Path (INSERT/UPDATE/DELETE)
```
Client -> MySQL -> InnoDB only
         (DuckDB not called for user DML)
```
The flow above applies to secondary mode. In primary mode, DuckDB handler DML
paths are used directly for DuckDB tables.
In secondary mode, DuckDB is updated via the binlog applier, not direct SQL
DML.

### Read Path (SELECT)
```
Client
  |
  v
MySQL Parser
  |
  v
Optimizer -------------------------------+
  |                                      |
  | (1) Optimize with InnoDB             |
  v                                      v
PrepareSecondaryEngine()        OptimizeSecondaryEngine()
  |                                      |
  | (2) Eligibility checks               | (3) Validate in DuckDB
  |                                      |     (prepare query)
  +------------------+-------------------+
                     |
                     v
              Cost Comparison
                     |
        +------------+------------+
        |                         |
        v                         v
   Use InnoDB                Use DuckDB
   (standard path)           (external executor)
```

## Code Flow Details
### Step 1: PrepareSecondaryEngine()
Checks that the query is a simple SELECT over one base table, then stores
the DuckDB path and rewritten SQL in the execution context.

### Step 2: OptimizeSecondaryEngine()
Prepares the rewritten SQL in DuckDB to validate it, then wires
`DuckdbExecuteQuery` as the external executor when eligible.

### Step 3: DuckdbExecuteQuery()
Opens the per-schema DuckDB database file, executes the query, and streams rows back
through the MySQL result pipeline.

## Data Sync (SECONDARY_LOAD)
`ALTER TABLE t1 SECONDARY_LOAD` builds the DuckDB copy by scanning the
primary handler and writing rows into the DuckDB table:

```
handler *primary = table.file;  // InnoDB handler
primary->ha_rnd_init(true);
while (primary->ha_rnd_next(record) == 0) {
  appender.BeginRow();
  // copy field values
  appender.EndRow();
}
```

After the initial load, the DuckDB binlog applier keeps the tables inside the
schema-level `.duckdb` database in sync with changes on the primary.

## Multi-table Queries
DuckDB offload can run multi-table SELECTs when all referenced tables resolve
to the same DuckDB database file (default: per-schema `<schema>.duckdb`).
If you override paths with `SECONDARY_ENGINE_ATTRIBUTE`, ensure every table in
the query points to the same file. Cross-schema joins are not yet offloaded.
Only INNER and LEFT joins are currently eligible; NATURAL/USING and semi/anti
joins fall back to the primary engine.

## DuckDB File Location
By default, per-schema DuckDB files live under `@@datadir`. To store them
elsewhere, configure `duckdb_db_dir` at startup (e.g., in `my.cnf`).

## Legacy Per-table Layout
If you need to keep existing per-table DuckDB files, set
`SECONDARY_ENGINE_ATTRIBUTE` to an explicit `.duckdb` path when creating or
altering the table. This forces the handler to use that file instead of the
default per-schema database.

## Key Files
- `storage/duckdb/ha_duckdb.cc` - DuckDB handler and secondary engine hooks
- `storage/duckdb/ha_duckdb.h` - handler class definition
- `storage/duckdb/duckdb_adapter.cc` - DDL/DML application to DuckDB
- `storage/duckdb/duckdb_binlog_applier.cc` - replication apply path
- `sql/handler.h` - MySQL handlerton interfaces
