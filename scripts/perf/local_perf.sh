#!/usr/bin/env bash
set -euo pipefail

event="manual"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --event)
      if [ "$#" -lt 2 ]; then
        echo "--event requires a value" >&2
        exit 2
      fi
      event="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [ "${MATTSQL_SKIP_PERF_HOOKS:-0}" = "1" ]; then
  echo "MattSQL perf hook skipped because MATTSQL_SKIP_PERF_HOOKS=1"
  exit 0
fi

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

profile_dir="build/profile"
report_dir="$profile_dir/performance-report"
benchmark_json="$profile_dir/benchmark-results.json"
prometheus_text="$report_dir/prometheus-metrics.prom"
history_jsonl="$profile_dir/performance-history.jsonl"
label="$(git rev-parse --short=12 HEAD 2>/dev/null || echo local)"
pushgateway_url="${PUSHGATEWAY_URL:-${PROMETHEUS_PUSHGATEWAY_URL:-http://localhost:9091}}"
publish="${MATTSQL_PERF_PUBLISH:-1}"
require_publish="${MATTSQL_PERF_REQUIRE_PUBLISH:-0}"
check_baseline="${MATTSQL_PERF_CHECK_BASELINE:-1}"
configure="${MATTSQL_PERF_CONFIGURE:-1}"
build="${MATTSQL_PERF_BUILD:-1}"
export MATTSQL_PERF_ENVIRONMENT="${MATTSQL_PERF_ENVIRONMENT:-local}"

mkdir -p "$report_dir"

if [ "$configure" != "0" ] && [ ! -f "$profile_dir/CMakeCache.txt" ]; then
  echo "[mattsql-perf] configuring profile preset"
  cmake --preset profile
fi

if [ "$build" != "0" ]; then
  echo "[mattsql-perf] building benchmark target"
  cmake --build --preset profile --target mattsql_benchmarks
fi

echo "[mattsql-perf] running benchmarks for $event"
"$profile_dir/mattsql_benchmarks" --json > "$benchmark_json"

report_status=0
echo "[mattsql-perf] writing local report"
python3 benchmarks/perf_visualize.py \
  --current-json "$benchmark_json" \
  --baseline benchmarks/baseline.tsv \
  --output-dir "$report_dir" \
  --history "$history_jsonl" \
  --label "$label" || report_status=$?

echo "[mattsql-perf] writing Prometheus metrics"
python3 benchmarks/prometheus_publish.py \
  --current-json "$benchmark_json" \
  --baseline benchmarks/baseline.tsv \
  --output-text "$prometheus_text"

if [ "$publish" != "0" ]; then
  echo "[mattsql-perf] publishing metrics to $pushgateway_url"
  publish_status=0
  python3 benchmarks/prometheus_publish.py \
    --current-json "$benchmark_json" \
    --baseline benchmarks/baseline.tsv \
    --output-text "$prometheus_text" \
    --pushgateway-url "$pushgateway_url" \
    --publish || publish_status=$?

  if [ "$publish_status" -ne 0 ]; then
    if [ "$require_publish" = "1" ]; then
      echo "[mattsql-perf] publish failed and MATTSQL_PERF_REQUIRE_PUBLISH=1" >&2
      exit "$publish_status"
    fi
    echo "[mattsql-perf] publish failed; continuing because publishing is best-effort" >&2
  fi
fi

if [ "$check_baseline" != "0" ] && [ "$report_status" -ne 0 ]; then
  echo "[mattsql-perf] performance regression detected" >&2
  exit "$report_status"
fi

echo "[mattsql-perf] complete"
