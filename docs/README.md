# Network Bridge 文档

- [SDK API](api/sdk-api.md)：对象模型、生命周期、线程模型与所有权。
- [架构](architecture.md)：两条并列主干（RPC / DDS）+ 共享底座、库目标和契约。
- [RTPS 公共接口](rtps-api.md)：内建 RTPS 引擎的对象模型、QoS、read/take 与线程约束。
- [配置](api/config.md)：JSON 字段、默认值与校验规则。
- [错误码](api/error-codes.md)：稳定数值、名称、描述与重试语义。
- [示例](examples.md)：按场景索引的可运行程序。
- [测试](testing.md)：统一的 unit / e2e / perf / stability 质量门禁。

配置文件的编辑器校验使用 [`config.schema.json`](config.schema.json)。
