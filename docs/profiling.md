# MattSQL Profiling

MattSQL treats performance as a feature. The current profiling framework gives
the hosted SQL engine a repeatable benchmark target, baseline checks, static
performance reports, Perfetto-compatible function traces, and flame graph
artifacts for both native Linux and dev-container workflows.

Related docs:

- [Docs index](README.md)
- [Overview](overview.md)
- [Benchmarks](../benchmarks/README.md)
- [Dev container](../.devcontainer/README.md)

## Goals

- Keep performance tests in the normal CMake/CTest workflow.
- Make profiler runs focused and reproducible through benchmark filters.
- Preserve historical comparison points in `benchmarks/baseline.tsv`.
- Generate artifacts that are useful on macOS, native Linux, dev containers,
  and future custom-kernel runs.
- Keep runtime/kernel profiling SQL-agnostic; SQL concepts must not leak below
  `PlatformRuntime` or the C ABI boundary.

## Entry Points

Build the profile configuration and benchmark executable:

```sh
cmake --preset profile
cmake --build --preset profile --target mattsql_benchmarks
```

Run the performance check used by CTest:

```sh
ctest --preset profile -R mattsql.performance
```

Generate static performance reports:

```sh
cmake --build --preset profile --target mattsql_performance_report
```

Generate trace and flame graph artifacts:

```sh
cmake --build --preset profile --target mattsql_perfetto_traces
```

Generate a Grafana OTLP metrics payload from an existing benchmark run:

```sh
./build/profile/mattsql_benchmarks --json > build/profile/benchmark-results.json
python3 benchmarks/grafana_publish.py \
  --current-json build/profile/benchmark-results.json \
  --baseline benchmarks/baseline.tsv \
  --output-json build/profile/performance-report/grafana-otlp-metrics.json
```

The benchmark executable supports filters so profiler sessions can spend time
inside one subsystem instead of cycling through every benchmark:

```sh
./build/profile/mattsql_benchmarks --filter frontend --iterations 2000
./build/profile/mattsql_benchmarks --filter engine_select --iterations 1000
./build/profile/mattsql_benchmarks --filter tuple_codec --iterations 2000
./build/profile/mattsql_benchmarks --filter page_heap --iterations 1000
```

## Baseline Checks

`benchmarks/baseline.tsv` records the current accepted median nanoseconds per
benchmark iteration and a tolerated regression ratio. The CTest performance
test runs the profile-build benchmark executable against that file.

Update baselines only from a quiet, stable runner:

```sh
./build/profile/mattsql_benchmarks --check-baseline benchmarks/baseline.tsv
```

The printed median values are the candidates to copy into
`benchmarks/baseline.tsv` when a performance change should become the new
comparison point. Avoid updating baselines from thermally constrained laptops,
loaded machines, or different virtualization settings.

## Perfetto Traces

Passing `--trace-json` to `mattsql_benchmarks` writes Chrome/Perfetto complete
events. The trace contains benchmark iteration events plus function-level spans
emitted by the SQL engine through `mattsql::ScopedTrace`.

Current function span categories are:

- `function.frontend`: lexer, parser, binder, logical planner, optimizer, and
  physical planner entry points.
- `function.engine`: top-level SQL engine execution calls.
- `function.execution`: physical plan execution and scalar expression
  evaluation.
- `function.storage`: tuple codec, slotted page, heap table, and in-memory
  table storage calls.
- `function.runtime`: hosted runtime and platform runtime calls.

Benchmark-owned context spans use `benchmark.iteration` and `benchmark.batch`.
Reserve `function.*` categories for code-level scopes inside the engine.

Open `build/profile/performance-artifacts/*.trace.json` in Perfetto UI or
Chrome's trace viewer.

## Flame Graphs

`benchmarks/profile_artifacts.py` always collects Perfetto traces. It also
converts trace complete events into folded stacks and, when `flamegraph.pl` is
available, writes `*.trace-flamegraph.svg`.

That trace-derived flame graph is based on explicit function durations, not CPU
sampling. It is useful in Docker Desktop and other environments where kernel
sampling is unavailable, but it only shows spans that MattSQL instruments.

On native Linux, or in a container/VM where host kernel sampling works, request
sampled perf artifacts as well:

```sh
python3 benchmarks/profile_artifacts.py \
  --benchmark ./build/profile/mattsql_benchmarks \
  --output-dir build/profile/performance-artifacts \
  --filters engine_select page_heap \
  --perf-flamegraph
```

Sampled perf flame graphs and trace-derived flame graphs answer different
questions:

