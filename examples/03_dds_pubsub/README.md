# 03 DDS 发布/订阅

和常见 DDS 应用一样的对象模型：`participant → topic → writer / reader`。

```bash
./example_03_dds_pubsub          # dds://0，Cyclone DDS
./example_03_dds_pubsub rtps     # rtps://0，内建 RTPS 引擎
```

要点：

- `nb_topic_create(participant, "chat", "text", qos, &topic)`：topic 名就是
  线上名字，不会被加上任何前缀或后缀。
- reader 有两种：回调（`on_data` 非空）和 history（`on_data` 为空、depth
  非 0，用 `nb_reader_read` / `nb_reader_take` 取）。
- QoS 写在 binding 的 `ps_qos` 上（或作为 `pubsub.default_qos` 让所有 binding
  继承）；TCP/UDP 的 RPC 链路不读取这些策略。
- 同一个 topic 可以同时有多个 reader；provider 只建一条底层订阅，核心按
  注册的 reader 分发。
- 换成 `shm` backend 时把 binding 改成 `shm://<channel>:<id>`，IPC 零拷贝；
  `shm` 目前只在 Linux 上构建。
- 想让两个进程互通：双方使用相同的 domain 和 topic 名即可，Cyclone DDS /
  RTPS 的发现会自动匹配；`shm` 则需要同一 channel 且 node id 不同。
