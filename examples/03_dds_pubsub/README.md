# 03 DDS 发布/订阅

DDS 是独立的通信面：`participant → topic → writer / reader`，不依赖 RPC node、
binding、路由或 zn_discovery。

```bash
./example_03_dds_pubsub          # 内建 RTPS 后端（唯一后端）
```

要点：

- `nb_dds_participant_create({backend, domain, partition, prefix, default_qos})`
  创建 domain；后端自己做 SPDP/SEDP 发现。
- `nb_dds_topic_create(participant, "chat", "text", qos, &topic)`：`chat` 是应用
  名，线上名会带上 participant 的 `prefix`（本示例为 `example/chat`）。
- reader 有两种：回调（`on_data` 非空）和 history（`on_data` 为空，用
  `nb_dds_reader_read` / `nb_dds_reader_take` 取，`take` 后调用
  `nb_dds_sample_release`）。
- QoS 写在 participant 默认值、topic、writer/reader 上，逐级继承；使用统一的
  DDS 默认值。
- 同一个 participant 上的 writer/reader 由框架做进程内投递，后端只负责远端；
  两个进程互通时使用相同的 domain、partition 和 topic 名即可。
- `backend` 固定为 `NB_DDS_BACKEND_RTPS`：内建 RTPS 是唯一后端，线上格式与
  Cyclone DDS / Fast DDS 互通。
