# DuckDB Compatibility Matrix

This document tracks deterministic SQL rewrites used when offloading
MySQL SELECT statements to DuckDB. The rewrite layer is rule-based and
runs in the server process (no runtime LLM).

## Rewrite Layer Summary
- Backtick identifiers are normalized to double quotes.
- `LIMIT offset, count` is rewritten to `LIMIT count OFFSET offset`.
- Function name mapping is applied where DuckDB has an equivalent.

## Coverage Snapshot (2026-02-01)
- Rules tracked: 11
- Supported: 4
- Partial: 5
- Not supported: 2
- Rewrite coverage (supported + partial): 9/11 = 82%

## Function Compatibility Matrix
Legend: SUPPORTED, PARTIAL (differences), NOT SUPPORTED

### String Functions
- SUPPORTED: `IFNULL(a,b)` -> `COALESCE(a,b)`
- PARTIAL: `LOCATE(substr,str)` -> `STRPOS(str, substr)` (no start-position arg)

### Date/Time Functions
- PARTIAL: `DATE_FORMAT(dt, fmt)` -> `STRFTIME(dt, fmt)` (format tokens may differ)
- PARTIAL: `STR_TO_DATE(str, fmt)` -> `STRPTIME(str, fmt)` (format tokens may differ)
- PARTIAL: `FROM_UNIXTIME(ts)` -> `TO_TIMESTAMP(ts)`
- PARTIAL: `UNIX_TIMESTAMP(dt)` -> `EPOCH(dt)`
- SUPPORTED: `CURDATE()` -> `CURRENT_DATE`
- SUPPORTED: `CURTIME()` -> `CURRENT_TIME`
- SUPPORTED: `NOW()` -> `CURRENT_TIMESTAMP`

### NULL Handling
- SUPPORTED: `IFNULL(a,b)` -> `COALESCE(a,b)`

### Aggregate/Window Functions
- NOT SUPPORTED: Not rewritten yet (use DuckDB-compatible syntax directly).

### Type Casting
- NOT SUPPORTED: Not rewritten yet (use `CAST(.. AS type)` with DuckDB types).

## Notes
- Offload currently requires a simple single-table `SELECT` with direct column
  projections. Complex projections may be added later.
- Unsupported rewrites fall back to the primary engine when
  `use_secondary_engine=ON`, or raise an error if `FORCED`.
- See `Docs/duckdb_compatibility_gaps.md` for current gaps and next steps.
