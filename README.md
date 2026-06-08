# MattSQL

MattSQL is a small C++ SQL engine playground.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
```

The debug binary is written to `build/debug/mattsql`.

## Dev Container

For Ubuntu-based development from macOS, use the
[dev container](.devcontainer/README.md). It keeps Linux build artifacts in a
Docker volume and provides the C++, CMake, debugger, Python, and Rust tools used
by the normal presets.

To start it, run Docker Desktop, open this repository in VS Code or Codex, and
choose `Dev Containers: Reopen in Container`. The first start builds the Ubuntu
image and runs `cmake --preset debug`.

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

## Benchmark

```sh
cmake --preset profile
cmake --build --preset profile --target mattsql_benchmarks
ctest --preset profile -R mattsql.performance
```

Benchmarks live in `benchmarks/` and compare current profile-build results
against `benchmarks/baseline.tsv`. The benchmark executable is also the target
to run under Instruments, `perf`, `heaptrack`, or a VM/hypervisor profiler.

Generate static performance visualizations with:

```sh
cmake --build --preset profile --target mattsql_performance_report
cmake --build --preset profile --target mattsql_perfetto_traces
```

The report target writes current-vs-baseline and trend views under
`build/profile/performance-report/`. The trace target writes
Perfetto-compatible timeline artifacts under `build/profile/performance-artifacts/`.

## Docs

Start with [docs/README.md](docs/README.md) for the overview, current
implementation state, code-level invariants, and runtime ABI contract.

## Debug

Open the folder in VS Code and use the `Debug MattSQL` launch configuration. It
builds the debug preset first, then starts LLDB against `build/debug/mattsql`.

The workspace recommends the CodeLLDB extension because it provides the `lldb`
debug adapter used by `.vscode/launch.json`.
