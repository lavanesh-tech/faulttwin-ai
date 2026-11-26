"""Summarise scripts/measure-collection.sh output into Markdown (standard library only)."""

import csv
import gzip
import math
import json
import statistics
import sys
from pathlib import Path


def pct(values, p):
    """Nearest-rank percentile, matching faulttwin-monitor's summary."""
    if not values:
        return None
    ordered = sorted(values)
    rank = max(1, math.ceil(p / 100.0 * len(ordered)))
    return ordered[min(rank, len(ordered)) - 1]


def fmt(value, digits=2):
    return "n/a" if value is None else f"{value:.{digits}f}"


def main(out_dir: Path) -> None:
    cycles, summary = [], None
    plain, packed = out_dir / "idle.jsonl", out_dir / "idle.jsonl.gz"
    text = plain.read_text() if plain.exists() else gzip.decompress(packed.read_bytes()).decode()
    for line in text.splitlines():
        record = json.loads(line)
        if record["type"] == "cycle":
            cycles.append(record)
        else:
            summary = record
    wall = [c["cycle_wall_ms"] for c in cycles]
    cpu = [c["client_cpu_ms"] for c in cycles]
    node_wall = [n["collect_wall_ms"] for c in cycles for n in c["nodes"]]
    fresh = [n["freshness_ms"] for c in cycles for n in c["nodes"]]
    # Cycle 1 opens the connections; report steady state separately.
    steady_wall, steady_cpu = wall[1:], cpu[1:]

    print(f"# Collection measurements ({out_dir.name})\n")
    print("## A. Idle cluster, 300 cycles at 200 ms, 3 nodes\n")
    print(f"- Collection success (MON-SUCC): {fmt(summary['collection_success_percent'])} % "
          f"({summary['successes']}/{summary['attempts']})")
    print(f"- Coverage (MON-COV): {fmt(summary['coverage_percent'])} % "
          f"({summary['readings_ok']}/{summary['readings_applicable']} applicable readings ok)")
    print(f"- First cycle (connects to 3 nodes): wall {fmt(wall[0])} ms, client CPU {fmt(cpu[0])} ms")
    print("\n| Metric (cycles 2-300) | mean | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|")
    for name, values in [("cycle wall ms (3 nodes)", steady_wall),
                         ("per-node collect wall ms", node_wall[3:]),
                         ("client CPU ms per cycle", steady_cpu),
                         ("freshness at report ms (MON-FRESH)", fresh)]:
        print(f"| {name} | {fmt(statistics.fmean(values))} | {fmt(pct(values, 50))} | "
              f"{fmt(pct(values, 95))} | {fmt(pct(values, 99))} | {fmt(max(values))} |")

    print("\nServer-side execution time of the monitor's own statements (pg_stat_statements):\n")
    print("| node | statements | total exec ms | exec ms per cycle |")
    print("|---|---|---|---|")
    with (out_dir / "idle-server.csv").open() as f:
        for row in csv.DictReader(f):
            per_cycle = float(row["total_exec_ms"]) / len(cycles)
            print(f"| {row['node']} | {row['calls']} | {row['total_exec_ms']} | {fmt(per_cycle, 3)} |")

    print("\n## B. pgbench throughput (4 clients, 15 s, single-row inserts)\n")
    by_condition = {}
    with (out_dir / "pgbench.csv").open() as f:
        for row in csv.DictReader(f):
            if row["tps"]:
                by_condition.setdefault(row["condition"], []).append(float(row["tps"]))
    print("| condition | runs | tps per run | mean tps | vs none |")
    print("|---|---|---|---|---|")
    base = statistics.fmean(by_condition["none"]) if by_condition.get("none") else None
    for condition in ["none", "monitor-1000ms", "monitor-100ms"]:
        values = by_condition.get(condition, [])
        if not values:
            continue
        mean = statistics.fmean(values)
        delta = "—" if base is None or condition == "none" else f"{100.0 * (mean - base) / base:+.1f} %"
        runs = ", ".join(f"{v:.0f}" for v in values)
        print(f"| {condition} | {len(values)} | {runs} | {mean:.0f} | {delta} |")
    spread = {c: (max(v) - min(v)) / statistics.fmean(v) * 100 for c, v in by_condition.items() if len(v) > 1}
    if spread:
        print("\nRun-to-run spread (max - min) / mean: " +
              ", ".join(f"{c} {s:.1f} %" for c, s in spread.items()) +
              ". Differences smaller than the spread are not distinguishable from noise.")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