- Use the trace-derived SVG to compare MattSQL function phases and explain
  high-level engine structure.
- Use sampled `perf` flame graphs to find CPU hotspots inside instrumented and
  uninstrumented code, standard library calls, allocator paths, and runtime
  overhead.

## Dev Container

The dev container installs Linux perf packages and Brendan Gregg's FlameGraph
scripts. It is enough to produce trace-derived flame graphs and may produce
sampled perf flame graphs when Docker Desktop and the host kernel allow it.

Docker Desktop on macOS commonly exposes a LinuxKit kernel that does not match
Ubuntu's `linux-tools-*` packages. In that case `perf record` fails, but the
trace-derived flame graph remains usable. Run the same workflow in a native
Ubuntu VM or bare-metal Linux environment when trustworthy sampled CPU profiles
are required.

## Grafana Cloud Pipeline

The GitHub Actions workflow in `.github/workflows/performance.yml` runs the
profile benchmark suite for branch pushes, pull requests, and manual
dispatches. It uploads the raw benchmark JSON, static performance report, and
Grafana OTLP payload as workflow artifacts.

The publisher uses Grafana Cloud's
[OTLP endpoint](https://grafana.com/docs/grafana-cloud/send-data/otlp/send-data-otlp/)
and sends OTLP/HTTP JSON metrics to the standard
[`/v1/metrics` path](https://opentelemetry.io/docs/specs/otlp/#otlphttp).

For pushes and manual dispatches, the workflow publishes metrics to Grafana
Cloud when repository secrets are configured. Pull requests intentionally do not
publish metrics because forked PRs do not receive secrets and because PR runs
can be noisier than the main-branch trend.

Configure one of these secret sets:

- `GRAFANA_OTLP_ENDPOINT`: the Grafana Cloud OTLP endpoint, such as
  `https://otlp-gateway-prod-us-east-0.grafana.net/otlp`.
- `GRAFANA_OTLP_HEADERS`: OTLP headers copied from Grafana Cloud, such as
  `Authorization=Basic%20...`.

Or:

- `GRAFANA_OTLP_ENDPOINT`: the Grafana Cloud OTLP endpoint.
- `GRAFANA_INSTANCE_ID`: the Grafana Cloud metrics or OTLP instance ID.
- `GRAFANA_API_TOKEN`: a token with permission to publish OTLP metrics.

`benchmarks/grafana_publish.py` writes these gauge metrics:

- `mattsql_benchmark_median_ns`
- `mattsql_benchmark_min_ns`
- `mattsql_benchmark_max_ns`
- `mattsql_benchmark_iterations`
- `mattsql_benchmark_baseline_ns`
- `mattsql_benchmark_baseline_ratio`
- `mattsql_benchmark_improvement_ratio`
- `mattsql_benchmark_regression`
- `mattsql_benchmark_regression_threshold_ratio`

Each data point includes labels for benchmark name, subsystem family, branch,
commit SHA, short commit, repository, GitHub run metadata, and baseline status.
Import `observability/grafana/mattsql-performance-dashboard.json` into Grafana
and point the dashboard at the Prometheus/Mimir data source receiving OTLP
metrics.

The workflow publishes metrics even if the baseline check detects a regression,
then fails the job afterward. This preserves the regression datapoint in
Grafana while still enforcing performance as a CI gate.

## Custom Kernel Profiling

When MattSQL runs under a custom runtime/kernel, keep the benchmark payloads and
runtime boundary intact:

1. Boot the kernel or runtime image.
2. Launch a focused benchmark payload equivalent to one
   `mattsql_benchmarks --filter ...` run.
3. Collect host-side samples from the hypervisor.
4. Correlate those samples with MattSQL Perfetto traces where possible.

QEMU/KVM is the expected first hypervisor path on Linux. Use host-side
`perf kvm` sampling once the kernel can boot MattSQL and symbols are available.
This keeps kernel/runtime profiling below the SQL layer while still letting the
same benchmark families drive measurements.

## Adding New Profiling Spans

Add spans at subsystem boundaries or functions that represent meaningful
ownership and cost. Do not instrument every tiny helper by default; trace size
and observer overhead should stay bounded.

Use the existing pattern:

```cpp
#include "mattsql/common/trace.hpp"

mattsql::ScopedTrace trace("mattsql::TypeName::FunctionName",
                           "function.storage");
```

Choose the category based on the layer that owns the work. If the work crosses
the runtime boundary, keep the span name and category SQL-agnostic.
