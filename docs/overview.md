# MattSQL Overview

MattSQL is a C++20 SQL engine playground that is being shaped toward a
database-oriented operating system. The hosted engine exists first; the custom
runtime/kernel should become another backend behind the same narrow runtime
boundary.

Related docs:

- [Docs index](README.md)
- [Current state](status/current_state.md)
- [Code-level invariants](reference/invariants.md)
- [Runtime C ABI](reference/runtime_abi.md)
- [Long-term plan](../plan.txt)

## Purpose

The long-term question is:

> What can a SQL database do better when it owns the operating environment?

The project should optimize for vertical slices rather than broad SQL or OS
surface area. The most important slice is:

1. Boot or launch.
2. Create table.
3. Insert row.
4. Persist row.
5. Crash.
6. Recover.
7. Select row.

## Current Shape

The hosted SQL pipeline is:

1. `Lexer` tokenizes SQL and preserves source ranges.
2. `Parser` builds one-statement ASTs.
3. `DefaultBinder` resolves names and attaches SQL types.
4. `DefaultLogicalPlanner` builds logical operators.
5. `DefaultOptimizer` applies rule-based logical rewrites.
6. `DefaultPhysicalPlanner` attaches executable access metadata.
7. `DefaultExecutor` runs physical plans against catalog and table storage.

The supported SQL surface is intentionally small: `CREATE TABLE`,
`INSERT ... VALUES`, scalar `SELECT`, table `SELECT`, projection aliases,
qualified column references, `WHERE`, integer arithmetic, comparisons, boolean
operators and literals, string literals, and `NULL`.

The parser and engine execute one SQL statement at a time. The CLI is
responsible for splitting scripts into statements.

## Source Areas

- `include/mattsql/common`: status, result, identifier, type, value, and
  query-result helpers.
- `include/mattsql/lexer` and `include/mattsql/parser`: tokenization and AST
  construction.
- `include/mattsql/binder`: semantic name and type binding.
- `include/mattsql/planner`: logical and physical plan objects.
- `include/mattsql/optimizer`: logical rewrite rules.
- `include/mattsql/execution`: physical execution and scalar expression
  evaluation.
- `include/mattsql/catalog`: table/index metadata.
- `include/mattsql/storage`: tuple encoding, pages, heaps, block devices, and
  pending storage interfaces.
- `include/mattsql/runtime`: hosted runtime and C ABI runtime adapters.
- `include/mattsql/abi`: C-compatible runtime boundary.
- `include/mattsql/txn`: transaction interfaces.

## Boundary Principles

- SQL, planning, execution, catalog, storage, and runtime stay separate.
- The runtime and future kernel remain SQL-agnostic.
- OS interaction belongs behind `PlatformRuntime` or the C ABI.
- Storage receives encoded records and pages, not SQL expression trees.
- Query-facing materialization uses `QueryResult`; hot internal paths should
  prefer tuples, pages, spans, vectors, and batches.
- Cross-module objects must satisfy the contracts in
  [code-level invariants](reference/invariants.md).
- ABI changes must follow the frozen-version rules in
  [runtime C ABI](reference/runtime_abi.md).

## Build And Test

Configure and build the debug preset:

```sh
cmake --preset debug
cmake --build --preset debug
```

Run the debug binary:

```sh
./build/debug/mattsql "SELECT 1 AS one, 'MattSQL' AS project;"
printf "SELECT 42 AS answer;" | ./build/debug/mattsql
```

Run tests:

```sh
ctest --preset debug
```

Normal builds and tests do not download dependencies. If `rustc` is available,
CTest also runs the Rust ABI layout compile check in
`tests/rust/abi_runtime_layout.rs`.

## Deferred Scope

Do not add these early unless they directly support the persistence/recovery
slice:

- full SQL compatibility
- joins and aggregation
- MVCC
- cost-based optimization
- PostgreSQL wire compatibility
- a general-purpose filesystem
- process isolation
- user accounts and permissions
- demand paging
- distributed execution
- JIT compilation

