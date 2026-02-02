# DuckDB Compatibility Matrix

This document tracks deterministic SQL rewrites used when offloading
MySQL SELECT statements to DuckDB. The rewrite layer is rule-based and
runs in the server process (no runtime LLM).

## Rewrite Layer Summary
- Backtick identifiers are normalized to double quotes.
- `LIMIT offset, count` is rewritten to `LIMIT count OFFSET offset`.
- Function name mapping is applied where DuckDB has an equivalent.

## Coverage Snapshot (2026-02-02)
- Rules tracked: 15
- Supported: 8
- Partial: 5
- Not supported: 2
- Rewrite coverage (supported + partial): 13/15 = 87%

## Function Compatibility Matrix
Legend: SUPPORTED, PARTIAL (differences), NOT SUPPORTED

### String Functions
- SUPPORTED: `IFNULL(a,b)` -> `COALESCE(a,b)`
- SUPPORTED: `LCASE(str)` -> `LOWER(str)`
- SUPPORTED: `UCASE(str)` -> `UPPER(str)`
- SUPPORTED: `MID(str, pos, len)` -> `SUBSTR(str, pos, len)`
- PARTIAL: `LOCATE(substr,str)` -> `STRPOS(str, substr)` (no start-position arg)

### Conditional
- SUPPORTED: `IF(expr, a, b)` -> `CASE WHEN expr THEN a ELSE b END`

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
- Offload currently requires a simple single-table `SELECT` (no subqueries or
  UNIONs). The SELECT list may include expressions, functions, and aggregates
  that DuckDB can execute; unsupported expressions fall back.
- Unsupported rewrites fall back to the primary engine when
  `use_secondary_engine=ON`, or raise an error if `FORCED`.
- See `Docs/duckdb_compatibility_gaps.md` for current gaps and next steps.
