#!/usr/bin/env python3
"""Collect focused MattSQL profiling artifacts."""

from __future__ import annotations

import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Collect benchmark profiler artifacts.")
    parser.add_argument("--benchmark", required=True, help="Path to mattsql_benchmarks.")
    parser.add_argument("--output-dir", required=True, help="Artifact output directory.")
    parser.add_argument(
        "--filters",
        nargs="+",
        default=["frontend", "engine_select", "tuple_codec", "page_heap"],
        help="Benchmark filters to collect.",
    )
    parser.add_argument(
        "--trace-iterations",
        type=int,
        default=1,
        help="Iterations for Perfetto/Chrome trace collection.",
    )
    parser.add_argument(
        "--perf-iterations",
        type=int,
        default=1000,
        help="Iterations for Linux perf flame graph collection.",
    )
    parser.add_argument(
        "--perf-frequency",
        type=int,
        default=99,
        help="Sampling frequency passed to perf record.",
    )
    parser.add_argument(
        "--perf-flamegraph",
        action="store_true",
        help="Collect perf.data and generate SVG flame graphs when tools are present.",
    )
    return parser.parse_args()


def sanitize(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    return cleaned.strip("_") or "benchmark"


def run_command(
    command: list[str],
    stdout_path: Path | None = None,
    stderr_path: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    stdout_handle = stdout_path.open("w", encoding="utf-8") if stdout_path else subprocess.PIPE
    stderr_handle = stderr_path.open("w", encoding="utf-8") if stderr_path else subprocess.PIPE
    try:
        return subprocess.run(
            command,
            text=True,
            stdout=stdout_handle,
            stderr=stderr_handle,
            check=False,
        )
    finally:
        if stdout_path:
            stdout_handle.close()
        if stderr_path:
            stderr_handle.close()


def collect_trace(
    benchmark: str,
    output_dir: Path,
    benchmark_filter: str,
    trace_iterations: int,
) -> dict[str, Any]:
    stem = sanitize(benchmark_filter)
    trace_path = output_dir / f"{stem}.trace.json"
    results_path = output_dir / f"{stem}.results.json"
    stderr_path = output_dir / f"{stem}.trace.stderr.txt"
    command = [
        benchmark,
        "--filter",
        benchmark_filter,
        "--iterations",
        str(trace_iterations),
        "--trace-json",
        str(trace_path),
        "--json",
    ]
    result = run_command(command, stdout_path=results_path, stderr_path=stderr_path)
    artifact = {
        "filter": benchmark_filter,
        "trace": str(trace_path),
        "results": str(results_path),
        "stderr": str(stderr_path),
        "returncode": result.returncode,
    }
    if result.returncode == 0:
        artifact.update(generate_trace_flamegraph(trace_path, output_dir, stem))
    return artifact


def clean_stack_name(name: str) -> str:
    return name.replace(";", ":").strip() or "unknown"


def trace_events_to_folded(trace_path: Path, folded_path: Path) -> int:
    data = json.loads(trace_path.read_text(encoding="utf-8"))
    events = []
    for event_id, event in enumerate(data.get("traceEvents", [])):
        if event.get("ph") != "X":
            continue
        name = clean_stack_name(str(event.get("name", "unknown")))
        start = float(event.get("ts", 0.0))
        duration = float(event.get("dur", 0.0))
        if duration <= 0:
            continue
        end = start + duration
        events.append({"id": event_id, "name": name, "start": start, "end": end})

    if not events:
        folded_path.write_text("", encoding="utf-8")
        return 0

    starts: dict[float, list[dict[str, Any]]] = {}
    ends: dict[float, list[dict[str, Any]]] = {}
    for event in events:
        starts.setdefault(event["start"], []).append(event)
        ends.setdefault(event["end"], []).append(event)

    boundaries = sorted(set(starts) | set(ends))
    active: dict[int, dict[str, Any]] = {}
    folded: dict[str, int] = {}
    previous = boundaries[0]
    for current in boundaries:
        if current > previous and active:
            ordered = sorted(
                active.values(),
                key=lambda event: (
                    event["start"],
                    -(event["end"] - event["start"]),
                    event["id"],
                ),
            )
            stack = ";".join(event["name"] for event in ordered)
            weight_ns = max(1, int(round((current - previous) * 1000.0)))
            folded[stack] = folded.get(stack, 0) + weight_ns

        for event in ends.get(current, []):
            active.pop(event["id"], None)
        for event in starts.get(current, []):
            active[event["id"]] = event
        previous = current

    if not folded and events:
        for event in events:
            weight_ns = max(1, int(round((event["end"] - event["start"]) * 1000.0)))
            folded[event["name"]] = folded.get(event["name"], 0) + weight_ns

    with folded_path.open("w", encoding="utf-8") as output:
        for stack, weight in sorted(folded.items()):
            output.write(f"{stack} {weight}\n")
    return len(folded)


def generate_trace_flamegraph(trace_path: Path, output_dir: Path, stem: str) -> dict[str, Any]:
    folded = output_dir / f"{stem}.trace.folded"
    stack_count = trace_events_to_folded(trace_path, folded)

    artifact: dict[str, Any] = {
        "trace_folded": str(folded),
        "trace_folded_stack_count": stack_count,
    }
    flamegraph = shutil.which("flamegraph.pl")
    if flamegraph is None:
        artifact["trace_flamegraph_available"] = False
        artifact["trace_flamegraph_reason"] = "flamegraph.pl was not found on PATH."
        return artifact

    svg = output_dir / f"{stem}.trace-flamegraph.svg"
    result = run_command(
        [
            flamegraph,
            "--title",
            f"MattSQL {stem} function trace",
            "--subtitle",
            "Perfetto complete-event durations",
            str(folded),
        ],
        stdout_path=svg,
    )
    artifact["trace_flamegraph_svg"] = str(svg)
    artifact["trace_flamegraph_returncode"] = result.returncode
    artifact["trace_flamegraph_available"] = result.returncode == 0
    if result.returncode != 0:
        artifact["trace_flamegraph_reason"] = "flamegraph.pl failed."
    return artifact


def collect_perf_flamegraph(
    benchmark: str,
    output_dir: Path,
    benchmark_filter: str,
    perf_iterations: int,
    perf_frequency: int,
) -> dict[str, Any]:
    stem = sanitize(benchmark_filter)
    perf = shutil.which("perf")
    stackcollapse = shutil.which("stackcollapse-perf.pl")
    flamegraph = shutil.which("flamegraph.pl")

    artifact: dict[str, Any] = {
        "filter": benchmark_filter,
        "requested": True,
        "available": False,
        "reason": "",
    }
    if platform.system() != "Linux":
        artifact["reason"] = "Linux perf flame graphs require a Linux host."
        return artifact
    if perf is None:
        artifact["reason"] = "perf was not found on PATH."
        return artifact

    perf_data = output_dir / f"{stem}.perf.data"
    perf_stderr = output_dir / f"{stem}.perf.stderr.txt"
    record_command = [
        perf,
        "record",
        "-F",
        str(perf_frequency),
        "-g",
        "-o",
        str(perf_data),
        "--",
        benchmark,
        "--filter",
        benchmark_filter,
        "--iterations",
        str(perf_iterations),
        "--quiet",
    ]
    record_result = run_command(record_command, stderr_path=perf_stderr)
    artifact.update(
        {
            "perf_data": str(perf_data),
            "perf_stderr": str(perf_stderr),
            "perf_record_returncode": record_result.returncode,
        }
    )
    if record_result.returncode != 0:
        artifact["reason"] = "perf record failed."
        return artifact

    perf_script = output_dir / f"{stem}.perf.script"
    script_result = run_command(
        [perf, "script", "-i", str(perf_data)], stdout_path=perf_script
    )
    artifact["perf_script"] = str(perf_script)
    artifact["perf_script_returncode"] = script_result.returncode
    if script_result.returncode != 0:
        artifact["reason"] = "perf script failed."
        return artifact

    if stackcollapse is None or flamegraph is None:
        artifact["reason"] = (
            "stackcollapse-perf.pl and flamegraph.pl are required for SVG output."
        )
        return artifact

    folded = output_dir / f"{stem}.folded"
    collapse_result = run_command([stackcollapse, str(perf_script)], stdout_path=folded)
    artifact["folded"] = str(folded)
    artifact["stackcollapse_returncode"] = collapse_result.returncode
    if collapse_result.returncode != 0:
        artifact["reason"] = "stackcollapse-perf.pl failed."
        return artifact

    svg = output_dir / f"{stem}.flamegraph.svg"
    flamegraph_result = run_command([flamegraph, str(folded)], stdout_path=svg)
    artifact["flamegraph_svg"] = str(svg)
    artifact["flamegraph_returncode"] = flamegraph_result.returncode
    if flamegraph_result.returncode != 0:
        artifact["reason"] = "flamegraph.pl failed."
        return artifact

    artifact["available"] = True
    return artifact


def write_readme(output_dir: Path, artifacts: list[dict[str, Any]]) -> None:
    lines = [
        "# MattSQL Performance Artifacts",
        "",
        "Open `*.trace.json` files in Perfetto UI or Chrome's trace viewer.",
        "Open `*.flamegraph.svg` files in a browser when Linux perf flame graphs were collected.",
        "",
    ]
    for artifact in artifacts:
        lines.append(f"## {artifact['filter']}")
        lines.append("")
        if "trace" in artifact:
            lines.append(f"- Trace: `{Path(artifact['trace']).name}`")
            lines.append(f"- Benchmark JSON: `{Path(artifact['results']).name}`")
            if artifact.get("trace_folded"):
                lines.append(f"- Trace folded stacks: `{Path(artifact['trace_folded']).name}`")
            if artifact.get("trace_flamegraph_available"):
                lines.append(
                    f"- Trace flame graph: `{Path(artifact['trace_flamegraph_svg']).name}`"
                )
            elif artifact.get("trace_flamegraph_reason"):
                lines.append(
                    f"- Trace flame graph unavailable: "
                    f"{artifact.get('trace_flamegraph_reason', '')}"
                )
            if artifact.get("returncode") != 0:
                lines.append(f"- Trace run failed with code `{artifact['returncode']}`")
        if artifact.get("requested"):
            if artifact.get("available"):
                lines.append(f"- Flame graph: `{Path(artifact['flamegraph_svg']).name}`")
            else:
                lines.append(f"- Flame graph unavailable: {artifact.get('reason', '')}")
        lines.append("")
    (output_dir / "README.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    artifacts: list[dict[str, Any]] = []
    failed = False
    for benchmark_filter in args.filters:
        trace_artifact = collect_trace(
            args.benchmark, output_dir, benchmark_filter, args.trace_iterations
        )
        artifacts.append(trace_artifact)
        failed = failed or trace_artifact["returncode"] != 0

        if args.perf_flamegraph:
            perf_artifact = collect_perf_flamegraph(
                args.benchmark,
                output_dir,
                benchmark_filter,
                args.perf_iterations,
                args.perf_frequency,
            )
            artifacts.append(perf_artifact)

    (output_dir / "artifacts.json").write_text(
        json.dumps(artifacts, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    write_readme(output_dir, artifacts)
    print(f"wrote {output_dir / 'artifacts.json'}")
    print(f"wrote {output_dir / 'README.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
