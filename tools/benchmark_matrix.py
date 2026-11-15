#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Linux TCP/UDP RPC 性能矩阵（仅 Python 3.8+ 标准库）。

sync 使用同步 API；async 使用异步 API。两者均 --window 1，单进程仅
一个请求在途。clients 是并行的独立 loadgen 进程数：TCP connections，
UDP sessions/独立客户端；window 不是客户端数，也不会创建多条连接。
默认 2 transport × 2 API × 5 payload × 2 clients = 40 格；单轮每格
60 秒，纯测量约 40 分钟，另加启动、连接及清理时间。payload 按严格
KiB 换算为 1024/2048/4096/9216/65536 bytes。--dry-run 不启动进程。

输出 summary.json（含原始轮次和完整参数）、rounds.csv、clients.csv 和
summary.csv（各格跨轮次 min/median/max）。aggregate QPS 是并行客户端
各自 succeeded / duration_seconds 的吞吐和，窗口起止有小量启动偏差。
聚合分位数先合并 histogram 同名桶，再取累计计数的 nearest-rank 桶值，
是桶精度的估计，绝不平均客户端 p99。失败轮次不计入成功轮次统计。
现有 server ready 日志没有 IO 数字：IO=6 记录为命令配置，并保留真实
启动日志及其可观察字段，不伪造 IO 的日志验证结果。
"""

import argparse
import csv
import datetime
import itertools
import json
import math
import re
import shlex
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path


NOTE = ("aggregate QPS = sum(client succeeded / client duration_seconds); "
        "parallel client measurement windows have small startup/end offsets")
METRICS = ("aggregate_qps", "p50_us", "p90_us", "p95_us", "p99_us",
           "failed", "timeouts")


def positive(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--duration", type=positive, default=60)
    parser.add_argument("--rounds", type=positive, default=1)
    parser.add_argument("--payloads", nargs="+", type=positive,
                        default=[1024, 2048, 4096, 9216, 65536], metavar="BYTES")
    parser.add_argument("--clients", nargs="+", type=positive, default=[2, 4])
    parser.add_argument("--transports", nargs="+", choices=["tcp", "udp"],
                        default=["tcp", "udp"])
    parser.add_argument("--output-dir", type=Path,
                        default=Path("benchmark_matrix_" + datetime.datetime.now().strftime("%Y%m%d_%H%M%S")))
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    for name in ("payloads", "clients", "transports"):
        if len(set(getattr(args, name))) != len(getattr(args, name)):
            parser.error("--" + name + " contains duplicates")
    if max(args.payloads) > 1048576:
        parser.error("payload exceeds --max-msg-bytes 1048576")
    return args


def executable(build, name):
    # Common CMake layouts; never select a Windows executable for a Linux run.
    for relative in (name, "tools/" + name, "bin/" + name):
        candidate = build / relative
        if candidate.is_file():
            return str(candidate.resolve())
    return str((build / name).resolve())


def commands(args, transport, mode, payload, count, port, directory, tag):
    server = [executable(args.build_dir, "bench_server"), "--transport", transport,
              "--mode", "static", "--bind", "127.0.0.1", "--data-port", str(port),
              "--io-threads", "6", "--handler-threads", "2", "--worker",
              "--echo-request", "--max-msg-bytes", "1048576",
              "--backpressure-bytes", "4194304"]
    clients = []
    for index in range(count):
        command = [executable(args.build_dir, "loadgen"), "--transport", transport,
                   "--target", "127.0.0.1:" + str(port), "--mode", "static",
                   "--service", "OrderService", "--method", "get",
                   "--duration-seconds", str(args.duration), "--payload-bytes", str(payload),
                   "--expect-response-bytes", str(payload), "--window", "1",
                   "--timeout-ms", "5000", "--connect-timeout-seconds", "20",
                   "--io-threads", "1", "--max-msg-bytes", "1048576",
                   "--backpressure-bytes", "4194304", "--frag-bytes", "1440",
                   "--worker-id", tag + "-client-" + str(index + 1),
                   "--output", str(directory / ("client-%d.json" % (index + 1)))]
        if mode == "sync":
            command.append("--sync")
        clients.append(command)
    return server, clients


def stop(process):
    if process is None:
        return None
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    return process.returncode


def choose_port(transport, used):
    kind = socket.SOCK_STREAM if transport == "tcp" else socket.SOCK_DGRAM
    # Bind before selecting, and never reuse a port even across transports/rounds.
    for _ in range(1000):
        with socket.socket(socket.AF_INET, kind) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port not in used:
            used.add(port)
            return port
    raise RuntimeError("cannot allocate a unique port")


def read_log(path):
    return path.read_text(encoding="utf-8", errors="replace")


def sample_ss(port, sample):
    if sample["status"] == "unavailable":
        return
    try:
        result = subprocess.run(["ss", "-H", "-tn", "state", "established",
                                 "(", "sport", "=", ":" + str(port), ")"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                encoding="utf-8", errors="replace", timeout=2)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or "ss returned nonzero")
        count = len([line for line in result.stdout.splitlines() if line.strip()])
        sample["status"] = "available"
        sample["peak"] = max(sample["peak"] or 0, count)
        sample["samples"] += 1
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        sample.update(status="unavailable", reason=str(error))


def nonnegative(value, label):
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError("invalid " + label)
    return value


def client_result(path, returncode):
    record = {"result_path": str(path), "returncode": returncode, "errors": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        record["raw"] = data
        succeeded = nonnegative(data["succeeded"], "succeeded")
        failed = nonnegative(data["failed"], "failed")
        timeouts = nonnegative(data["metrics"]["timeouts"], "timeouts")
        record.update(succeeded=succeeded, failed=failed, timeouts=timeouts)
        duration = data["duration_seconds"]
        if isinstance(duration, bool) or not isinstance(duration, (float, int)) or not math.isfinite(duration) or duration <= 0:
            raise ValueError("invalid duration_seconds")
        histogram = {}
        for key, value in data["latency_histogram_us"].items():
            bucket = int(key)
            if bucket < 0:
                raise ValueError("negative histogram bucket")
            histogram[bucket] = histogram.get(bucket, 0) + nonnegative(value, "histogram count")
        if not sum(histogram.values()):
            raise ValueError("empty latency histogram")
        record.update(succeeded=succeeded, failed=failed, timeouts=timeouts,
                      duration_seconds=duration, qps=succeeded / duration,
                      histogram=histogram)
        if data["status"] != "ok" or failed or timeouts or succeeded <= 0:
            record["errors"].append("status/count validation failed")
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        record["errors"].append(str(error))
    if returncode != 0:
        record["errors"].append("loadgen exit code: " + str(returncode))
    record["status"] = "FAIL" if record["errors"] else "PASS"
    return record


def percentiles(histogram):
    total = sum(histogram.values())
    result = {}
    for percentile in (50, 90, 95, 99):
        threshold = math.ceil(total * percentile / 100)
        cumulative = 0
        value = None
        for bucket, count in sorted(histogram.items()):
            cumulative += count
            if cumulative >= threshold and total:
                value = bucket
                break
        result["p%d_us" % percentile] = value
    return result


def run_cell(args, dimensions, round_number, used):
    transport, mode, payload, count = dimensions
    tag = "%s-%s-%d-%d-r%d" % (transport, mode, payload, count, round_number)
    directory = args.output_dir / tag
    directory.mkdir()
    port = choose_port(transport, used)
    server_cmd, client_cmds = commands(args, *dimensions, port, directory, tag)
    record = dict(transport=transport, api_mode=mode, payload_bytes=payload,
                  clients=count, client_kind="connections" if transport == "tcp" else "sessions",
                  round=round_number, port=port, duration_requested_seconds=args.duration,
                  window=1, server_command=server_cmd, client_commands=client_cmds,
                  configured_server=dict(worker=1, io_threads=6, handler_threads=2),
                  errors=[], client_results=[])
    sample = dict(status="pending" if transport == "tcp" else "not_applicable",
                  peak=None, samples=0)
    record["tcp_established"] = sample
    server = None
    processes = []
    handles = []
    log_path = directory / "server.log"
    record["server_log_path"] = str(log_path)
    try:
        log = log_path.open("wb")
        handles.append(log)
        server = subprocess.Popen(server_cmd, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 20
        while True:
            if server.poll() is not None:
                raise RuntimeError("server exited before readiness: %s" % server.returncode)
            text = read_log(log_path)
            ready = next((line for line in text.splitlines() if "ready:" in line), None)
            if ready:
                record["server_ready_line"] = ready
                fields = dict(re.findall(r"\b(\w+)=(\S+)", ready))
                record["observed_server"] = fields
                record["io_log_verification"] = "unavailable: ready log does not emit IO thread count"
                if fields.get("worker") != "1" or fields.get("handler_threads") != "2":
                    raise RuntimeError("ready log does not confirm worker=1 and handler_threads=2")
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("server ready timeout")
            time.sleep(0.05)
        # Start every independent client before waiting for any client.
        for index, command in enumerate(client_cmds):
            log = (directory / ("client-%d.log" % (index + 1))).open("wb")
            handles.append(log)
            processes.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT))
        deadline = time.monotonic() + args.duration + 60
        next_sample = 0.0
        while any(process.poll() is None for process in processes):
            if server.poll() is not None:
                raise RuntimeError("server exited during measurement: %s" % server.returncode)
            now = time.monotonic()
            if now > deadline:
                raise RuntimeError("loadgen completion timeout")
            if transport == "tcp" and now >= next_sample:
                sample_ss(port, sample)
                next_sample = time.monotonic() + 0.25
            time.sleep(0.05)
        if server.poll() is not None:
            raise RuntimeError("server exited before cleanup: %s" % server.returncode)
    except (OSError, RuntimeError) as error:
        record["errors"].append(str(error))
    finally:
        for process in processes:
            stop(process)
        server_was_running = server is not None and server.poll() is None
        record["server_returncode"] = stop(server)
        # SIGTERM is intentional; a natural nonzero exit is always a failure.
        if server is not None and record["server_returncode"] not in (0, -15):
            record["errors"].append("server nonzero exit: %s" % server.returncode)
        if server is not None and not server_was_running and server.returncode != 0:
            record["errors"].append("server failed before intentional shutdown")
        for handle in handles:
            handle.close()
        record["server_startup_log"] = read_log(log_path) if log_path.exists() else ""
    for index, command in enumerate(client_cmds):
        code = processes[index].returncode if index < len(processes) else None
        result = client_result(Path(command[command.index("--output") + 1]), code)
        result["command"] = command
        record["client_results"].append(result)
    histogram = {}
    for result in record["client_results"]:
        for bucket, value in result.get("histogram", {}).items():
            histogram[bucket] = histogram.get(bucket, 0) + value
    record.update(percentiles(histogram))
    record["latency_histogram_us"] = histogram
    record["aggregate_qps"] = sum(item.get("qps", 0) for item in record["client_results"])
    for field in ("failed", "timeouts"):
        record[field] = sum(item.get(field, 0) for item in record["client_results"])
    record["counts_complete"] = all("failed" in item and "timeouts" in item for item in record["client_results"])
    record["status"] = "FAIL" if record["errors"] or any(item["status"] == "FAIL" for item in record["client_results"]) else "PASS"
    return record


def write_csv(path, rows):
    if not rows:
        return
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({key: json.dumps(value, ensure_ascii=False) if isinstance(value, (list, dict)) else value
                             for key, value in row.items()})


def save(args, records):
    summaries = []
    keys = ("transport", "api_mode", "payload_bytes", "clients")
    for dimensions, group in itertools.groupby(sorted(records, key=lambda row: tuple(row[key] for key in keys)),
                                                key=lambda row: tuple(row[key] for key in keys)):
        rounds = list(group)
        successful = [row for row in rounds if row["status"] == "PASS"]
        summary = dict(zip(keys, dimensions))
        summary.update(client_kind=rounds[0]["client_kind"], rounds=len(rounds),
                       passed_rounds=len(successful), failed_rounds=len(rounds) - len(successful),
                       status="PASS" if len(successful) == len(rounds) else "FAIL")
        for metric in METRICS:
            values = [row[metric] for row in successful if row[metric] is not None]
            for name, operation in (("min", min), ("median", statistics.median), ("max", max)):
                summary[metric + "_" + name] = operation(values) if values else None
        summaries.append(summary)
    report = dict(qps_note=NOTE, percentile_note="Merged bucket counts; nearest-rank bucket values in microseconds",
                  summary_statistics="PASS rounds only; failed rounds retained in raw rounds",
                  arguments={key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
                  summaries=summaries, rounds=records)
    (args.output_dir / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_csv(args.output_dir / "summary.csv", summaries)
    write_csv(args.output_dir / "rounds.csv", [{key: value for key, value in row.items() if key != "client_results"} for row in records])
    clients = []
    for row in records:
        for index, result in enumerate(row["client_results"], 1):
            clients.append(dict({key: row[key] for key in keys}, round=row["round"], client_index=index, **result))
    write_csv(args.output_dir / "clients.csv", clients)


def main():
    args = parse_args()
    args.build_dir = args.build_dir.resolve()
    args.output_dir = args.output_dir.resolve()
    matrix = list(itertools.product(args.transports, ("sync", "async"), args.payloads, args.clients))
    total = len(matrix) * args.rounds
    print("%d cells x %d rounds = %d runs; measurement estimate %.1f minutes (+ startup/cleanup)" %
          (len(matrix), args.rounds, total, total * args.duration / 60))
    print(NOTE)
    if args.dry_run:
        for number, (dimensions, round_number) in enumerate(itertools.product(matrix, range(1, args.rounds + 1)), 1):
            if number > 45535:
                raise RuntimeError("too many runs for unique dry-run ports")
            tag = "%s-%s-%d-%d-r%d" % (*dimensions, round_number)
            server, clients = commands(args, *dimensions, 20000 + number, args.output_dir / tag, tag)
            print("\n[%d/%d] %s (%s)" % (number, total, tag, "connections" if dimensions[0] == "tcp" else "sessions"))
            for command in [server] + clients:
                print(shlex.join(command))
        return 0
    for name in ("bench_server", "loadgen"):
        if not Path(executable(args.build_dir, name)).is_file():
            raise RuntimeError("missing executable: " + executable(args.build_dir, name))
    # Refuse overwriting previous raw results.
    args.output_dir.mkdir(parents=True, exist_ok=False)
    records = []
    used = set()
    try:
        for dimensions, round_number in itertools.product(matrix, range(1, args.rounds + 1)):
            record = run_cell(args, dimensions, round_number, used)
            records.append(record)
            save(args, records)
            print("[%d/%d] %s %s payload=%d clients=%d %s QPS=%.3f" %
                  (len(records), total, record["status"], dimensions[0], dimensions[2],
                   dimensions[3], dimensions[1], record["aggregate_qps"]), flush=True)
    finally:
        if records:
            save(args, records)
    return 1 if any(row["status"] == "FAIL" for row in records) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Interrupted; subprocesses cleaned up.", file=sys.stderr)
        sys.exit(130)
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
