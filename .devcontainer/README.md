# MattSQL Dev Container

This dev container provides an Ubuntu 24.04 C++ development environment for
MattSQL while keeping macOS as the editor/workstation host.

## Included Tools

- C++20 build toolchain: GCC, Clang, CMake, Ninja, CTest
- Debugging and analysis: GDB, LLDB, Valgrind, Heaptrack, Linux perf,
  FlameGraph
- Formatting and language tooling: clang-format, clang-tidy, clangd extension
- Optional project checks: Python 3 and Rust via rustup stable

## Prerequisites

- Docker Desktop is installed and running on macOS.
- VS Code or Codex has Dev Containers support enabled.

## Start

1. Open this repository on macOS.
2. Run `Dev Containers: Reopen in Container` from the command palette, or use
   the `Reopen in Container` prompt when it appears.
3. Wait for the image build and post-create setup to finish. The first run
   downloads the Ubuntu base image and Rust stable toolchain.
4. Open a terminal inside the container. The workspace path is
   `/workspaces/MattSQL`.

The container config runs:

```sh
cmake --preset debug
```

after creation, so the debug preset is configured before normal development.

If the Dockerfile or devcontainer config changes, run
`Dev Containers: Rebuild Container` so the local image is refreshed.

## Build And Test

Inside the container:

```sh
cmake --build --preset debug
ctest --preset debug
./build/debug/mattsql "SELECT 1 AS one, 'MattSQL' AS project;"
```

## Build Artifacts

The repository source is mounted from the host, but `build/` is mounted as a
Docker volume. This keeps Linux build outputs separate from macOS build outputs
and avoids slow host-file synchronization for generated files.

Do not use symlinks to share build directories across macOS, containers, and
VMs. Use separate native build directories or this container-managed `build/`
volume.

## Profiling Notes

The container enables `SYS_PTRACE` for debugger/profiler compatibility, but
Linux `perf` is still host-kernel-sensitive. The image includes `perf` and the
FlameGraph scripts so `benchmarks/profile_artifacts.py --perf-flamegraph` can
produce SVGs when Docker Desktop and the host kernel permit sampling. Use a real
Ubuntu VM with a native repo clone for trustworthy `perf` runs and
kernel/runtime experiments.

Inside the container:

```sh
cmake --preset profile
cmake --build --preset profile --target mattsql_benchmarks
python3 benchmarks/profile_artifacts.py \
  --benchmark ./build/profile/mattsql_benchmarks \
  --output-dir build/profile/performance-artifacts \
  --filters engine_select page_heap \
  --perf-flamegraph
```

The helper always writes Perfetto traces and trace-derived flame graphs from
function-level engine spans. If `perf record` is unavailable because the host
kernel is Docker Desktop's LinuxKit kernel, the sampled perf flame graph will be
reported as unavailable while the trace-derived SVG remains usable.
