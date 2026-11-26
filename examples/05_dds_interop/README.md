# 05 DDS 互通示例（RTPS ↔ Cyclone DDS）

用同一个 IDL 类型 `Trade` 演示内建 RTPS 引擎与 Cyclone DDS 的双向互通，覆盖
严格类型一致性（XTypes TypeInformation）场景。

| 程序 | 角色 |
| --- | --- |
| `example_05_nb_pub` | network-bridge 发布（原始 CDR + TypeInformation） |
| `example_05_nb_sub` | network-bridge 订阅 |
| `example_05_cyl_pub` | Cyclone DDS 发布 |
| `example_05_cyl_sub` | Cyclone DDS 订阅（`strict` 参数开启严格类型一致性） |

类型：

```idl
struct Trade { string symbol; double price; long volume; };
```

network-bridge 侧不依赖生成代码：`trade_cdr.h` 手写编解码，`trade_ti.h` 携带
idlc 为这份 IDL 生成的 TypeInformation（SEDP 以 `PID_TYPE_INFORMATION` 通告）。
Cyclone 侧由 `idlc` 从 `Trade.idl` 现场生成 `Trade.c/.h`。

## 构建

```sh
cmake -B build -DNB_BUILD_EXAMPLES=ON
cmake --build build
```

- `example_05_nb_pub` / `example_05_nb_sub` 总是构建。
- `example_05_cyl_pub` / `example_05_cyl_sub` 仅在找到 Cyclone DDS 库与 `idlc`
  时构建（`find_package(CycloneDDS)` + `idlc` 在 PATH 或 `CYCLONEDDS_HOME/bin`）。
  没有 Cyclone 的机器只构建 network-bridge 侧，不影响其余示例。

## 运行（默认 domain 42）

四种组合，各开两个终端：

```sh
# 1) network-bridge 发布 -> Cyclone 订阅（严格类型一致性）
./example_05_nb_pub 42
./example_05_cyl_sub 42 strict      # 输出 CYCLONE_MATCHED=1 / CYCLONE_GOT ...

# 2) Cyclone 发布 -> network-bridge 订阅
./example_05_cyl_pub 42
./example_05_nb_sub 42 20           # 输出 GOT AAPL ...

# 3) Cyclone 发布 -> Cyclone 订阅
./example_05_cyl_pub 42
./example_05_cyl_sub 42

# 4) network-bridge 发布 -> network-bridge 订阅
./example_05_nb_pub 42
./example_05_nb_sub 42 20
```

## 说明

- 发布端非 `dds::Message` 的 topic 传真实类型名（`"Trade"`）即按原始 CDR body
  发送，编解码由应用负责（此处为 `trade_cdr.h`，真实项目用 idlc 生成代码）。
- `trade_ti.h` 的字节与 `Trade.idl` 绑定：改 IDL 必须用 idlc 重新生成，否则严格
  对端会拒绝匹配（等价哈希不一致）。
- 保持 participant 的 `prefix` 为空，使线缆 topic 名恰为 `Trade`，与 Cyclone 对齐。
