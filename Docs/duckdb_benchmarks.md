# DuckDB Benchmark Harness

This document describes the benchmark tooling used to measure DuckDB
secondary-engine performance. The harness is designed to be reproducible
and generates CSV output suitable for dashboards.

## Build
```
cmake --build build --target duckdb_bench
```

## Run (default)
```
scripts/duckdb_bench/run_duckdb_bench.sh
```

The script writes:
- CSV: `build/duckdb_bench/results.csv`
- Markdown summary: `build/duckdb_bench/results.md`

## Parameters
Environment overrides:
- `DUCKDB_BENCH_ROWS` (default: 100000)
- `DUCKDB_BENCH_BATCH` (default: 1000)
- `DUCKDB_BENCH_QUERY_ITERS` (default: 5)

Example:
```
DUCKDB_BENCH_ROWS=500000 DUCKDB_BENCH_BATCH=5000 \
  scripts/duckdb_bench/run_duckdb_bench.sh
```

## Benchmarks Included
- `sql_insert`: baseline INSERT statements (per-batch multi-values).
- `appender`: bulk ingestion via `DuckDBAdapter::AppendRows`.
- `binlog_applier`: transaction-aware micro-batching via `DuckDBBinlogApplier`.
- `query_count`: simple COUNT query latency over a populated table.

The CSV provides before/after numbers within a single run by comparing
`sql_insert` (baseline) against `appender` and `binlog_applier`.

## Reproducibility Notes
- The harness creates a temporary DuckDB file and removes it after the run.
- Use consistent `rows`/`batch` values and an idle machine for stable numbers.
- For regressions, store CSV files in your performance dashboard or attach
  them to the DUCKM issue thread.
