# DuckDB Performance Notes

This document summarizes expected bottlenecks and tuning levers for
DuckDB secondary-engine workloads.

## Bottleneck Checklist
- **Row-by-row INSERTs**: Slow baseline; use `DuckDBAdapter::AppendRows`.
- **Small transactions**: Higher overhead; increase batch size or use
  binlog applier micro-batching.
- **Write amplification**: Frequent schema changes can trigger Copy DDL
  and table rewrites; avoid repeated ALTERs in tight loops.
- **Offload eligibility**: Queries must be simple single-table SELECTs
  with direct column projection; unsupported queries fall back.
- **Disk contention**: Per-table DuckDB files can amplify IO when many
  tables are loaded at once.

## Tuning Knobs
- `DuckDBBinlogApplier::Options`:
  - `max_rows` (default 50k)
  - `max_bytes` (default 64 MB)
  - `max_delay` (default 200 ms)
- Session variables for offload:
  - `@@session.use_secondary_engine`
  - `@@session.secondary_engine_cost_threshold`

## Recommendations
- Start with `batch=1000` and scale up while monitoring latency.
- Use `binlog_applier` benchmark mode to measure apply throughput.
- Capture CSV results after each major change and compare `rows/sec`.
