# DuckDB Secondary Engine Architecture

## Component Diagram

+---------------------------+
| MySQL Parser/Optimizer    |
+-------------+-------------+
              |
              v
+---------------------------+
| Secondary Engine Framework|
| (prepare/optimize/offload)|
+-------------+-------------+
              |
              v
+---------------------------+
| DuckDB SE Plugin          |
| - Offload coordinator     |
| - Compatibility rewrite   |
| - Result streaming        |
+-------------+-------------+
              |
              v
+---------------------------+
| DuckDB Embedded Database  |
| (per-schema .duckdb files)|
+---------------------------+

See `Docs/duckdb_dual_storage.md` for a dual-storage overview and query flow.

Replication path:
+---------------------------+
| Binlog stream             |
+-------------+-------------+
              |
              v
+---------------------------+
| DuckDB Binlog Applier     |
| - micro-batching          |
| - watermark update        |
+-------------+-------------+
              |
              v
+---------------------------+
| DuckDB Adapter            |
| (DDL/DML, transactions)   |
+-------------+-------------+
              |
              v
+---------------------------+
| DuckDB Embedded Database  |
+---------------------------+

## Component Boundaries
- Secondary Engine Framework: MySQL core handles eligibility, table opening,
  and lifecycle hooks for secondary engines.
- DuckDB SE Plugin: own offload eligibility rules, rewrite layer, execution
  and streaming back to MySQL protocol.
- DuckDB Adapter: authoritative API for DDL/DML and apply transactions.
- Binlog Applier: batching and ordering for replication apply.
- DuckDB Embedded DB: per-schema storage files (default), read-only for
  offload path.

## Dependency Graph
- Offload coordinator -> Compatibility rewrite
- Offload coordinator -> DuckDB Adapter (prepare/execute)
- Binlog applier -> DuckDB Adapter
- DuckDB Adapter -> Embedded DuckDB
- MySQL framework -> DuckDB SE Plugin

## Risks and Mitigations
- SQL incompatibilities: start strict, add deterministic rewrites, maintain
  compatibility matrix, and fall back to primary engine when unsupported.
- DDL unsupported by DuckDB: use copy DDL fallback; guard with schema
  validation and swap/rollback on failure.
  See `Docs/duckdb_ddl_support.md` for the unsupported DDL list.
- Crash consistency: watermark table updated in same DuckDB transaction as
  applied changes; verify on restart.
- Performance regressions: micro-batching for apply, per-schema DB files,
  and cost threshold gating for offload.
- Type mapping drift: keep adapter mapping centralized and add tests for
  common types (NULL handling, binary, JSON, etc.). See
  `Docs/duckdb_type_mapping.md` for the current mapping table.
- Operational controls: pause/throttle/lag metrics and backfill workflow are
  documented in `Docs/duckdb_operations.md`.

## Milestone Plan
- M1: Read-only secondary engine and per-schema DuckDB files (default; legacy
  per-table files via SECONDARY_ENGINE_ATTRIBUTE).
- M2: Binlog applier + micro-batching + watermarks (done).
- M3: DDL support with copy DDL fallback (done).
- M4: Query offload + deterministic compatibility rewrites (done).
- M5: Wire DDL binlog events into applier; broaden compatibility coverage.
- M6: Cost model tuning, observability, and performance benchmarks.

## Interface Governance
- The YAML in Docs/duckdb_interfaces.yaml is the authoritative interface
  contract. Any agent changing interfaces must update that file and add
  a compatibility note if behavior changes.
