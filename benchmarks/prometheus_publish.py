#!/usr/bin/env python3
"""Publish MattSQL benchmark results to a Prometheus Pushgateway."""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert MattSQL benchmark JSON to Prometheus metrics."
    )
    parser.add_argument("--current-json", required=True, help="Benchmark JSON input.")
    parser.add_argument("--baseline", required=True, help="Benchmark baseline TSV.")
    parser.add_argument(
        "--output-text",
        required=True,
        help="Prometheus exposition text output path.",
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Repository root used for git metadata fallback.",
    )
    parser.add_argument(
        "--publish",
        action="store_true",
        help="PUT the metrics to a Pushgateway after writing them.",
    )
    parser.add_argument(
        "--pushgateway-url",
        default=os.environ.get("PUSHGATEWAY_URL")
        or os.environ.get("PROMETHEUS_PUSHGATEWAY_URL"),
        help="Pushgateway base URL, for example https://perf.example.com.",
    )
    parser.add_argument(
        "--headers",
        default=os.environ.get("PUSHGATEWAY_HEADERS"),
        help="Comma-separated HTTP headers, for example Authorization=Bearer%%20...",
    )
    parser.add_argument(
        "--username",
        default=os.environ.get("PUSHGATEWAY_USERNAME"),
        help="Basic auth username for the Pushgateway reverse proxy.",
    )
    parser.add_argument(
        "--password",
        default=os.environ.get("PUSHGATEWAY_PASSWORD"),
        help="Basic auth password for the Pushgateway reverse proxy.",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=30.0,
        help="HTTP publish timeout.",
    )
    return parser.parse_args()


def run_command(command: list[str], cwd: Path | None = None) -> str | None:
    result = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def git_value(repo_root: Path, *args: str) -> str | None:
    return run_command(["git", *args], cwd=repo_root)


def git_metadata(repo_root: Path) -> dict[str, str]:
    github_sha = os.environ.get("GITHUB_SHA")
    github_ref_name = os.environ.get("GITHUB_REF_NAME")
    github_repo = os.environ.get("GITHUB_REPOSITORY")
    return {
        "commit_sha": github_sha or git_value(repo_root, "rev-parse", "HEAD") or "unknown",
        "branch": github_ref_name
        or git_value(repo_root, "rev-parse", "--abbrev-ref", "HEAD")
        or "unknown",
        "repository": github_repo
        or Path(git_value(repo_root, "rev-parse", "--show-toplevel") or repo_root).name,
    }


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


def benchmark_family(name: str) -> str:
    if name.startswith("frontend_"):
        return "frontend"
    if name.startswith("engine_") or name.startswith("sql_logic_"):
        return "engine"
    if name.startswith("tuple_") or name.startswith("slotted_") or name.startswith("page_"):
        return "storage"
    if name.startswith("runtime_"):
        return "runtime"
    return "other"


