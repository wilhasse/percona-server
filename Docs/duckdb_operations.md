# DuckDB Replication Operations

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
