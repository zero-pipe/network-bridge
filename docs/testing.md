# Testing and Quality Gates

The repository has a single gate entry point: `tools/gate.py`. CTest runs
in-process tests; `gate.py` drives process-level e2e, performance and stability
scenarios and writes every result under `artifacts/gate/`.

## Categories

| Gate | CTest label | Coverage | Command |
| --- | --- | --- | --- |
| unit | `unit` | pure modules, codecs, config, framing, metrics, registry | `python3 tools/gate.py unit` |
| e2e | `e2e` | in-process loopback/stream/api/worker/semantics, separate server/client echo, and a separate DDS publisher/Cyclone subscriber | `python3 tools/gate.py e2e` |
| perf | none | TCP/UDP/DDS throughput and latency smoke, optional baseline regression | `python3 tools/gate.py perf` |
| stability | none | long-running server, multiple waves with varying payload sizes, failure and resource growth checks | `python3 tools/gate.py stability` |
| fuzz | `fuzz` | libFuzzer smoke (only with `NB_BUILD_FUZZERS=ON`) | `python3 tools/gate.py fuzz` |

Naming rules:

- CTest names are `<category>.<case>`, for example `unit.framing` and
  `e2e.loopback`.
- CTest labels carry both category and component, for example `unit;framing`
  and `e2e;worker`.
- `integration` is not a top-level category; end-to-end behaviour is always
  `e2e`.
- C test executables keep the `test_*` prefix; load tools use `bench_*` and
  `loadgen`.
- `tools/gate.py` is the only gate entry point; do not add ad-hoc shell
  scripts.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DNB_BUILD_TESTS=ON -DNB_BUILD_TOOLS=ON
python3 tools/gate.py unit
python3 tools/gate.py e2e --transports tcp,udp,dds
python3 tools/gate.py perf --perf-duration 10
python3 tools/gate.py stability --transports tcp --stability-duration 60
```

`gate.py` runs `cmake --build` first by default; pass `--no-build` when the
tree is already built. Multi-config generators on Windows need an explicit
configuration:

```powershell
python tools\gate.py unit --config Debug
python tools\gate.py e2e --config Debug --transports tcp,udp,dds
```

CMake targets are also available (they build the required tests/tools first):

```bash
cmake --build build --target gate-unit
cmake --build build --target gate-e2e
cmake --build build --target gate-perf
cmake --build build --target gate-stability
cmake --build build --target gate-all
```

Run all default gates:

```bash
python3 tools/gate.py all
```

`all` uses short durations: perf 5 s per transport, stability 30 s (TCP only).
For the full stability matrix, be explicit:

```bash
python3 tools/gate.py stability --transports tcp,udp,dds --stability-duration 300
```

## Pass criteria

### unit

- The `unit` label has at least one case.
- Every case exits with 0.
- No case is silently skipped; `NB_BUILD_TESTS=OFF` fails the gate and asks for
  a reconfigure.

### e2e

- All `e2e` cases pass.
- Each transport completes at least `--e2e-requests` requests (default 200).
- `status=ok`, `failed=0`, and response bytes equal successful requests times
  the expected response size.
- The server does not exit before the scenario ends.
- DDS is skipped automatically when `NB_ENABLE_DDS=OFF`; this is not a failure.

### perf

- Each transport reports `status=ok`, `failed=0`, QPS > 0.
- By default results are recorded without hardware-dependent thresholds.
- With `--baseline`, QPS and p99 are compared:

```bash
# create a baseline
python3 tools/gate.py perf --update-baseline --baseline tests/perf/baseline.json

# check against it, allowing 20% variance
python3 tools/gate.py perf --baseline tests/perf/baseline.json --tolerance 0.20
```

Baselines are machine-dependent. Commit only baselines from a fixed CI or
benchmark host, or keep them under `artifacts/` per machine.

### stability

- The server stays up for the whole run and is not restarted between waves.
- Every wave reports `status=ok`, `failed=0` and a positive request count.
- The server does not crash early.
- On Linux the following resource growth limits apply (defaults):
  - RSS peak growth <= 64 MiB
  - thread count growth <= 8
  - FD peak growth <= 32
- Windows currently skips `/proc` checks and only verifies failures and
  process liveness.

Override thresholds when needed:

```bash
python3 tools/gate.py stability \
  --max-rss-growth-mb 128 --max-thread-growth 16 --max-fd-growth 64
