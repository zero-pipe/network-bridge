# 测试与质量门禁

仓库只有一个门禁入口：`tools/gate.py`。CTest 运行进程内测试；`gate.py`
驱动进程级 e2e、性能和稳定性场景，并把所有结果写到 `artifacts/gate/` 下。

## 分类

| 门禁 | CTest 标签 | 覆盖范围 | 命令 |
| --- | --- | --- | --- |
| unit | `unit` | 纯模块、codec、配置、组帧、指标、registry | `python3 tools/gate.py unit` |
| e2e | `e2e` | 进程内 loopback/stream/api/worker/semantics、独立 server/client echo，以及 DDS participant/topic/writer/reader（rtps，DDS 开启时 cyclone） | `python3 tools/gate.py e2e` |
| perf | 无 | tcp/udp/shm 吞吐与延迟冒烟，可选基线回归 | `python3 tools/gate.py perf` |
| stability | 无 | 长跑 server、多轮不同载荷大小、失败与资源增长检查 | `python3 tools/gate.py stability` |

命名规则：

- CTest 名称为 `<category>.<case>`，例如 `unit.framing`、`e2e.loopback`。
- CTest 标签同时带分类和组件，例如 `unit;framing`、`e2e;worker`。
- `integration` 不是顶层分类；端到端行为统一归为 `e2e`。
- C 测试可执行文件保留 `test_*` 前缀；压测工具使用 `bench_*` 和
  `loadgen`。
- `tools/gate.py` 是唯一的门禁入口；不要另外增加临时 shell 脚本。

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DNB_BUILD_TESTS=ON -DNB_BUILD_TOOLS=ON
python3 tools/gate.py unit
python3 tools/gate.py e2e --transports tcp,udp,dds,rtps
python3 tools/gate.py perf --perf-duration 10
python3 tools/gate.py stability --transports tcp --stability-duration 60
```

`gate.py` 默认先执行 `cmake --build`；目录已经构建过时加 `--no-build`。
Windows 多配置生成器需要显式指定配置：

```powershell
python tools\gate.py unit --config Debug
python tools\gate.py e2e --config Debug --transports tcp,udp,dds,rtps
```

也可以直接使用 CMake 目标（会先构建所需的测试/工具）：

```bash
cmake --build build --target gate-unit
cmake --build build --target gate-e2e
cmake --build build --target gate-perf
cmake --build build --target gate-stability
cmake --build build --target gate-all
```

运行所有默认门禁：

```bash
python3 tools/gate.py all
```

`all` 使用较短时长：perf 每种传输 5 秒，stability 30 秒（仅 TCP）。需要完整
稳定性矩阵时请显式指定：

```bash
python3 tools/gate.py stability --transports tcp,udp,dds --stability-duration 300
```

## 通过标准

### unit

- `unit` 标签至少有一个用例。
- 每个用例退出码为 0。
- 不允许静默跳过；`NB_BUILD_TESTS=OFF` 会直接判定门禁失败，并提示重新配置。

### e2e

- 所有 `e2e` 用例通过。
- 每种传输至少完成 `--e2e-requests` 个请求（默认 200）。
- `status=ok`、`failed=0`，响应字节数等于成功请求数乘以预期响应大小。
- 场景结束前 server 不能退出。
- `NB_ENABLE_DDS=OFF` 时自动跳过 DDS，不算失败。

### perf

- 每种传输报告 `status=ok`、`failed=0`、QPS > 0。
- 默认只记录结果，不设置依赖硬件的阈值。
- 指定 `--baseline` 时比较 QPS 和 p99：

```bash
# 生成基线
python3 tools/gate.py perf --update-baseline --baseline tests/perf/baseline.json

# 按基线检查，允许 20% 波动
python3 tools/gate.py perf --baseline tests/perf/baseline.json --tolerance 0.20
```

基线依赖机器。只提交来自固定 CI 或基准测试机的基线，或者按机器放在
`artifacts/` 下。

### stability

- server 全程存活，轮与轮之间不重启。
- 每一轮都报告 `status=ok`、`failed=0`，且请求数大于 0。
- server 不能提前崩溃。
- Linux 上默认检查以下资源增长：
  - RSS 峰值增长 <= 64 MiB
  - 线程数增长 <= 8
  - FD 峰值增长 <= 32
- Windows 目前跳过 `/proc` 检查，只验证失败数和进程存活。

需要时可以覆盖阈值：

```bash
python3 tools/gate.py stability \
  --max-rss-growth-mb 128 --max-thread-growth 16 --max-fd-growth 64
```

## 产物

每次运行都会生成一个带时间戳的目录：

```text
artifacts/gate/<UTC timestamp>/
  summary.json              总体状态以及各门禁的 status/detail/metrics
  unit/
    list.log                ctest -N -L unit 的输出
    ctest.log               ctest -L unit 的完整输出
  e2e/
    summary.json
    ctest/ctest.log
    tcp/{server.log,client.log,result.json,server.exit}
    udp/...
    dds/...
    rtps/...
  perf/
    summary.json
    metrics.json            每种传输的 QPS / p50 / p90 / p99
    tcp/{server.log,client.log,result.json}
  stability/
    tcp/summary.json
    tcp/wave_0_64b/...
    tcp/wave_1_1384b/...
```

顶层 `status` 为 `pass` 或 `fail`；子门禁的 `SKIP` 不会导致整体失败。失败时
脚本退出码为 1，供 CI 消费。

## 直接使用 CTest

```bash
cd build
ctest -N -L unit          # 列出 unit 用例
ctest -L unit             # 只跑 unit
ctest -L e2e              # 只跑进程内 e2e
ctest -R unit.framing     # 跑单个用例
```

### Sanitizer 构建

通过 `NB_SANITIZE` 启用 ASAN / UBSAN / TSAN，需要 GCC 或 Clang（MSVC 会
快速失败）：

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

`unit.buf_concurrency` 覆盖共享缓冲区的并发 ref/release、批量 `nb_buf_ref_n`
和池化回收；建议在 TSAN 下运行以检查引用计数数据竞争。

Windows 多配置：

```powershell
cd build
ctest -C Debug -L unit
ctest -C Debug -L e2e
```

## 新增测试

单元测试：

1. 新增 `tests/test_foo.c`，沿用现有的 `main()` + 退出码风格。
2. 在 `tests/CMakeLists.txt` 中用公共 helper 注册：
   `nb_add_test(test_foo unit.foo LABELS "unit;foo" TIMEOUT 120)`。
3. 只有测试需要访问 `src/` 时才添加 include 目录。

进程内 e2e：

1. 新增 `tests/test_foo_e2e.c`。
2. 使用 `e2e.foo` 测试名和 `e2e;foo` 标签。
3. 需要真实 socket 时，在专用端口上绑定 `127.0.0.1`，返回前销毁所有 node。

进程级 e2e / perf / stability：

- 在 `tools/gate.py` 中增加 case 函数，复用 `server_command`、
  `loadgen_command`、`run_scenario` 和 `evaluate_result`。
- 不要再新增独立 shell 脚本，否则产物路径和失败标准又会分散。

