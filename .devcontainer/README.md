# MattSQL Dev Container

This dev container provides an Ubuntu 24.04 C++ development environment for
MattSQL while keeping macOS as the editor/workstation host.

## Included Tools

- C++20 build toolchain: GCC, Clang, CMake, Ninja, CTest
- Debugging and analysis: GDB, LLDB, Valgrind, Heaptrack
- Formatting and language tooling: clang-format, clang-tidy, clangd extension
- Optional project checks: Python 3 and Rust via rustup stable

## Open

Open the repository in VS Code or Codex with Dev Containers support and choose
`Reopen in Container`.

The container config runs:

```sh
cmake --preset debug
```

after creation.

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
Linux `perf` is still host-kernel-sensitive. Use a real Ubuntu VM with a native
repo clone for trustworthy `perf` runs and kernel/runtime experiments.
