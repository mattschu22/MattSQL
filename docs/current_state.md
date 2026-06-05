# MattSQL Current State

MattSQL is a C++20 SQL engine playground with a hosted in-memory execution
path and an emerging runtime boundary for database-oriented operating-system
experiments.

## Implemented Pipeline

The primary query path is:

1. `Lexer` tokenizes SQL text and preserves source ranges for diagnostics.
2. `Parser` builds lightweight AST nodes for one statement at a time.
3. `DefaultBinder` resolves names against the catalog and attaches SQL types.
4. `DefaultLogicalPlanner` lowers bound statements into logical operators.
5. `DefaultOptimizer` applies rule-based logical rewrites.
6. `DefaultPhysicalPlanner` creates executable physical operators.
7. `DefaultExecutor` runs physical plans against catalog and table storage.

The supported SQL surface is intentionally small: `CREATE TABLE`,
`INSERT ... VALUES`, scalar `SELECT`, table `SELECT`, projection aliases,
qualified column references, `WHERE`, integer arithmetic, comparison operators,
boolean operators, boolean literals, string literals, and `NULL`.

## Storage And Catalog

Implemented storage pieces include:

- in-memory catalog metadata
- hosted catalog API abstraction
- in-memory table storage manager
- binary tuple codec
- slotted pages
- page-backed heap table handles
- memory block device for aligned block I/O tests

The B-tree index, buffer pool, WAL manager, and transaction manager are currently
interfaces. Their headers define expected ownership boundaries, but no durable
or concurrent implementation is present yet.

## Runtime Boundary

`PlatformRuntime` is the runtime abstraction used by the engine. The hosted
runtime supports runtime-managed page spans, synchronous block-device-backed
batch I/O, completion polling, cooperative yield, monotonic time, logging, and
panic.

The C ABI runtime adapter exposes the same MVP runtime concepts through
`include/mattsql/abi/runtime.h`. ABI version 1 is documented in
`docs/runtime_abi.md` and is covered by C++ layout tests plus a Rust layout
compile check when `rustc` is available.

## Test Coverage

The project uses CTest plus a local test harness in `tests/test_framework.hpp`.
Coverage includes lexer, parser, binder, planner, optimizer, executor, catalog,
storage, runtime, C ABI layout, C ABI adapter behavior, SQL logic scripts, and a
deterministic SQL logic fuzzer.

The SQL logic runner and fuzzer live in `tests/sql_logic_support.cpp`; the test
registration file is intentionally small.

## Current Gaps

- Parser and engine execution still accept one statement at a time; the CLI
  splits scripts before dispatching statements.
- There is no durable heap storage path yet.
- Buffer pool, WAL, B-tree, and transaction manager implementations are pending.
- Runtime task scheduling is still a hosted placeholder.
- The runtime C ABI intentionally excludes SQL, catalog, storage format, timers,
  filesystems, networking, and scheduler policy in version 1.
