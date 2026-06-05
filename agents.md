# MattSQL Agent Context

This file gives agents immediate project context. For fuller documentation,
start with [docs/README.md](docs/README.md).

## Project Direction

MattSQL is a hosted-first C++20 SQL engine that is being developed toward a
database-oriented operating system. The custom runtime/kernel should remain a
backend behind a narrow runtime boundary, not a place where SQL concepts leak.

Primary vertical slice:

1. Launch or boot.
2. Create table.
3. Insert row.
4. Persist row.
5. Crash.
6. Recover.
7. Select row.

## Current Pipeline

`Lexer` -> `Parser` -> `DefaultBinder` -> `DefaultLogicalPlanner` ->
`DefaultOptimizer` -> `DefaultPhysicalPlanner` -> `DefaultExecutor`.

The engine executes one SQL statement at a time. Script splitting belongs to the
CLI or a future script layer.

## Core Rules

- Keep OS interaction behind `PlatformRuntime` or the C ABI.
- Keep runtime/kernel APIs SQL-agnostic.
- Use `Status` / `Result<T>` across module boundaries.
- Preserve the code-level contracts in
  [docs/reference/invariants.md](docs/reference/invariants.md).
- Preserve ABI versioning rules in
  [docs/reference/runtime_abi.md](docs/reference/runtime_abi.md).
- Prefer existing helper APIs such as identifier folding, value/status helpers,
  expression utilities, plan child validation, and little-endian byte helpers.
- Add focused tests near the changed layer when behavior changes.

## Build And Test

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Normal builds and tests do not require dependency downloads.

