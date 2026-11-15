#!/usr/bin/env python3
"""DDS/RTPS matrix runner (Python 3.8+, standard library only).

DDS does not have RPC-style sync/async modes or socket connections. This matrix
therefore separates two application patterns:
  pubsub: continuous best-effort writer -> reader callback, one-way delivery;
  rtt:    sequential request -> reader echo -> response callback, window=1 RTT.

`pairs=2|4` means independent participant pairs (separate process, domain and
four RTPS ports per pair), not connections within one participant. Per-cell
aggregate QPS is the sum of pair QPS. Latency percentiles are reported as
per-pair median/min/max; raw samples are not available to merge true percentiles.

Default matrix: 2 patterns x 5 payloads x 2 pair counts = 20 cells. At 60s per
cell, pure measurement is about 20 minutes plus process startup/cleanup.
"""
import argparse
import csv
import datetime
import itertools
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_PAYLOADS = [1024, 2048, 4096, 9216, 65536]


def positive(value):
    n = int(value)
    if n <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build-dir", type=Path, default=Path("build"))
    p.add_argument("--duration", type=positive, default=60)
    p.add_argument("--rounds", type=positive, default=1)
    p.add_argument("--payloads", nargs="+", type=positive, default=DEFAULT_PAYLOADS)
    p.add_argument("--pairs", nargs="+", type=positive, default=[2, 4])
    p.add_argument("--patterns", nargs="+", choices=["pubsub", "rtt"],
                   default=["pubsub", "rtt"])
    p.add_argument("--output-dir", type=Path,
                   default=Path("artifacts/dds-matrix-" +
                               datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    p.add_argument("--dry-run", action="store_true")
    a = p.parse_args()
    if max(a.payloads) > 1024 * 1024:
        p.error("payload exceeds 1 MiB tool limit")
    if len(set(a.payloads)) != len(a.payloads) or len(set(a.pairs)) != len(a.pairs):
        p.error("duplicate payload/pair value")
    return a


def executable(build, name):
    for candidate in (build / "tools" / name, build / name, build / "bin" / name):
        if candidate.is_file():
            return str(candidate.resolve())
    return str((build / "tools" / name).resolve())


def parse_output(text):
    result = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        try:
            result[key] = float(value) if any(c in value for c in ".eE") else int(value)
        except ValueError:
            result[key] = value
    return result


def quantile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, int((len(ordered) - 1) * q + 0.5)))]


