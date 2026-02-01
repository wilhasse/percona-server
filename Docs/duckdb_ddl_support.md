# DuckDB DDL Support Notes

This document summarizes DDL limitations for the DuckDB secondary engine and
how to handle edge cases safely.

## Supported Patterns
- Basic `CREATE TABLE` for column types + `NOT NULL`.
- Simple `ALTER TABLE` operations (add/drop/rename column) when DuckDB accepts
  the statement.
- `DROP TABLE`, `RENAME TABLE`, and `TRUNCATE TABLE`.

## Unsupported or Risky DDL
These statements are rejected unless a copy DDL fallback (`new_def`) is
provided:
- Column reordering (`FIRST`, `AFTER`).
- Character set/collation clauses (`CHARSET`, `CHARACTER SET`, `COLLATE`).
- Generated columns (`GENERATED`, `VIRTUAL`, `STORED`).
- Partitioning (`PARTITION` / `SUBPARTITION`).
- Foreign keys (`FOREIGN KEY`, `REFERENCES`).
- Secondary indexes (`INDEX`, `KEY`, `UNIQUE`, `FULLTEXT`, `SPATIAL`).

## Copy DDL Fallback
For unsupported DDL, the adapter performs a copy-based rebuild:
1. Create a temp table from `new_def`.
2. Copy rows from the old table into the new layout.
3. Swap tables and drop the old one.

This requires a fully populated `MySQLTableDef` in the DDL change object.
