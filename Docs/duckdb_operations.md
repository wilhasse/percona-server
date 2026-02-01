# DuckDB Replication Operations

## Pause/Resume and Throttle
- Pause apply: `SET GLOBAL duckdb_binlog_apply_paused = 1;`
- Resume apply: `SET GLOBAL duckdb_binlog_apply_paused = 0;`
- Throttle by rows: `SET GLOBAL duckdb_binlog_apply_throttle_rows_per_sec = 5000;`
- Throttle by bytes: `SET GLOBAL duckdb_binlog_apply_throttle_bytes_per_sec = 10485760;`
- If both throttle limits are set, the applier sleeps to satisfy the slower rate.
- Stop at a GTID for point-in-time recovery:
  `SET GLOBAL duckdb_binlog_apply_stop_at_gtid = 'uuid:123';`

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

## Resume After Stop-at-GTID
- Clear the GTID stop and resume:
  `SET GLOBAL duckdb_binlog_apply_stop_at_gtid = '';`
  `SET GLOBAL duckdb_binlog_apply_paused = 0;`
