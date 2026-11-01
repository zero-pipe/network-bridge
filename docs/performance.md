# Low Latency and Millisecond Spike Control

This document answers three questions: how to **detect** latency spikes, how to
**attribute** them, and how to **remove** them. It also records approaches that
look plausible but do not work in practice. Every number below can be
reproduced with `tools/loadgen` (including `--spike-log`) and the commands in
this document.

## Summary

1. Spikes do not depend on payload size or transport implementation: 64 B and
   1 KB payloads have the same p99/max level.
2. The removable portion comes from **thread scheduling**: CPU pinning plus
   inline dispatch lowers the mean by 40-50% and p99 by 30-50%, and raises
   throughput by more than 60%.
3. Residual spikes (1-8 ms) account for only 0.003-0.01% of samples. They are
   randomly distributed, have no fixed period, and **appear even in a pure
   busy-wait program on an idle machine**. They come from vCPU preemption or
   host scheduling stalls and cannot be removed inside the process; they
   require cpuset isolation and real-time scheduling.
4. Adding a spin loop to the synchronous wait path **measurably makes things
   worse** (p50 38 us -> 46-58 us), because the poller thread that delivers the
   completion usually shares a core with the caller. That experiment was
   reverted and is not in the code.

## Measurement method

Common setup: loopback, TCP, 1 KB request/response, `--window 1` (strictly one
request in flight), `bench_server` on the server, client and server pinned to
different physical cores, built with `CMAKE_BUILD_TYPE=Debug` (relative
differences are more trustworthy than absolute values).

```bash
# server
taskset -c 0 build-linux/tools/bench_server --transport tcp --bind 127.0.0.1 --data-port 9001 \
  --mode static --io-threads 6 --handler-threads 2 --response-bytes 1024

# client (--spike-log records samples above the threshold together with their time)
taskset -c 2 build-linux/tools/loadgen --transport tcp --target 127.0.0.1:9001 \
  --mode static --service OrderService --method get --duration-seconds 30 \
  --payload-bytes 1024 --expect-response-bytes 1024 --window 1 \
  --spike-log spikes.csv --spike-threshold-us 1000 --output result.json
```

`result.json` now includes `slow_samples` (the number of samples above
`--spike-threshold-us`) and `slow_threshold_us`, so a gate can budget tail
latency directly. `spikes.csv` records `t_us,latency_us`, which shows whether
spikes are periodic.

## Measurements

Five-second rounds (synchronous API path, 1 KB, Debug):

| Configuration | QPS | mean | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| worker pool, no pinning | 10,664 | 93.4us | 80us | 235us | 4.08ms |
| worker pool, pinned | 14,955 | 66.7us | 50us | 172us | 2.30ms |
| inline dispatch, pinned | 18,135 | 54.9us | 44us | 130us | 3.39ms |

Ten-second rounds (`--window 1` asynchronous path, 1 KB, Debug):

| Configuration | QPS | mean | p50 | p99 | p999 | over 1 ms |
| --- | --- | --- | --- | --- | --- | --- |
| inline, pinned | 21,162 | 47.1us | 41us | 107us | 272us | 11 / 211,625 (0.005%) |
| worker, pinned | 16,681 | 59.8us | 51us | 152us | 344us | 12 / 166,809 (0.007%) |
| inline, unpinned | 18,173 | 54.8us | 51us | 141us | 348us | 7 / 181,736 (0.004%) |

Removing the worker pool saves one cross-thread wake-up; pinning removes
migration and cache jitter. The gains are close to additive.

Thirty-second soak (inline, pinned, 1 KB, default `TICK_INTERVAL_MS=2`):

| Run | QPS | mean | p99 | max | over 1 ms |
| --- | --- | --- | --- | --- | --- |
| round 1 | 23,759 | 41.9us | 103us | 2.72ms | 26 / 712,770 (0.0036%) |
| round 2 | 22,438 | 44.4us | 100us | 7.78ms | 37 / 673,130 (0.0055%) |
| `TICK_INTERVAL_MS=20` | 22,727 | 43.8us | 98us | 3.62ms | 22 / 681,814 (0.0032%) |

## Spike attribution experiments

| Experiment | Result | Interpretation |
| --- | --- | --- |
| 64 B vs 1 KB | p99 296us vs 279us, max 2.3ms vs 2.6ms | payload size is not the source |
| `TICK_INTERVAL_MS=20` vs default 2 ms | 0.81/s vs 0.87/s, 1.30/s | timer ticks are not the source |
| 30-second window statistics | 0.0032-0.0055% of samples > 1 ms | stable order of magnitude |
| Periodicity test (phase concentration + Monte Carlo p-value) | max concentration 0.50-0.53 (a true period would approach 1.0), p=0.37 / 0.63 | no stable period; intervals are close to Poisson |
| Pure busy-wait baseline (no RPC) | 2 stalls >= 1 ms in 60 s (worst 2.76 ms) | the host itself has scheduling stalls |
| Rate limited to 2000 QPS (30 s) | 9 / 60,001 samples > 1 ms (0.015%) | spikes follow wall-clock time, not throughput |

The second-to-last row is the decisive evidence: a single-threaded program that
only busy-waits for 100 us, with no networking and no RPC, still saw 1-3 ms
preemption stalls within 60 seconds on an idle machine. No amount of in-process
hot-path tuning can remove that tail.

## Mitigations and validation

Ordered by benefit:

1. **Pin or isolate CPUs.** Use `taskset -c` or a cgroup `cpuset` to give the
   server and client dedicated cores. Measured: QPS +40% or more, p99 down about
   30%. Production deployments can go further with `isolcpus=`/`nohz_full=` and
   IRQ affinity.
2. **Use inline dispatch for latency-sensitive methods.** Set
   `handler_threads=0` (or disable the worker pool in the equivalent
   configuration) to remove one thread hand-off. Measured: mean -18%, p99 -20%.
3. **Raise scheduling priority.** `SCHED_FIFO` plus `mlockall` removes
   millisecond stalls caused by CFS preemption; it requires root, which the
   validation environment did not have.
4. **Trade window depth for tail latency.** With `window>1` a single stall
   affects only one frame and throughput can exceed 80k QPS, at the cost of
   queueing delay per request.

### Verified ineffective

A bounded 50 us spin before `ntk_sem_timedwait` in `nb_proxy_call_sync`, intended
to avoid the futex wake-up:

| Configuration | no spin p50 / mean | 50 us spin p50 / mean |
| --- | --- | --- |
| inline, one core pinned | 44us / 54.9us | 58us / 69.5us |
| inline, two cores pinned | 45us / 53.3us | 53us / 66.3us |

The poller thread that delivers completion events lives in the same process and
shares a core (or cgroup quota) with the spinner, so spinning starves the
waker and the wake-up arrives later. The change was rolled back.

## Reproducing and gating

```bash
python3 tools/gate.py perf --perf-duration 5 --payload-bytes 1024
```

To investigate spikes, collect them with
`loadgen --spike-log <csv> --spike-threshold-us 1000`, then analyse the interval
distribution and periodicity against `t_us`. A gate can read `slow_samples`
from `result.json` and set a tail-latency budget (for example, no more than 10
in a 1 KB / 5 s scenario).