def label_value(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def label_fragment(labels: dict[str, str]) -> str:
    return ",".join(
        f'{key}="{label_value(value)}"' for key, value in sorted(labels.items())
    )


def metric_line(name: str, labels: dict[str, str], value: float) -> str:
    return f"{name}{{{label_fragment(labels)}}} {value:.17g}"


def help_and_type(name: str, help_text: str) -> list[str]:
    return [
        f"# HELP {name} {help_text}",
        f"# TYPE {name} gauge",
    ]


def benchmark_labels(
    benchmark: dict[str, Any],
    metadata: dict[str, str],
    baseline: dict[str, float] | None,
) -> dict[str, str]:
    name = str(benchmark["name"])
    commit_sha = metadata["commit_sha"]
    labels = {
        "benchmark": name,
        "family": benchmark_family(name),
        "commit_sha": commit_sha,
        "commit_short": commit_sha[:12],
        "branch": metadata["branch"],
        "repository": metadata["repository"],
        "environment": os.environ.get("MATTSQL_PERF_ENVIRONMENT", "ci"),
    }
    for env_name, label_name in [
        ("GITHUB_RUN_ID", "github_run_id"),
        ("GITHUB_RUN_ATTEMPT", "github_run_attempt"),
        ("GITHUB_WORKFLOW", "github_workflow"),
        ("GITHUB_EVENT_NAME", "github_event_name"),
    ]:
        if os.environ.get(env_name):
            labels[label_name] = os.environ[env_name]

    if baseline is not None:
        ratio = float(benchmark["median_ns_per_iteration"]) / baseline["baseline_ns"]
        labels["baseline_status"] = (
            "regression" if ratio > baseline["max_ratio"] else "pass"
        )
    else:
        labels["baseline_status"] = "missing"
    return labels


def render_prometheus_text(
    current: dict[str, Any],
    baselines: dict[str, dict[str, float]],
    metadata: dict[str, str],
) -> str:
    metrics = {
        "mattsql_benchmark_median_ns": (
            "Median nanoseconds per benchmark iteration.",
            [],
        ),
        "mattsql_benchmark_min_ns": (
            "Minimum nanoseconds per benchmark iteration.",
            [],
        ),
        "mattsql_benchmark_max_ns": (
            "Maximum nanoseconds per benchmark iteration.",
            [],
        ),
        "mattsql_benchmark_iterations": (
            "Benchmark iterations completed in this run.",
            [],
        ),
        "mattsql_benchmark_baseline_ns": (
            "Baseline median nanoseconds per benchmark iteration.",
            [],
        ),
        "mattsql_benchmark_baseline_ratio": (
            "Current median divided by baseline median; above 1 is slower.",
            [],
        ),
        "mattsql_benchmark_improvement_ratio": (
            "Baseline median divided by current median; above 1 is faster.",
            [],
        ),
        "mattsql_benchmark_regression": (
            "1 when the current median exceeds the allowed baseline ratio.",
            [],
        ),
        "mattsql_benchmark_regression_threshold_ratio": (
            "Allowed current-to-baseline regression ratio.",
            [],
        ),
    }

    for benchmark in current.get("benchmarks", []):
        name = str(benchmark["name"])
        baseline = baselines.get(name)
        labels = benchmark_labels(benchmark, metadata, baseline)
        median = float(benchmark["median_ns_per_iteration"])
        metrics["mattsql_benchmark_median_ns"][1].append(
            metric_line("mattsql_benchmark_median_ns", labels, median)
        )
        metrics["mattsql_benchmark_min_ns"][1].append(
            metric_line(
                "mattsql_benchmark_min_ns",
                labels,
                float(benchmark["min_ns_per_iteration"]),
            )
        )
        metrics["mattsql_benchmark_max_ns"][1].append(
            metric_line(
                "mattsql_benchmark_max_ns",
                labels,
                float(benchmark["max_ns_per_iteration"]),
            )
        )
        metrics["mattsql_benchmark_iterations"][1].append(
            metric_line(
                "mattsql_benchmark_iterations",
                labels,
                float(benchmark.get("iterations", 0)),
            )
        )

        if baseline is not None:
            baseline_ns = baseline["baseline_ns"]
            ratio = median / baseline_ns
            improvement = baseline_ns / median if median > 0.0 else 0.0
            regression = 1.0 if ratio > baseline["max_ratio"] else 0.0
            metrics["mattsql_benchmark_baseline_ns"][1].append(
                metric_line("mattsql_benchmark_baseline_ns", labels, baseline_ns)
            )
            metrics["mattsql_benchmark_baseline_ratio"][1].append(
                metric_line("mattsql_benchmark_baseline_ratio", labels, ratio)
            )
            metrics["mattsql_benchmark_improvement_ratio"][1].append(
                metric_line("mattsql_benchmark_improvement_ratio", labels, improvement)
            )
            metrics["mattsql_benchmark_regression"][1].append(
                metric_line("mattsql_benchmark_regression", labels, regression)
            )
            metrics["mattsql_benchmark_regression_threshold_ratio"][1].append(
                metric_line(
                    "mattsql_benchmark_regression_threshold_ratio",
                    labels,
                    baseline["max_ratio"],
                )
            )

    lines: list[str] = []
    for metric_name, (help_text, metric_lines) in metrics.items():
        if not metric_lines:
            continue
        lines.extend(help_and_type(metric_name, help_text))
        lines.extend(metric_lines)
    return "\n".join(lines) + "\n"


def parse_headers(raw_headers: str | None) -> dict[str, str]:
    headers = {"Content-Type": "text/plain; version=0.0.4; charset=utf-8"}
    if not raw_headers:
        return headers

    for raw_pair in raw_headers.split(","):
        pair = raw_pair.strip()
        if not pair:
            continue
        if "=" not in pair:
            raise ValueError(f"invalid HTTP header entry: {pair!r}")
        key, value = pair.split("=", 1)
        headers[key.strip()] = urllib.parse.unquote(value.strip())
    return headers


def slug(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return cleaned.strip("_") or "unknown"


def pushgateway_endpoint(pushgateway_url: str | None, metadata: dict[str, str]) -> str:
    if not pushgateway_url:
        raise ValueError("Pushgateway URL is required when --publish is set")
    base = pushgateway_url.rstrip("/")
    if base.endswith("/metrics"):
        return base

    grouping = [
        "job",
        "mattsql_performance",
        "repository",
        slug(metadata["repository"]),
        "branch",
        slug(metadata["branch"]),
        "commit",
        slug(metadata["commit_sha"][:12]),
    ]
    encoded = "/".join(urllib.parse.quote(part, safe="") for part in grouping)
    return f"{base}/metrics/{encoded}"


def publish_metrics(
    pushgateway_url: str | None,
    headers: str | None,
    username: str | None,
    password: str | None,
    metadata: dict[str, str],
    body: str,
    timeout_seconds: float,
) -> None:
    url = pushgateway_endpoint(pushgateway_url, metadata)
    request_headers = parse_headers(headers)
    if "Authorization" not in request_headers and username and password:
        raw = f"{username}:{password}".encode("utf-8")
        request_headers["Authorization"] = (
            "Basic " + base64.b64encode(raw).decode("ascii")
        )

    request = urllib.request.Request(
        url,
        data=body.encode("utf-8"),
        headers=request_headers,
        method="PUT",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
            response_body = response.read().decode("utf-8", errors="replace")
            print(f"published Prometheus metrics to {url}: HTTP {response.status}")
            if response_body:
                print(response_body)
    except urllib.error.HTTPError as error:
        response_body = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"Pushgateway publish failed with HTTP {error.code}: {response_body}"
        ) from error


def main() -> int:
    args = parse_args()
    current = json.loads(Path(args.current_json).read_text(encoding="utf-8"))
    baselines = parse_baseline(Path(args.baseline))
    metadata = git_metadata(Path(args.repo_root))
    metrics_text = render_prometheus_text(current, baselines, metadata)

    output_path = Path(args.output_text)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(metrics_text, encoding="utf-8")
    print(f"wrote {output_path}")

    if args.publish:
        publish_metrics(
            pushgateway_url=args.pushgateway_url,
            headers=args.headers,
            username=args.username,
            password=args.password,
            metadata=metadata,
            body=metrics_text,
            timeout_seconds=args.timeout_seconds,
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"prometheus publish error: {error}", file=sys.stderr)
        raise SystemExit(1)
