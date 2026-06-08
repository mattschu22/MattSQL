#!/usr/bin/env python3
"""Run MattSQL performance benchmarks and materialize metrics artifacts."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run MattSQL benchmarks, write metrics, and enforce baselines."
    )
    parser.add_argument("--benchmark", required=True, help="Path to mattsql_benchmarks.")
    parser.add_argument("--baseline", required=True, help="Path to baseline TSV.")
    parser.add_argument("--output-dir", required=True, help="Metrics artifact directory.")
    parser.add_argument("--history", help="JSONL history path for trend reports.")
    parser.add_argument("--label", default="ctest", help="Run label for reports.")
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Repository root.",
    )
    parser.add_argument(
        "--publish",
        action="store_true",
        help="Publish Prometheus metrics to Pushgateway after writing them.",
    )
    parser.add_argument(
        "--allow-publish-failure",
        action="store_true",
        help="Do not fail when Pushgateway publishing fails.",
    )
    parser.add_argument(
        "--no-baseline-gate",
        action="store_true",
        help="Write metrics without failing on baseline regressions.",
    )
    parser.add_argument(
        "--pushgateway-url",
        help="Pushgateway base URL. Defaults to PUSHGATEWAY_URL in prometheus_publish.py.",
    )
    return parser.parse_args()


def run_command(command: list[str], stdout_path: Path | None = None) -> int:
    stdout_handle = stdout_path.open("w", encoding="utf-8") if stdout_path else None
    try:
        result = subprocess.run(
            command,
            text=True,
            stdout=stdout_handle if stdout_handle else subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    finally:
        if stdout_handle:
            stdout_handle.close()

    if result.returncode != 0:
        if not stdout_path:
            sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
    return result.returncode


def parse_baseline(path: Path) -> dict[str, dict[str, float]]:
    baselines: dict[str, dict[str, float]] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3:
            raise ValueError(
                f"{path}:{line_number}: expected benchmark baseline_ns max_ratio"
            )
        name, baseline_ns, max_ratio = fields
        baselines[name] = {
            "baseline_ns": float(baseline_ns),
            "max_ratio": float(max_ratio),
        }
    return baselines


def format_ns(value: float) -> str:
    if value >= 1_000_000.0:
        return f"{value / 1_000_000.0:.3f} ms"
    if value >= 1_000.0:
        return f"{value / 1_000.0:.3f} us"
    return f"{value:.0f} ns"


def print_metrics_summary(
    current: dict[str, Any],
    baselines: dict[str, dict[str, float]],
) -> bool:
    passed = True
    ran_names: set[str] = set()
    print("[PERF] benchmark metrics:")
    for benchmark in current.get("benchmarks", []):
        name = str(benchmark["name"])
        ran_names.add(name)
        median = float(benchmark["median_ns_per_iteration"])
        min_ns = float(benchmark["min_ns_per_iteration"])
        max_ns = float(benchmark["max_ns_per_iteration"])
        iterations = int(benchmark.get("iterations", 0))
        baseline = baselines.get(name)
        if baseline is None:
            print(
                f"[PERF][FAIL] {name} median={format_ns(median)} "
                f"min={format_ns(min_ns)} max={format_ns(max_ns)} "
                f"iterations={iterations} baseline=missing"
            )
            passed = False
            continue

        baseline_ns = baseline["baseline_ns"]
        ratio = median / baseline_ns
        max_ratio = baseline["max_ratio"]
        status = "PASS" if ratio <= max_ratio else "FAIL"
        print(
            f"[PERF][{status}] {name} median={format_ns(median)} "
            f"min={format_ns(min_ns)} max={format_ns(max_ns)} "
            f"iterations={iterations} baseline={format_ns(baseline_ns)} "
            f"ratio={ratio:.3f}x limit={max_ratio:.3f}x"
        )
        if ratio > max_ratio:
            passed = False

    for name in sorted(set(baselines) - ran_names):
        print(f"[PERF][FAIL] baseline benchmark did not run: {name}")
        passed = False
    return passed


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    repo_root = Path(args.repo_root)
    script_dir = repo_root / "benchmarks"
    current_json = output_dir / "benchmark-results.json"
    prometheus_text = output_dir / "prometheus-metrics.prom"
    history = Path(args.history) if args.history else output_dir / "history.jsonl"

    benchmark_status = run_command([args.benchmark, "--json"], stdout_path=current_json)
    if benchmark_status != 0:
        return benchmark_status

    current = json.loads(current_json.read_text(encoding="utf-8"))
    baselines = parse_baseline(Path(args.baseline))

    report_command = [
        sys.executable,
        str(script_dir / "perf_visualize.py"),
        "--current-json",
        str(current_json),
        "--baseline",
        args.baseline,
        "--output-dir",
        str(output_dir),
        "--history",
        str(history),
        "--label",
        args.label,
    ]
    report_status = run_command(report_command)

    prometheus_command = [
        sys.executable,
        str(script_dir / "prometheus_publish.py"),
        "--current-json",
        str(current_json),
        "--baseline",
        args.baseline,
        "--output-text",
        str(prometheus_text),
    ]
    if args.publish:
        prometheus_command.append("--publish")
    if args.pushgateway_url:
        prometheus_command.extend(["--pushgateway-url", args.pushgateway_url])

    publish_status = run_command(prometheus_command)
    if publish_status != 0 and not args.allow_publish_failure:
        return publish_status
    if publish_status != 0:
        print("[PERF] metric publishing failed; continuing because it is best-effort")

    print(f"[PERF] benchmark_json={current_json}")
    print(f"[PERF] prometheus_metrics={prometheus_text}")
    print(f"[PERF] report_html={output_dir / 'index.html'}")
    print(f"[PERF] report_markdown={output_dir / 'current_vs_baseline.md'}")

    baseline_passed = print_metrics_summary(current, baselines)
    if not args.no_baseline_gate and not baseline_passed:
        return 1
    if report_status != 0 and args.no_baseline_gate:
        return report_status
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
