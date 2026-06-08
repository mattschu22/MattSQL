#!/usr/bin/env python3
"""Publish MattSQL benchmark results as OTLP metrics for Grafana Cloud."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import json
import os
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert MattSQL benchmark JSON to OTLP metrics."
    )
    parser.add_argument("--current-json", required=True, help="Benchmark JSON input.")
    parser.add_argument("--baseline", required=True, help="Benchmark baseline TSV.")
    parser.add_argument("--output-json", required=True, help="OTLP payload output path.")
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[1]),
        help="Repository root used for git metadata fallback.",
    )
    parser.add_argument(
        "--service-name",
        default=os.environ.get("OTEL_SERVICE_NAME", "mattsql"),
        help="OTLP service.name resource attribute.",
    )
    parser.add_argument(
        "--environment",
        default=os.environ.get("MATTSQL_PERF_ENVIRONMENT", "ci"),
        help="deployment.environment resource attribute.",
    )
    parser.add_argument(
        "--publish",
        action="store_true",
        help="POST the OTLP payload to Grafana after writing it.",
    )
    parser.add_argument(
        "--endpoint",
        default=os.environ.get("GRAFANA_OTLP_ENDPOINT")
        or os.environ.get("OTEL_EXPORTER_OTLP_ENDPOINT"),
        help="Grafana OTLP endpoint. May be the root endpoint or /v1/metrics.",
    )
    parser.add_argument(
        "--headers",
        default=os.environ.get("GRAFANA_OTLP_HEADERS")
        or os.environ.get("OTEL_EXPORTER_OTLP_HEADERS"),
        help="Comma-separated OTLP headers, for example Authorization=Basic%%20...",
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


def string_attr(key: str, value: str) -> dict[str, Any]:
    return {"key": key, "value": {"stringValue": value}}


def datapoint(
    value: float,
    timestamp_ns: int,
    attributes: list[dict[str, Any]],
) -> dict[str, Any]:
    return {
        "attributes": attributes,
        "timeUnixNano": str(timestamp_ns),
        "asDouble": float(value),
    }


def gauge_metric(
    name: str,
    description: str,
    unit: str,
    points: list[dict[str, Any]],
) -> dict[str, Any]:
    return {
        "name": name,
        "description": description,
        "unit": unit,
        "gauge": {"dataPoints": points},
    }


def benchmark_attributes(
    benchmark: dict[str, Any],
    metadata: dict[str, str],
    baseline: dict[str, float] | None,
) -> list[dict[str, Any]]:
    name = str(benchmark["name"])
    commit_sha = metadata["commit_sha"]
    attrs = [
        string_attr("benchmark", name),
        string_attr("family", benchmark_family(name)),
        string_attr("commit_sha", commit_sha),
        string_attr("commit_short", commit_sha[:12]),
        string_attr("branch", metadata["branch"]),
        string_attr("repository", metadata["repository"]),
    ]
    if os.environ.get("GITHUB_RUN_ID"):
        attrs.append(string_attr("github_run_id", os.environ["GITHUB_RUN_ID"]))
    if os.environ.get("GITHUB_RUN_ATTEMPT"):
        attrs.append(string_attr("github_run_attempt", os.environ["GITHUB_RUN_ATTEMPT"]))
    if os.environ.get("GITHUB_WORKFLOW"):
        attrs.append(string_attr("github_workflow", os.environ["GITHUB_WORKFLOW"]))
    if os.environ.get("GITHUB_EVENT_NAME"):
        attrs.append(string_attr("github_event_name", os.environ["GITHUB_EVENT_NAME"]))
    if baseline is not None:
        ratio = float(benchmark["median_ns_per_iteration"]) / baseline["baseline_ns"]
        status = "regression" if ratio > baseline["max_ratio"] else "pass"
        attrs.append(string_attr("baseline_status", status))
    else:
        attrs.append(string_attr("baseline_status", "missing"))
    return attrs


def build_payload(
    current: dict[str, Any],
    baselines: dict[str, dict[str, float]],
    metadata: dict[str, str],
    service_name: str,
    environment: str,
) -> dict[str, Any]:
    timestamp_ns = int(dt.datetime.now(dt.timezone.utc).timestamp() * 1_000_000_000)

    median_points: list[dict[str, Any]] = []
    min_points: list[dict[str, Any]] = []
    max_points: list[dict[str, Any]] = []
    iteration_points: list[dict[str, Any]] = []
    baseline_points: list[dict[str, Any]] = []
    ratio_points: list[dict[str, Any]] = []
    improvement_points: list[dict[str, Any]] = []
    regression_points: list[dict[str, Any]] = []
    threshold_points: list[dict[str, Any]] = []

    for benchmark in current.get("benchmarks", []):
        name = str(benchmark["name"])
        baseline = baselines.get(name)
        attrs = benchmark_attributes(benchmark, metadata, baseline)
        median = float(benchmark["median_ns_per_iteration"])
        min_ns = float(benchmark["min_ns_per_iteration"])
        max_ns = float(benchmark["max_ns_per_iteration"])
        iterations = int(benchmark.get("iterations", 0))

        median_points.append(datapoint(median, timestamp_ns, attrs))
        min_points.append(datapoint(min_ns, timestamp_ns, attrs))
        max_points.append(datapoint(max_ns, timestamp_ns, attrs))
        iteration_points.append(datapoint(float(iterations), timestamp_ns, attrs))

        if baseline is not None:
            baseline_ns = baseline["baseline_ns"]
            max_ratio = baseline["max_ratio"]
            ratio = median / baseline_ns
            improvement = baseline_ns / median if median > 0.0 else 0.0
            regression = 1.0 if ratio > max_ratio else 0.0
            baseline_points.append(datapoint(baseline_ns, timestamp_ns, attrs))
            ratio_points.append(datapoint(ratio, timestamp_ns, attrs))
            improvement_points.append(datapoint(improvement, timestamp_ns, attrs))
            regression_points.append(datapoint(regression, timestamp_ns, attrs))
            threshold_points.append(datapoint(max_ratio, timestamp_ns, attrs))

    metrics = [
        gauge_metric(
            "mattsql_benchmark_median_ns",
            "Median nanoseconds per benchmark iteration.",
            "ns",
            median_points,
        ),
        gauge_metric(
            "mattsql_benchmark_min_ns",
            "Minimum nanoseconds per benchmark iteration.",
            "ns",
            min_points,
        ),
        gauge_metric(
            "mattsql_benchmark_max_ns",
            "Maximum nanoseconds per benchmark iteration.",
            "ns",
            max_points,
        ),
        gauge_metric(
            "mattsql_benchmark_iterations",
            "Benchmark iterations completed in this run.",
            "1",
            iteration_points,
        ),
    ]
    if baseline_points:
        metrics.extend(
            [
                gauge_metric(
                    "mattsql_benchmark_baseline_ns",
                    "Baseline median nanoseconds per benchmark iteration.",
                    "ns",
                    baseline_points,
                ),
                gauge_metric(
                    "mattsql_benchmark_baseline_ratio",
                    "Current median divided by baseline median; above 1 is slower.",
                    "1",
                    ratio_points,
                ),
                gauge_metric(
                    "mattsql_benchmark_improvement_ratio",
                    "Baseline median divided by current median; above 1 is faster.",
                    "1",
                    improvement_points,
                ),
                gauge_metric(
                    "mattsql_benchmark_regression",
                    "1 when the current median exceeds the allowed baseline ratio.",
                    "1",
                    regression_points,
                ),
                gauge_metric(
                    "mattsql_benchmark_regression_threshold_ratio",
                    "Allowed current-to-baseline regression ratio.",
                    "1",
                    threshold_points,
                ),
            ]
        )

    return {
        "resourceMetrics": [
            {
                "resource": {
                    "attributes": [
                        string_attr("service.name", service_name),
                        string_attr("service.namespace", "mattsql"),
                        string_attr("deployment.environment", environment),
                        string_attr("vcs.repository.name", metadata["repository"]),
                        string_attr("vcs.ref.head.name", metadata["branch"]),
                        string_attr("vcs.revision", metadata["commit_sha"]),
                    ]
                },
                "scopeMetrics": [
                    {
                        "scope": {
                            "name": "mattsql.benchmarks",
                            "version": "1",
                        },
                        "metrics": metrics,
                    }
                ],
            }
        ]
    }


def parse_headers(raw_headers: str | None) -> dict[str, str]:
    headers = {"Content-Type": "application/json"}
    if not raw_headers:
        return headers

    for raw_pair in raw_headers.split(","):
        pair = raw_pair.strip()
        if not pair:
            continue
        if "=" not in pair:
            raise ValueError(f"invalid OTLP header entry: {pair!r}")
        key, value = pair.split("=", 1)
        headers[key.strip()] = urllib.parse.unquote(value.strip())
    return headers


def metrics_endpoint(endpoint: str | None) -> str:
    if not endpoint:
        raise ValueError("Grafana OTLP endpoint is required when --publish is set")
    cleaned = endpoint.rstrip("/")
    if cleaned.endswith("/v1/metrics"):
        return cleaned
    return f"{cleaned}/v1/metrics"


def publish_payload(
    endpoint: str | None,
    headers: str | None,
    payload: dict[str, Any],
    timeout_seconds: float,
) -> None:
    url = metrics_endpoint(endpoint)
    request_headers = parse_headers(headers)
    if "Authorization" not in request_headers:
        instance_id = os.environ.get("GRAFANA_INSTANCE_ID")
        token = os.environ.get("GRAFANA_API_TOKEN")
        if instance_id and token:
            raw = f"{instance_id}:{token}".encode("utf-8")
            request_headers["Authorization"] = (
                "Basic " + base64.b64encode(raw).decode("ascii")
            )
    if "Authorization" not in request_headers:
        raise ValueError(
            "Grafana OTLP Authorization header is required. Set "
            "GRAFANA_OTLP_HEADERS or GRAFANA_INSTANCE_ID/GRAFANA_API_TOKEN."
        )

    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        headers=request_headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
            response_body = response.read().decode("utf-8", errors="replace")
            print(f"published OTLP metrics to {url}: HTTP {response.status}")
            if response_body:
                print(response_body)
    except urllib.error.HTTPError as error:
        response_body = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(
            f"Grafana OTLP publish failed with HTTP {error.code}: {response_body}"
        ) from error


def main() -> int:
    args = parse_args()
    current = json.loads(Path(args.current_json).read_text(encoding="utf-8"))
    baselines = parse_baseline(Path(args.baseline))
    metadata = git_metadata(Path(args.repo_root))
    payload = build_payload(
        current=current,
        baselines=baselines,
        metadata=metadata,
        service_name=args.service_name,
        environment=args.environment,
    )

    output_path = Path(args.output_json)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"wrote {output_path}")

    if args.publish:
        publish_payload(args.endpoint, args.headers, payload, args.timeout_seconds)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"grafana publish error: {error}", file=sys.stderr)
        raise SystemExit(1)
