# DuckDB Mixed-Engine Joins and Offload Defaults

Status: design note for DUCKM-84.

## Mixed-engine joins
- MySQL can execute joins across storage engines (for example, InnoDB +
  DuckDB). Those joins run in the MySQL executor and read rows from each
  engine; they are not accelerated by DuckDB.
- DuckDB offload requires every referenced table to be loaded into DuckDB
  and mapped to the same DuckDB database file (default: per-schema
  `<schema>.duckdb`). If any table is missing or points to a different file,
  the join falls back to MySQL execution even with
  `use_secondary_engine=ON`.
- Partial migration is OK, but any accelerated join must include all
  participating tables (often a large fact table plus small dimensions).

## Offload defaults for tests
- Prefer `use_secondary_engine=ON` with `secondary_engine_cost_threshold=0`
  to encourage offload without forcing it.
- For repeatable tests, this can be applied automatically via `init_connect`:

```
SET PERSIST init_connect='SET SESSION use_secondary_engine=ON; SET SESSION secondary_engine_cost_threshold=0;';
```

## Open questions
- Should we surface mixed-engine join fallback in docs or via a server warning?
- Should the plugin provide a helper/sysvar to set per-session defaults for
  offload behavior?