def one_cell(args, pattern, payload, pairs, round_no, cell_no):
    tag = f"{pattern}-{payload}-pairs{pairs}-r{round_no}"
    directory = args.output_dir / tag
    directory.mkdir(parents=True)
    proc_records = []
    procs = []
    start = time.monotonic()
    tool = "dds_bench" if pattern == "pubsub" else "dds_rtt_bench"
    exe = executable(args.build_dir, tool)
    if not Path(exe).is_file():
        raise RuntimeError("missing benchmark tool: " + exe)

    # Reserve disjoint domains/ports per pair and per cell. Domain range is
    # 0..232; ports stay below 60000 for four consecutive RTPS endpoints.
    base_domain = (40 + cell_no * 7 + round_no * 3) % 220
    base_port = 50000 + cell_no * 64 + round_no * 8
    if base_port + (pairs - 1) * 64 + 3 >= 60000:
        raise RuntimeError("port plan exceeded safe range; reduce matrix size")

    for i in range(pairs):
        domain = (base_domain + i) % 232
        port = base_port + i * 8
        cmd = [exe, str(args.duration), str(payload), str(domain), str(port)]
        out_path = directory / f"pair-{i+1}.log"
        handle = out_path.open("w", encoding="utf-8")
        procs.append((subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT),
                      handle, out_path, cmd, i + 1))

    for proc, handle, path, cmd, pair_no in procs:
        try:
            rc = proc.wait(timeout=args.duration + 90)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait()
        handle.close()
        raw = path.read_text(encoding="utf-8", errors="replace")
        metrics = parse_output(raw)
        completed = int(metrics.get("RECEIVED", metrics.get("COMPLETED", 0)) or 0)
        failed = (rc != 0 or completed == 0 or
                  int(metrics.get("TIMED_OUT", 0) or 0) != 0 or
                  int(metrics.get("WRITE_FAILED", 0) or 0) != 0)
        if pattern == "pubsub":
            qps = float(metrics.get("RECEIVED_QPS", 0.0) or 0.0)
            latency_key = "LATENCY_US_P99"
        else:
            qps = float(metrics.get("QPS", 0.0) or 0.0)
            latency_key = "RTT_US_P99"
        proc_records.append({
            "pair": pair_no,
            "command": cmd,
            "exit_code": rc,
            "status": "FAIL" if failed else "PASS",
            "qps": qps,
            "latency_p50_us": float(metrics.get("LATENCY_US_P50" if pattern == "pubsub"
                                                  else "RTT_US_P50", 0) or 0),
            "latency_p95_us": float(metrics.get("LATENCY_US_P95" if pattern == "pubsub"
                                                  else "RTT_US_P95", 0) or 0),
            "latency_p99_us": float(metrics.get(latency_key, 0) or 0),
            "completed": completed,
            "timeouts": int(metrics.get("TIMED_OUT", 0) or 0),
            "write_failed": int(metrics.get("WRITE_FAILED", 0) or 0),
            "dropped": int(metrics.get("DROPPED", 0) or 0),
            "raw_metrics": metrics,
            "log": str(path),
        })

    qps_values = [r["qps"] for r in proc_records]
    summary = {
        "pattern": pattern,
        "payload_bytes": payload,
        "pairs": pairs,
        "round": round_no,
        "duration_requested_seconds": args.duration,
        "domain_base": base_domain,
        "port_base": base_port,
        "status": "PASS" if all(r["status"] == "PASS" for r in proc_records) else "FAIL",
        "aggregate_qps": sum(qps_values),
        "pair_qps_min": min(qps_values) if qps_values else 0,
        "pair_qps_median": statistics.median(qps_values) if qps_values else 0,
        "pair_qps_max": max(qps_values) if qps_values else 0,
        "pair_p50_median_us": statistics.median(r["latency_p50_us"] for r in proc_records),
        "pair_p95_median_us": statistics.median(r["latency_p95_us"] for r in proc_records),
        "pair_p99_median_us": statistics.median(r["latency_p99_us"] for r in proc_records),
        "pair_p99_min_us": min(r["latency_p99_us"] for r in proc_records),
        "pair_p99_max_us": max(r["latency_p99_us"] for r in proc_records),
        "completed_total": sum(r["completed"] for r in proc_records),
        "timeouts_total": sum(r["timeouts"] for r in proc_records),
        "write_failed_total": sum(r["write_failed"] for r in proc_records),
        "dropped_total": sum(r["dropped"] for r in proc_records),
        "pair_results": proc_records,
    }
    return summary


def main():
    args = parse_args()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    cells = list(itertools.product(args.patterns, args.payloads, args.pairs))
    total = len(cells) * args.rounds
    print(f"{total} cells/runs, estimate {total * args.duration / 60:.1f} pure-measurement minutes")
    if args.dry_run:
        for i, (pattern, payload, pairs) in enumerate(cells, 1):
            print(f"{i}: {pattern}, {payload} bytes, {pairs} independent participant pairs")
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=False)
    results = []
    for round_no in range(1, args.rounds + 1):
        for i, (pattern, payload, pairs) in enumerate(cells, 1):
            row = one_cell(args, pattern, payload, pairs, round_no, i)
            results.append(row)
            print(f"[{len(results)}/{total}] {row['status']} {pattern} payload={payload} "
                  f"pairs={pairs} aggregate_qps={row['aggregate_qps']:.1f}", flush=True)
            (args.output_dir / "results.json").write_text(
                json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    summary_rows = []
    keys = ("pattern", "payload_bytes", "pairs")
    for cell in cells:
        group = [r for r in results if tuple(r[k] for k in keys) == cell]
        summary_rows.append({
            "pattern": cell[0], "payload_bytes": cell[1], "pairs": cell[2],
            "rounds": len(group),
            "failed_rounds": sum(r["status"] != "PASS" for r in group),
            "aggregate_qps_median": statistics.median(r["aggregate_qps"] for r in group),
            "pair_p50_median_us": statistics.median(r["pair_p50_median_us"] for r in group),
            "pair_p95_median_us": statistics.median(r["pair_p95_median_us"] for r in group),
            "pair_p99_median_us": statistics.median(r["pair_p99_median_us"] for r in group),
            "completed_median": statistics.median(r["completed_total"] for r in group),
            "timeouts_total": sum(r["timeouts_total"] for r in group),
            "dropped_total": sum(r["dropped_total"] for r in group),
        })
    (args.output_dir / "summary.json").write_text(
        json.dumps(summary_rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with (args.output_dir / "summary.csv").open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(summary_rows[0]))
        writer.writeheader()
        writer.writerows(summary_rows)
    return 1 if any(r["status"] != "PASS" for r in results) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(130)
    except (OSError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
