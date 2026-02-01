# DuckDB Type Mapping

This document captures the MySQL → DuckDB type mapping rules used when the
DuckDB secondary engine creates or rebuilds tables.

## Numeric Types
- `TINYINT` → `TINYINT` / `UTINYINT`
- `SMALLINT` → `SMALLINT` / `USMALLINT`
- `MEDIUMINT`, `INT`, `INTEGER` → `INTEGER` / `UINTEGER`
- `BIGINT` → `BIGINT` / `UBIGINT`
- `FLOAT` / `REAL` → `FLOAT`
- `DOUBLE` → `DOUBLE`
- `DECIMAL(p,s)` / `NUMERIC(p,s)` → `DECIMAL(p,s)` (precision > 38 is rejected
  by the adapter and stored as `VARCHAR` by the handler with a warning)

## Temporal Types
- `DATE` → `DATE`
- `TIME` → `TIME`
- `DATETIME` → `TIMESTAMP`
- `TIMESTAMP` → `TIMESTAMP` (warning: timezone semantics may differ)
- `YEAR` → `SMALLINT` (warning)

## String & Binary Types
- `CHAR`, `VARCHAR` → `VARCHAR`
- `TEXT` variants → `VARCHAR`
- `BLOB` variants → `BLOB`
- `BIT` → `BLOB` (lossy; warning)

## Complex/Unsupported Types
- `JSON` → `BLOB` (lossy; warning; JSON semantics not preserved)
- `ENUM`, `SET` → `VARCHAR` (lossy; warning)
- `GEOMETRY` → `BLOB` (lossy; warning)

## Behavior Notes
- The handler emits warnings for lossy mappings when creating/loading DuckDB
  tables.
- The adapter fails fast on lossy types when building tables from
  `MySQLTableDef` (copy DDL / rebuilds), to avoid silent corruption.
