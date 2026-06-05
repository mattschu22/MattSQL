# MattSQL

MattSQL is a small C++ SQL engine playground.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
```

The debug binary is written to `build/debug/mattsql`.

## Run

```sh
./build/debug/mattsql "SELECT 1 AS one, 'MattSQL' AS project;"
```

You can also pipe SQL through standard input:

```sh
printf "SELECT 42 AS answer;" | ./build/debug/mattsql
```

## Test

```sh
ctest --preset debug
```

Tests use CTest and a tiny local test harness in `tests/test_framework.hpp`, so
the project has no dependency downloads.

## Docs

Start with [docs/README.md](docs/README.md) for the overview, current
implementation state, code-level invariants, and runtime ABI contract.

## Debug

Open the folder in VS Code and use the `Debug MattSQL` launch configuration. It
builds the debug preset first, then starts LLDB against `build/debug/mattsql`.

The workspace recommends the CodeLLDB extension because it provides the `lldb`
debug adapter used by `.vscode/launch.json`.
