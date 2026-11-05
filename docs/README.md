# Network Bridge 文档

- [SDK API](api/sdk-api.md)：对象模型、生命周期、线程模型与所有权。
- [架构](architecture.md)：endpoint / message / QoS / mesh / provider 的分层。
- [架构总览](architecture-overview.html)：单页中文概览——ntk 运行时、服务发现、
  UDP/TCP/SHM、RTPS 与三方依赖，带分层和数据路径图。
- [RTPS 协议手册](rtps-protocol.html)：用大白话讲报文格式、SPDP/SEDP 流程、QoS
  匹配和可靠传输（单文件 HTML，浏览器直接打开）。
- [RTPS 实现规范](rtps-spec.md)：从 `src/rtps/` 反推出来的规范参考——报文与
  ParameterList 格式、发现、QoS、可靠性、分片、上限以及与 Cyclone DDS 的互通矩阵。
- [RTPS 公共接口](rtps-api.md)：对象模型、QoS 继承、read/take 语义、线程约束，
  以及和 Cyclone DDS `dds_*` 的对应关系。
- [RTPS 标准对照](rtps-standard-alignment.md)：对照 OMG DDSI-RTPS 2.5 检查
  实现程度，列出与 Cyclone DDS / Fast DDS 的互通矩阵、差距和兼容性取舍。
- [配置](api/config.md)：JSON 字段、默认值与校验规则。
- [错误码](api/error-codes.md)：稳定数值、名称、描述与重试语义。
- [示例](examples.md)：按场景索引的可运行程序。
- [测试](testing.md)：统一的 unit / e2e / perf / stability / fuzz 质量门禁。
- [低延迟与尖峰](performance.md)：延迟测量方法、尖峰归因实验、CPU 绑核与内联
  分发的结果，以及各传输与 gRPC 的对比。

配置文件的编辑器校验使用 [`config.schema.json`](config.schema.json)。

设计取舍与分层理由汇总在仓库根目录的 [README](../README.md) 中。