```

### fuzz

- `fuzz` tests exist only with `NB_BUILD_FUZZERS=ON` and a Clang toolchain.
- All `fuzz` cases pass; the gate reports `SKIP` when no fuzz tests exist.

## Artifacts

Each run writes a timestamped directory:

```text
artifacts/gate/<UTC timestamp>/
  summary.json              overall status plus per-gate status/detail/metrics
  unit/
    list.log                output of ctest -N -L unit
    ctest.log               full output of ctest -L unit
  e2e/
    summary.json
    ctest/ctest.log
    tcp/{server.log,client.log,result.json,server.exit}
    udp/...
    dds/...
  perf/
    summary.json
    metrics.json            QPS / p50 / p90 / p99 per transport
    tcp/{server.log,client.log,result.json}
  stability/
    tcp/summary.json
    tcp/wave_0_64b/...
    tcp/wave_1_1384b/...
```

Top-level `status` is `pass` or `fail`; a `SKIP` in a sub-gate does not fail the
run. On failure the script exits with 1, which is what CI consumes.

## Using CTest directly

```bash
cd build
ctest -N -L unit          # list unit cases
ctest -L unit             # run unit only
ctest -L e2e              # run in-process e2e only
ctest -R unit.framing     # run a single case
```

### Sanitizer builds

ASAN / UBSAN / TSAN are enabled with `NB_SANITIZE` and require GCC or Clang
(MSVC fails fast):

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DNB_BUILD_TESTS=ON -DNB_BUILD_TOOLS=OFF -DNB_BUILD_EXAMPLES=OFF \
  -DNB_SANITIZE=address,undefined
cmake --build build-asan --parallel 8
ctest -L unit --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DNB_BUILD_TESTS=ON -DNB_BUILD_TOOLS=OFF -DNB_BUILD_EXAMPLES=OFF \
  -DNB_SANITIZE=thread
cmake --build build-tsan --parallel 8
ctest -L unit --output-on-failure
```

`unit.buf_concurrency` covers concurrent ref/release of shared buffers, batched
`nb_buf_ref_n` and pooled recycling; run it under TSAN to check refcount data
races.

Multi-config on Windows:

```powershell
cd build
ctest -C Debug -L unit
ctest -C Debug -L e2e
```

## Adding tests

Unit tests:

1. Add `tests/test_foo.c`, following the existing `main()` + exit-code style.
2. Register it in `tests/CMakeLists.txt` with the shared helper:
   `nb_add_test(test_foo unit.foo LABELS "unit;foo" TIMEOUT 120)`.
3. Add include directories only when the test reaches into `src/`.

In-process e2e:

1. Add `tests/test_foo_e2e.c`.
2. Use the `e2e.foo` test name and `e2e;foo` labels.
3. When a real socket is needed, bind `127.0.0.1` on a dedicated port and
   destroy every node before returning.

Process-level e2e / perf / stability:

- Add a case function in `tools/gate.py` and reuse `server_command`,
  `loadgen_command`, `run_scenario` and `evaluate_result`.
- Do not add separate shell scripts; that would scatter artifact paths and
  failure criteria again.

## CI

`.github/workflows/ci.yml` runs three jobs:

- `linux`: GCC, Release, build plus `ctest -L unit` and `ctest -L e2e`.
- `windows`: MSVC, Debug, build plus `ctest -L unit`.
- `sanitizers`: GCC with `NB_SANITIZE=address,undefined`, DDS disabled, unit
  and e2e tests.

CI performance numbers are not cross-machine hard thresholds; CI only verifies
that the pipelines run without failures or crashes.
