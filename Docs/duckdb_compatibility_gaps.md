# DuckDB Compatibility Gaps

This document tracks known SQL compatibility gaps when offloading MySQL
SELECTs to DuckDB. These gaps map directly to items in the compatibility
matrix and are used to prioritize rewrite and test work.

## Open Gaps
- Aggregate/window functions: No rewrite mapping yet. Queries must use
  DuckDB-compatible syntax directly.
- Type casting differences: No rewrite mapping yet for MySQL-specific
  casts. Use standard `CAST(.. AS type)` with DuckDB type names.
- LOCATE start-position: `LOCATE(substr, str, start)` is not supported
  (only the 2-argument form is rewritten).
- DATE_FORMAT / STR_TO_DATE tokens: Format token sets differ between
  MySQL and DuckDB; complex patterns may not round-trip.
- Time zone semantics: Functions like `FROM_UNIXTIME` and `NOW()` follow
  DuckDB time zone rules, which may differ from MySQL depending on session
  settings.

## Next Candidates
- Add rewrite support for simple `CAST(expr AS SIGNED)` and `CAST(expr AS CHAR)`.
- Extend LOCATE rewrite to include a start-position using `STRPOS` + `SUBSTR`.
- Add a safe subset of DATE_FORMAT tokens to the compatibility matrix tests.
