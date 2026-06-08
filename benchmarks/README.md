# MattSQL Benchmarks

MattSQL treats performance as a tracked behavior. The benchmark executable is
dependency-free so it can run anywhere the normal CMake build runs, and it is
usable as a target under external profilers.

## Build

```sh
cmake --preset profile
cmake --build --preset profile --target mattsql_benchmarks
```

## Run

```sh
./build/profile/mattsql_benchmarks
./build/profile/mattsql_benchmarks --json
./build/profile/mattsql_benchmarks --filter engine_select --iterations 1000
./build/profile/mattsql_benchmarks --filter frontend --iterations 1 --trace-json frontend.trace.json
```

CTest runs the same executable against the committed baseline:

```sh
ctest --preset profile -R mattsql.performance
```

The baseline file is `benchmarks/baseline.tsv`. It tracks the prior median
nanoseconds per benchmark iteration plus an allowed regression ratio. The
default ratios are conservative because local wall-clock performance varies by
host, thermal state, VM settings, and build type. Tighten them after choosing a
stable performance runner.

## Update Baselines

Use a profile build on a quiet, stable machine:

```sh
./build/profile/mattsql_benchmarks --check-baseline benchmarks/baseline.tsv
```

Then update `benchmarks/baseline.tsv` with the printed median values for any
benchmarks whose new behavior should become the comparison point.

## Visual Reports

Generate the repo-local performance dashboard:

```sh
cmake --build --preset profile --target mattsql_performance_report
```

The target writes these files under `build/profile/performance-report/`:

- `index.html`: current-vs-baseline bars, subsystem ratios, and per-benchmark
  trend sparklines.
- `current_vs_baseline.md`: Markdown summary suitable for a PR comment.
- `current.json`: the benchmark run plus commit, branch, timestamp, and dirty
  worktree metadata.

The report target appends each run to `build/profile/performance-history.jsonl`.
That history file is local build output; keep long-term trend data in CI or a
dedicated perf runner rather than committing machine-specific local results.

You can run the script directly when CI already produced benchmark JSON:

```sh
python3 benchmarks/perf_visualize.py \
  --current-json benchmark-results.json \
  --baseline benchmarks/baseline.tsv \
  --output-dir build/profile/performance-report
```

## Grafana Metrics

GitHub Actions publishes commit-tagged benchmark metrics to Grafana Cloud when
the repository has OTLP secrets configured. The same payload can be generated
locally from benchmark JSON:

```sh
python3 benchmarks/grafana_publish.py \
  --current-json build/profile/benchmark-results.json \
  --baseline benchmarks/baseline.tsv \
  --output-json build/profile/performance-report/grafana-otlp-metrics.json
```

See [docs/profiling.md](../docs/profiling.md) for the GitHub Actions workflow,
Grafana Cloud secret names, and dashboard import path.

## Profiler Targets

Use the benchmark filter to make profiler sessions long and focused:

```sh
./build/profile/mattsql_benchmarks --filter frontend --iterations 2000
./build/profile/mattsql_benchmarks --filter engine_select --iterations 1000
./build/profile/mattsql_benchmarks --filter tuple_codec --iterations 2000
./build/profile/mattsql_benchmarks --filter page_heap --iterations 1000
```

Useful external profilers:

- macOS Instruments Time Profiler or Allocations against
  `build/profile/mattsql_benchmarks`.
- Linux `perf record -g -- ./build/profile/mattsql_benchmarks --filter ...`.
- Linux `heaptrack ./build/profile/mattsql_benchmarks --filter ...`.
- QEMU/KVM custom-kernel runs should launch a focused benchmark payload and
  collect host-side samples with `perf kvm` once the kernel can boot MattSQL.

## Trace And Flame Graph Artifacts

Generate Perfetto-compatible trace files for selected benchmark families:

```sh
cmake --build --preset profile --target mattsql_perfetto_traces
```

The target writes `*.trace.json` files under
`build/profile/performance-artifacts/`. Open those files in Perfetto UI or
Chrome's trace viewer. The trace contains complete events for benchmark samples
plus function-level spans such as `mattsql::Lexer::Tokenize`,
`mattsql::DefaultSqlEngine::Execute`, `mattsql::BinaryTupleCodec::Encode`,
`mattsql::DefaultSlottedPage::Insert`, `mattsql::PageHeapTable::Insert`, and
`mattsql::PageHeapTable::Cursor::Next`.

When `flamegraph.pl` is available, `profile_artifacts.py` also converts those
function-level trace events into folded stacks and writes
`*.trace-flamegraph.svg`. This SVG is based on traced function durations, so it
works in Docker Desktop/dev containers even when kernel perf sampling is
unavailable.

On Linux, generate perf flame graph artifacts when `perf`,
`stackcollapse-perf.pl`, and `flamegraph.pl` are available:

```sh
python3 benchmarks/profile_artifacts.py \
  --benchmark ./build/profile/mattsql_benchmarks \
  --output-dir build/profile/performance-artifacts \
  --filters engine_select page_heap \
  --perf-flamegraph
```

The helper always writes trace artifacts. Flame graph SVGs are added when the
Linux profiling tools are present and the host permits `perf record`. In the
dev container, Docker Desktop may reject `perf record` because the LinuxKit host
kernel does not match Ubuntu's `linux-tools-*` packages; use the
trace-derived flame graph in that case, or run the same command in a native
Ubuntu VM for a sampled CPU flame graph.
