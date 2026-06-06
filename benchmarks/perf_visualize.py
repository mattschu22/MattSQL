#!/usr/bin/env python3
"""Generate static MattSQL benchmark visualizations."""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate MattSQL performance HTML and Markdown reports."
    )
    parser.add_argument("--benchmark", help="Path to the mattsql_benchmarks binary.")
    parser.add_argument("--current-json", help="Read benchmark JSON from this file.")
    parser.add_argument("--baseline", required=True, help="Path to baseline TSV.")
    parser.add_argument("--output-dir", required=True, help="Report output directory.")
    parser.add_argument("--history", help="JSONL history path.")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--filter", help="Benchmark name substring to run.")
    parser.add_argument("--iterations", type=int, help="Iteration override.")
    parser.add_argument("--label", help="Run label for trend views.")
    parser.add_argument(
        "--append-history",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Append the current run to the JSONL history.",
    )
    return parser.parse_args()


def run_command(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def git_value(repo_root: Path, *args: str) -> str | None:
    result = run_command(["git", *args], cwd=repo_root)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def git_metadata(repo_root: Path) -> dict[str, Any]:
    commit = git_value(repo_root, "rev-parse", "--short", "HEAD") or "unknown"
    branch = git_value(repo_root, "rev-parse", "--abbrev-ref", "HEAD") or "unknown"
    status = git_value(repo_root, "status", "--short") or ""
    return {
        "commit": commit,
        "branch": branch,
        "dirty": bool(status.strip()),
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


def run_benchmark(args: argparse.Namespace) -> dict[str, Any]:
    if not args.benchmark:
        raise ValueError("--benchmark is required when --current-json is not used")

    command = [args.benchmark, "--json"]
    if args.filter:
        command += ["--filter", args.filter]
    if args.iterations is not None:
        command += ["--iterations", str(args.iterations)]

    result = run_command(command)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise RuntimeError("benchmark run failed")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        sys.stderr.write(result.stdout)
        raise RuntimeError("benchmark did not emit valid JSON") from error


def load_current(args: argparse.Namespace) -> dict[str, Any]:
    if args.current_json:
        return json.loads(Path(args.current_json).read_text(encoding="utf-8"))
    return run_benchmark(args)


def make_record(args: argparse.Namespace, current: dict[str, Any]) -> dict[str, Any]:
    repo_root = Path(args.repo_root)
    metadata = git_metadata(repo_root)
    timestamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")
    label = args.label
    if not label:
        label = metadata["commit"] + ("*" if metadata["dirty"] else "")

    return {
        "version": 1,
        "timestamp_utc": timestamp,
        "label": label,
        "commit": metadata["commit"],
        "branch": metadata["branch"],
        "dirty": metadata["dirty"],
        "benchmarks": current.get("benchmarks", []),
    }


def load_history(path: Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []

    records: list[dict[str, Any]] = []
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as error:
            raise ValueError(f"{path}:{line_number}: invalid JSONL history") from error
    return records


def append_history(path: Path, record: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as output:
        output.write(json.dumps(record, sort_keys=True, separators=(",", ":")))
        output.write("\n")


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


def benchmark_rows(
    record: dict[str, Any], baselines: dict[str, dict[str, float]]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for benchmark in record.get("benchmarks", []):
        name = benchmark["name"]
        baseline = baselines.get(name)
        median = float(benchmark["median_ns_per_iteration"])
        baseline_ns = baseline["baseline_ns"] if baseline else None
        max_ratio = baseline["max_ratio"] if baseline else None
        ratio = median / baseline_ns if baseline_ns else None
        status = "missing-baseline"
        if ratio is not None and max_ratio is not None:
            status = "pass" if ratio <= max_ratio else "regression"
        rows.append(
            {
                "name": name,
                "family": benchmark_family(name),
                "description": benchmark.get("description", ""),
                "iterations": benchmark.get("iterations", 0),
                "median_ns": median,
                "min_ns": float(benchmark["min_ns_per_iteration"]),
                "max_ns": float(benchmark["max_ns_per_iteration"]),
                "baseline_ns": baseline_ns,
                "max_ratio": max_ratio,
                "ratio": ratio,
                "status": status,
            }
        )
    return sorted(rows, key=lambda row: (row["status"] != "regression", -(row["ratio"] or 0.0)))


def format_ns(value: float | None) -> str:
    if value is None:
        return "n/a"
    if value >= 1_000_000:
        return f"{value / 1_000_000:.3f} ms"
    if value >= 1_000:
        return f"{value / 1_000:.3f} us"
    return f"{value:.0f} ns"


def format_ratio(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.2f}x"


def trend_points(history: list[dict[str, Any]], benchmark_name: str) -> list[dict[str, Any]]:
    points: list[dict[str, Any]] = []
    for record in history:
        for benchmark in record.get("benchmarks", []):
            if benchmark.get("name") == benchmark_name:
                points.append(
                    {
                        "label": record.get("label", record.get("commit", "")),
                        "timestamp": record.get("timestamp_utc", ""),
                        "value": float(benchmark["median_ns_per_iteration"]),
                    }
                )
                break
    return points[-24:]


def sparkline(points: list[dict[str, Any]], baseline_ns: float | None) -> str:
    width = 420
    height = 92
    left = 8
    right = width - 8
    top = 8
    bottom = height - 18
    values = [point["value"] for point in points]
    if baseline_ns:
        values.append(baseline_ns)
    if not values:
        return ""
    low = min(values)
    high = max(values)
    if low == high:
        low *= 0.95
        high *= 1.05
    if low == high:
        high = low + 1.0

    def x_at(index: int) -> float:
        if len(points) == 1:
            return (left + right) / 2.0
        return left + (right - left) * index / (len(points) - 1)

    def y_at(value: float) -> float:
        return bottom - (bottom - top) * ((value - low) / (high - low))

    polyline = " ".join(
        f"{x_at(index):.1f},{y_at(point['value']):.1f}"
        for index, point in enumerate(points)
    )
    baseline_line = ""
    if baseline_ns:
        y = y_at(baseline_ns)
        baseline_line = (
            f'<line x1="{left}" y1="{y:.1f}" x2="{right}" y2="{y:.1f}" '
            'class="spark-baseline" />'
        )
    dots = "\n".join(
        f'<circle cx="{x_at(index):.1f}" cy="{y_at(point["value"]):.1f}" r="2.4" />'
        for index, point in enumerate(points)
    )
    return (
        f'<svg viewBox="0 0 {width} {height}" class="sparkline" role="img">'
        f"{baseline_line}"
        f'<polyline points="{polyline}" />'
        f"{dots}"
        f'<text x="{left}" y="{height - 3}">{html.escape(points[0]["label"])}</text>'
        f'<text x="{right}" y="{height - 3}" text-anchor="end">'
        f'{html.escape(points[-1]["label"])}</text>'
        "</svg>"
    )


def render_markdown(rows: list[dict[str, Any]], record: dict[str, Any]) -> str:
    lines = [
        "# MattSQL Performance Report",
        "",
        f"- Run: `{record.get('label', '')}`",
        f"- Commit: `{record.get('commit', '')}`",
        f"- Branch: `{record.get('branch', '')}`",
        f"- Dirty worktree: `{record.get('dirty', False)}`",
        f"- Timestamp UTC: `{record.get('timestamp_utc', '')}`",
        "",
        "| Status | Benchmark | Current | Baseline | Ratio | Limit |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        status = "FAIL" if row["status"] == "regression" else "PASS"
        if row["status"] == "missing-baseline":
            status = "NO BASELINE"
        lines.append(
            "| "
            + " | ".join(
                [
                    status,
                    f"`{row['name']}`",
                    format_ns(row["median_ns"]),
                    format_ns(row["baseline_ns"]),
                    format_ratio(row["ratio"]),
                    format_ratio(row["max_ratio"]),
                ]
            )
            + " |"
        )
    lines.append("")
    return "\n".join(lines)


def render_html(
    rows: list[dict[str, Any]],
    record: dict[str, Any],
    history: list[dict[str, Any]],
) -> str:
    regressions = [row for row in rows if row["status"] == "regression"]
    max_ratio = max((row["ratio"] or 0.0 for row in rows), default=0.0)
    families = sorted({row["family"] for row in rows})

    row_html = []
    for row in rows:
        ratio = row["ratio"] or 0.0
        width = min(100.0, ratio * 50.0)
        status_class = row["status"]
        spark = sparkline(trend_points(history, row["name"]), row["baseline_ns"])
        row_html.append(
            f"""
            <section class="benchmark {status_class}">
              <div class="benchmark-head">
                <div>
                  <h3>{html.escape(row["name"])}</h3>
                  <p>{html.escape(row["description"])}</p>
                </div>
                <span class="status">{html.escape(status_class)}</span>
              </div>
              <div class="metrics">
                <span>current <strong>{format_ns(row["median_ns"])}</strong></span>
                <span>baseline <strong>{format_ns(row["baseline_ns"])}</strong></span>
                <span>ratio <strong>{format_ratio(row["ratio"])}</strong></span>
                <span>limit <strong>{format_ratio(row["max_ratio"])}</strong></span>
              </div>
              <div class="bar-track"><div class="bar" style="width: {width:.1f}%"></div></div>
              {spark}
            </section>
            """
        )

    family_html = []
    for family in families:
        family_rows = [row for row in rows if row["family"] == family]
        average_ratio = sum(row["ratio"] or 0.0 for row in family_rows) / len(family_rows)
        family_html.append(
            f"""
            <div class="family">
              <span>{html.escape(family)}</span>
              <strong>{average_ratio:.2f}x</strong>
            </div>
            """
        )

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MattSQL Performance</title>
  <style>
    body {{
      margin: 0;
      font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      color: #16211b;
      background: #f6f7f3;
    }}
    header {{
      padding: 28px 32px 18px;
      background: #1c3028;
      color: #f7fbf5;
    }}
    main {{ max-width: 1180px; margin: 0 auto; padding: 24px 20px 44px; }}
    h1, h2, h3 {{ margin: 0; letter-spacing: 0; }}
    header p {{ margin: 8px 0 0; color: #d7e2db; }}
    .cards, .families {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 12px;
      margin-bottom: 20px;
    }}
    .card, .family, .benchmark {{
      background: #ffffff;
      border: 1px solid #dfe5de;
      border-radius: 8px;
      box-shadow: 0 1px 2px rgba(22, 33, 27, 0.05);
    }}
    .card {{ padding: 16px; }}
    .card span {{ display: block; color: #5e6b63; font-size: 12px; }}
    .card strong {{ display: block; margin-top: 4px; font-size: 22px; }}
    .family {{ padding: 12px 14px; display: flex; justify-content: space-between; }}
    .benchmark {{ padding: 16px; margin: 12px 0; }}
    .benchmark-head {{
      display: flex;
      justify-content: space-between;
      align-items: start;
      gap: 16px;
    }}
    .benchmark h3 {{ font-size: 16px; overflow-wrap: anywhere; }}
    .benchmark p {{ margin: 4px 0 0; color: #5e6b63; }}
    .status {{
      border-radius: 999px;
      padding: 3px 9px;
      background: #e9eee8;
      color: #405047;
      white-space: nowrap;
      font-size: 12px;
    }}
    .benchmark.regression .status {{ background: #f9d4d0; color: #7e241d; }}
    .benchmark.pass .status {{ background: #d9efe0; color: #1d6131; }}
    .metrics {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(140px, 1fr));
      gap: 8px;
      margin: 14px 0 10px;
      color: #5e6b63;
    }}
    .metrics strong {{ color: #16211b; }}
    .bar-track {{
      height: 10px;
      background: #e8ece7;
      border-radius: 999px;
      overflow: hidden;
    }}
    .bar {{
      height: 100%;
      background: #3e7f55;
      border-radius: inherit;
    }}
    .regression .bar {{ background: #c94b3f; }}
    .sparkline {{
      width: 100%;
      height: 92px;
      margin-top: 14px;
      overflow: visible;
    }}
    .sparkline polyline {{
      fill: none;
      stroke: #315f8f;
      stroke-width: 2.2;
      stroke-linejoin: round;
      stroke-linecap: round;
    }}
    .sparkline circle {{ fill: #315f8f; }}
    .sparkline text {{ fill: #708078; font-size: 10px; }}
    .spark-baseline {{
      stroke: #8d9b92;
      stroke-dasharray: 4 4;
      stroke-width: 1.2;
    }}
    .section-title {{ margin: 26px 0 12px; }}
  </style>
</head>
<body>
  <header>
    <h1>MattSQL Performance</h1>
    <p>Run {html.escape(str(record.get("label", "")))} on branch
    {html.escape(str(record.get("branch", "")))} at
    {html.escape(str(record.get("timestamp_utc", "")))}.</p>
  </header>
  <main>
    <section class="cards">
      <div class="card"><span>Benchmarks</span><strong>{len(rows)}</strong></div>
      <div class="card"><span>Regressions</span><strong>{len(regressions)}</strong></div>
      <div class="card"><span>Worst Ratio</span><strong>{max_ratio:.2f}x</strong></div>
      <div class="card"><span>Dirty Worktree</span><strong>{record.get("dirty", False)}</strong></div>
    </section>
    <h2 class="section-title">Subsystem Ratios</h2>
    <section class="families">{"".join(family_html)}</section>
    <h2 class="section-title">Current Vs Baseline</h2>
    {"".join(row_html)}
  </main>
</body>
</html>
"""


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    history_path = Path(args.history) if args.history else output_dir / "history.jsonl"

    baseline = parse_baseline(Path(args.baseline))
    current = load_current(args)
    record = make_record(args, current)

    if args.append_history:
        append_history(history_path, record)
    history = load_history(history_path)
    if not history or history[-1] != record:
        history.append(record)

    rows = benchmark_rows(record, baseline)
    (output_dir / "current.json").write_text(
        json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (output_dir / "current_vs_baseline.md").write_text(
        render_markdown(rows, record), encoding="utf-8"
    )
    (output_dir / "index.html").write_text(
        render_html(rows, record, history), encoding="utf-8"
    )

    regressions = [row for row in rows if row["status"] == "regression"]
    print(f"wrote {output_dir / 'index.html'}")
    print(f"wrote {output_dir / 'current_vs_baseline.md'}")
    return 1 if regressions else 0


if __name__ == "__main__":
    raise SystemExit(main())
