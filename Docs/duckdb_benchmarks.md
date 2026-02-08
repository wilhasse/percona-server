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

## Analytical Query Suite
The query suite runs a small set of JOIN/aggregate queries twice:
baseline (InnoDB) and DuckDB offload. It compares result hashes and
captures wall-clock latency per query.

Setup + run:
```
DUCKDB_QUERY_SETUP=1 \
  scripts/duckdb_bench/run_duckdb_query_suite.sh
```

Outputs:
- CSV: `build/duckdb_bench/query_suite/results.csv`
- Markdown summary: `build/duckdb_bench/query_suite/results.md`

Environment overrides:
- `DUCKDB_BENCH_DB` (default: `duckdb_bench`)
- `DUCKDB_QUERY_ITERS` (default: 3)
- `DUCKDB_QUERY_CUSTOMERS`, `DUCKDB_QUERY_ORDERS`, `DUCKDB_QUERY_LINEITEMS`
- `DUCKDB_QUERY_BASELINE_MODE` (default: `OFF`)
- `DUCKDB_QUERY_DUCKDB_MODE` (default: `FORCED`)
- `MYSQL_HOST`, `MYSQL_PORT`, `MYSQL_SOCKET`, `MYSQL_USER`, `MYSQL_PASSWORD`

## Go/No-Go Thresholds (Dual-Mode Rollout)
Use the query suite and compare DuckDB mode against the InnoDB baseline on the
same dataset.

- Pass gate A (correctness): every query result hash must match baseline.
- Pass gate B (performance): median DuckDB latency must be <= baseline for at
  least 70% of benchmarked analytical queries.
- Pass gate C (regression guard): no single analytical query may regress by
  more than 20% vs baseline unless explicitly waived in the ticket notes.
- Pass gate D (stability): 3 consecutive runs must satisfy A/B/C.

Suggested workflow:
1. Run baseline + DuckDB suite 3 times.
2. Publish `results.csv` and `results.md` as ticket artifacts.
3. Record pass/fail for gates A/B/C/D before enabling `FORCED` in staging.

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
