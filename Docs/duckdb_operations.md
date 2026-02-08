# DuckDB Replication Operations

## Mode Selection and Fallback
- Secondary mode (default): queries may offload to DuckDB based on eligibility
  and cost.
  - `SET SESSION use_secondary_engine=ON;`
  - `SET SESSION secondary_engine_cost_threshold=0;` (aggressive offload for QA)
- Forced mode: only for validation; unsupported/offload-ineligible statements
  fail instead of falling back.
  - `SET SESSION use_secondary_engine=FORCED;`
- Primary mode: DuckDB handler serves table I/O directly (no secondary offload
  decision path for those tables).

Fallback behavior:
- In `ON`, unsupported rewrites or missing DuckDB table mappings fall back to
  MySQL execution.
- In `FORCED`, the same conditions return an error and surface the offload
  failure reason.

## Pause/Resume and Throttle
- Pause apply: `SET GLOBAL duckdb_binlog_apply_paused = 1;`
- Resume apply: `SET GLOBAL duckdb_binlog_apply_paused = 0;`
- Throttle by rows: `SET GLOBAL duckdb_binlog_apply_throttle_rows_per_sec = 5000;`
- Throttle by bytes: `SET GLOBAL duckdb_binlog_apply_throttle_bytes_per_sec = 10485760;`
- If both throttle limits are set, the applier sleeps to satisfy the slower rate.
- Stop at a GTID for point-in-time recovery:
  `SET GLOBAL duckdb_binlog_apply_stop_at_gtid = 'uuid:123';`

## Batch Apply Settings (GTID Mode)
Batching commits multiple GTIDs in a single DuckDB transaction to improve
throughput. These values are read when the apply thread starts, so restart the
thread (or mysqld) after changing them.

- Max GTIDs per commit (default 100):
  `SET GLOBAL duckdb_binlog_apply_batch_gtids = 200;`
- Max delay before forcing a commit (default 200 ms):
  `SET GLOBAL duckdb_binlog_apply_batch_delay_ms = 500;`
- Optional row/byte caps (0 disables):
  `SET GLOBAL duckdb_binlog_apply_batch_rows = 50000;`
  `SET GLOBAL duckdb_binlog_apply_batch_bytes = 67108864;`

To apply new batch settings without restart:
```
SET GLOBAL duckdb_binlog_apply_enabled = OFF;
SET GLOBAL duckdb_binlog_apply_enabled = ON;
```

## Debug Logging
- Enable verbose apply logging (default OFF):
  `SET GLOBAL duckdb_binlog_apply_verbose = ON;`

## Monitoring and Lag
- Inspect metrics: `SHOW STATUS LIKE 'duckdb_binlog_%';`
- Key fields:
  - `duckdb_binlog_applied_transactions`, `duckdb_binlog_applied_rows`,
    `duckdb_binlog_applied_bytes`
  - `duckdb_binlog_last_flush_rows`, `duckdb_binlog_last_flush_bytes`,
    `duckdb_binlog_last_flush_ms`
  - `duckdb_binlog_last_commit_epoch_ms`, `duckdb_binlog_lag_ms`,
    `duckdb_binlog_lag_alert`
  - `duckdb_binlog_last_gtid`
- Configure lag alerting: `SET GLOBAL duckdb_binlog_apply_lag_alert_ms = 5000;`
- Lag is measured as time since the last DuckDB apply commit (local time),
  not source commit time.

## Backfill Workflow
- Snapshot primary into DuckDB with secondary load:
  `ALTER TABLE db.tbl SECONDARY_LOAD;`
- Use `SECONDARY_UNLOAD` to discard the DuckDB copy before reloading.
- After backfill, the binlog applier keeps the DuckDB table in sync.

## DuckDB File Location
- Default per-schema files live under `@@datadir`.
- Override the base directory at startup with `duckdb_db_dir=/path`.

## Known Limitations
- Offload requires all referenced tables to resolve to the same DuckDB file.
- Secondary-mode tables are read-only from SQL DML; changes arrive via
  `SECONDARY_LOAD` + binlog apply.
- Some MySQL function/format-token rewrites remain partial; see
  `Docs/duckdb_compatibility.md` and `Docs/duckdb_compatibility_gaps.md`.
- Cross-schema DuckDB table rename in primary mode is not supported by the
  current handler path.

## Resume After Stop-at-GTID
- Clear the GTID stop and resume:
  `SET GLOBAL duckdb_binlog_apply_stop_at_gtid = '';`
  `SET GLOBAL duckdb_binlog_apply_paused = 0;`

## Resync and State Recovery
- Inspect current replication state:
  `duckdb_snapshot_loader --state-only --duckdb-path /path/db.duckdb --show-state`
- Reset applied GTID state (force replay) and drop legacy watermark:
  `duckdb_snapshot_loader --state-only --duckdb-path /path/db.duckdb --reset-applied-gtid-set --drop-watermark`
- Force a specific applied GTID set:
  `duckdb_snapshot_loader --state-only --duckdb-path /path/db.duckdb --set-applied-gtid-set 'uuid:1-123'`
- Reload a single table during recovery:
  `duckdb_snapshot_loader --schema db --table tbl --overwrite --duckdb-dir /var/lib/mysql/duckdb`
