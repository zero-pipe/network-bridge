#ifndef NB_RTPS_PARTICIPANT_PRIV_H
#define NB_RTPS_PARTICIPANT_PRIV_H

/*
 * RTPS 参与者内部共享定义。
 *
 * 参与者拆成两个编译单元：
 *   - rtps_participant.c  生命周期、socket、线程节拍、发现（SPDP/SEDP）、
 *                         端点匹配、deadline / liveliness 检查
 *   - rtps_datapath.c     用户数据面：DATA / DATA_FRAG 收发、GAP / ACKNACK /
 *                         HEARTBEAT、分片重组与向本地订阅投递
 *
 * 两者共享同一套参与者形态（表、计数、定时器）和少量工具函数，集中在本头。
 * 仅供引擎内部使用，不安装。
 */

#include "rtps_participant.h"
#include "rtps_reliable.h"
#include "rtps_sedp.h"
#include "rtps_spdp.h"
#include "rtps_tcp.h"
#include "rtps_types.h"

#include <zn/zn.h>

#include <stddef.h>
#include <stdint.h>

#define NB_RTPS_SPDP_BASE_PORT 7400u
#define NB_RTPS_SPDP_DOMAIN_GAIN 250u
/* RTPS 2.5 9.6.1.4 port mapping: d0 multicast meta, d1 unicast meta,
 * d2 multicast user, d3 unicast user; participant gain PG. */
#define NB_RTPS_PORT_OFFSET_D0 0u
#define NB_RTPS_PORT_OFFSET_D1 10u
#define NB_RTPS_PORT_OFFSET_D2 1u
#define NB_RTPS_PORT_OFFSET_D3 11u
#define NB_RTPS_PORT_PARTICIPANT_GAIN 2u
#define NB_RTPS_DEFAULT_ANNOUNCE_MS 3000u
#define NB_RTPS_DEFAULT_LEASE_MS 20000u
#define NB_RTPS_MIN_ANNOUNCE_MS 100u
#define NB_RTPS_MIN_LEASE_MS 1000u
#define NB_RTPS_MAX_LEASE_MS 3600000u
#define NB_RTPS_SWEEP_INTERVAL_MS 200u
#define NB_RTPS_PACKET_MAX 1400u
/* Datagrams we accept. Outgoing messages stay within NB_RTPS_PACKET_MAX, but a
 * peer may pack a whole sample into one DATA_FRAG datagram, so receiving uses
 * the largest legal UDP payload. */
#define NB_RTPS_RX_MAX 65536u
/* Datagrams drained per recvmmsg batch (bounded to keep the RX arena modest:
 * NB_RTPS_RX_BATCH * NB_RTPS_RX_MAX bytes of heap). */
#define NB_RTPS_RX_BATCH 8u
#define NB_RTPS_SPDP_PAYLOAD_MAX 1200u

/* Incoming DATA_FRAG reassembly. Four concurrent samples, at most 4 MiB of
 * sample bytes in flight, and a sample that stops arriving is dropped after
 * two seconds. The budget is larger than one maximum-size sample so a peer may
 * use a different fragment size and still overlap two large samples. */
#define NB_RTPS_FRAG_MAX_ACTIVE 4u
#define NB_RTPS_FRAG_BUDGET_BYTES (4u * 1024u * 1024u)
#define NB_RTPS_FRAG_TIMEOUT_MS 2000u
/* A missing fragment is asked for once the sample has been quiet for this
 * long; a merely reordered datagram then arrives on its own. Retries back off
 * to this interval, and after a few rounds the reader falls back to waiting
 * for the next whole-sample retransmit. */
#define NB_RTPS_FRAG_NACK_DELAY_MS 60u
#define NB_RTPS_FRAG_NACK_RETRY_MS 100u
#define NB_RTPS_FRAG_MAX_NACKS 4u
/* Status events collected by one deadline sweep before it delivers them. */
#define NB_RTPS_MAX_STATUS_EVENTS 8u
/* Automatic liveliness is asserted at this share of the shortest lease we
 * offer. Cyclone asserts at 80%, but its timer is fine-grained; the participant
 * thread here wakes every 100 ms, so half the lease leaves room for both one
 * lost datagram and that scheduling slack. */
#define NB_RTPS_PMD_ASSERT_PCT 50u
#define NB_RTPS_MIN_PMD_INTERVAL_MS 50u
#define NB_RTPS_DEFAULT_MAX_LOCAL_ENDPOINTS 1024u
#define NB_RTPS_DEFAULT_MAX_REMOTE_ENDPOINTS 4096u
#define NB_RTPS_MAX_LOCAL_ENDPOINTS 4096u
#define NB_RTPS_MAX_REMOTE_ENDPOINTS 4096u
#define NB_RTPS_MAX_DATA_TARGETS 16u
#define NB_RTPS_DEFAULT_SEDP_HEARTBEAT_MS 1000u
#define NB_RTPS_MIN_SEDP_HEARTBEAT_MS 100u
#define NB_RTPS_DEFAULT_DATA_HEARTBEAT_MS 1000u
#define NB_RTPS_MIN_DATA_HEARTBEAT_MS 100u
#define NB_RTPS_DEFAULT_DATA_HISTORY 8u
#define NB_RTPS_MAX_DATA_HISTORY 1024u
/* Upper bound for the lazily allocated reader window table. */
#define NB_RTPS_MAX_DATA_READERS 4096u
/* Do not answer two HEARTBEATs from the same writer for the same reader inside
 * this window; peers that emit heartbeats faster than this would otherwise
 * turn into an ACKNACK storm. */
#define NB_RTPS_MIN_ACKNACK_INTERVAL_MS 20u
/* Do not retransmit for the same (writer, reader) pair more often than this.
 * Duplicate or aggressive ACKNACKs then cost one packet instead of a burst. */
#define NB_RTPS_MIN_RESEND_INTERVAL_MS 10u
/* Cap the samples resent for one ACKNACK. The reader asks again for the
 * remainder on the next round, which keeps a burst from monopolising a thread
 * or a link. */
#define NB_RTPS_MAX_RESEND_PER_ACK 64u

typedef nb_rtps_reader_state_t nb_rtps_sedp_reader_state_t;

/* Instances a subscription can track when the QoS sets no limit. */
#define NB_RTPS_DEFAULT_MAX_INSTANCES 64u
#define NB_RTPS_MAX_INSTANCES 4096u

/*
 * One instance as a subscription knows it, keyed by the DDS instance handle the
 * reader got from PID_KEY_HASH. Only keyed traffic creates entries, so the table
 * is allocated lazily; `state`, `view` and `generation_rank` below are the
 * per-instance parts of DDS SampleInfo.
 */
typedef struct nb_rtps_instance {
    int used;
    uint8_t handle[NB_INSTANCE_HANDLE_SIZE];
    nb_instance_state_t state;
    /* How often the instance went from not-alive back to alive; DDS
     * generationRank. */
    uint32_t generation_rank;
    /* A sample has been delivered for this instance already. */
    int seen;
    /* The writer that last registered the instance, for NO_WRITERS. */
    nb_rtps_guid_t writer;
} nb_rtps_instance_t;

typedef struct nb_rtps_local_endpoint {
    int used;
    nb_rtps_endpoint_kind_t kind;
    nb_rtps_endpoint_info_t info;
    uint64_t sn;
    uint64_t data_sn;
    nb_rtps_reliable_writer_t *history; /* reliable publications only */
    uint32_t heartbeat_count;
    uint64_t last_resend_ms;
    nb_rtps_guid_t last_resend_reader;
    int resend_seen;
    /* Deadline bookkeeping for a publication. */
    uint64_t last_write_ms;
    uint64_t next_deadline_ms;
    uint32_t deadline_miss_count;
    /* Newest source timestamp delivered for a BY_SOURCE_TIMESTAMP subscription.
     * There are no instances yet, so the topic is the instance. */
    uint64_t last_source_ns;
    int has_last_source;
    /* TimeBasedFilter state: source timestamp of the last delivered sample.
     * Cyclone keeps this per instance, so one per topic is the same thing
     * until instances exist. */
    uint64_t last_filtered_ns;
    int has_last_filtered;
    /* Cumulative remote endpoints this local endpoint has matched. */
    uint32_t match_total;
    /* Per-instance state for a keyed subscription. */
    nb_rtps_instance_t *instances;
    uint32_t instance_count;
    uint32_t max_instances;
    uint32_t sample_lost_count;
    uint32_t sample_rejected_count;
} nb_rtps_local_endpoint_t;

typedef struct nb_rtps_remote_endpoint {
    int used;
    nb_rtps_endpoint_kind_t kind;
    nb_rtps_endpoint_info_t info;
    uint64_t last_seen_ms;
} nb_rtps_remote_endpoint_t;

typedef struct nb_rtps_remote_participant {
    int used;
    uint64_t last_seen_ms;
    /* Last ParticipantMessageData assertion seen from this participant. */
    uint64_t last_pmd_ms;
    uint32_t lease_ms;
    nb_rtps_participant_info_t info;
    char source_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t source_port;
    /* Set once an RTPS-over-TCP link to this participant exists; sends use
     * this key instead of a UDP address. Empty means "use UDP". */
    char tcp_key[NB_RTPS_TCP_KEY_MAX];
    uint64_t next_tcp_connect_ms;
    nb_rtps_sedp_reader_state_t publications_reader;
    nb_rtps_sedp_reader_state_t subscriptions_reader;
} nb_rtps_remote_participant_t;

/*
 * Receive window for one (remote writer, local reader) pair. Reliable user
 * data needs this per pair because a writer can address several local
 * subscriptions with independent ACKNACK state.
 */
typedef struct nb_rtps_data_reader_state {
    int used;
    nb_rtps_guid_t writer;
    nb_rtps_entity_id_t reader;
    nb_rtps_reader_state_t state;
    uint32_t last_heartbeat_count;
    int heartbeat_seen;
    uint64_t last_acknack_ms;
    /* Deadline bookkeeping for a subscription. */
    uint64_t last_sample_ms;
    uint64_t next_deadline_ms;
    uint32_t deadline_miss_count;
    /* Liveliness bookkeeping for the matched writer. A pair starts alive, as
     * DDS does when a writer matches, and turns not-alive when no assertion
     * arrives within the writer's lease. */
    int alive;
    uint64_t last_liveliness_ms;
    uint32_t liveliness_events;
    /* Liveliness announced by the matched writer. AUTOMATIC renews on every
     * ParticipantMessageData, MANUAL_BY_PARTICIPANT only on a manual assertion,
     * MANUAL_BY_TOPIC only on a message from that writer (data or a heartbeat
     * with the L flag). */
    uint32_t liveliness_kind;
    uint32_t liveliness_lease_ms;
} nb_rtps_data_reader_state_t;

/*
 * One sample being reassembled from DATA_FRAG submessages. data holds
 * sample_size octets, received[] carries one flag per fragment.
 */
typedef struct nb_rtps_frag_assembly {
    int used;
    int complete;
    nb_rtps_guid_t writer;
    nb_rtps_entity_id_t reader_id;
    uint64_t sn;
    uint32_t sample_size;
    uint16_t fragment_size;
    uint32_t fragment_count;
    uint32_t received_count;
    /* Source timestamp of the first fragment of the sample. */
    int has_source_ts;
    nb_rtps_time_t source_ts;
    /* Inline QoS of the first fragment: PID_KEY_HASH / PID_STATUS_INFO. Copied
     * because the receive buffer is reused before the sample is complete. */
    int has_inline_qos;
    uint8_t inline_qos[64];
    size_t inline_qos_len;
    int key;
    uint8_t *data;
    uint8_t *received;
    size_t reserved;
    uint64_t last_update_ms;
    /* Where the fragments came from, so a missing one can be asked for
     * directly instead of waiting for a whole-sample retransmit. */
    char source_host[NB_RTPS_PEER_HOST_MAX];
    uint16_t source_port;
    uint64_t last_nack_ms;
    uint32_t nack_count;
} nb_rtps_frag_assembly_t;

/*
 * One remote endpoint may advertise a unicast locator per interface. Sending a
 * sample to all of them mirrors Cyclone DDS, which transmits to every unicast
 * address in a proxy reader's address set instead of guessing which interface
 * is reachable. Readers deduplicate by sequence number, so the copies that do
 * arrive twice are dropped.
 */
#define NB_RTPS_DATA_TARGET_MAX 4u

typedef struct nb_rtps_targets {
    size_t count;
    char host[NB_RTPS_DATA_TARGET_MAX][NB_RTPS_PEER_HOST_MAX];
    uint16_t port[NB_RTPS_DATA_TARGET_MAX];
} nb_rtps_targets_t;

/*
 * 参与者状态。并发契约（改动任何字段前先核对这一条）：
 *
 * - `lock` 保护全部动态表及其 *_used 稠密索引与计数：
 *     remotes[], remote_endpoints[], local_endpoints[], data_readers[],
 *     next_entity_key, frag_assemblies[], frag_bytes_in_use。
 *   热路径遍历 *_used 而不是整表；增删都必须在同一把锁下维护对应 *_used。
 * - 回调指针（callback / endpoint_callback / data_callback / status_callback
 *   及 *_user）由 setter 在 lock 下写入；调用方在锁内取快照、解锁后再回调，
 *   回调期间不得持锁。
 * - 参与者线程节拍（next_announce_ms / next_sweep_ms / next_sedp_heartbeat_ms /
 *   next_data_heartbeat_ms / next_pmd_ms / pmd_interval_ms / pmd_sn / spdp_sn）
 *   只由 thread_main 读写，其它线程不得触碰。
 * - 配置字段（bind_host … data_port、max_*、peers、local_guid、metatraffic_*）
 *   在 create 之后只读。
 * - `stop` 是跨线程停止标志（volatile）；其余字段要么受 lock 保护，要么线程
 *   私有/只读，禁止无锁写入。
 */
struct nb_rtps_participant {
    /* ---- 配置：create 之后只读 ---- */
    char bind_host[NB_RTPS_PEER_HOST_MAX];
    char advertise_host[NB_RTPS_PEER_HOST_MAX];
    char multicast_group[NB_RTPS_PEER_HOST_MAX];
    char name[NB_RTPS_SPDP_NAME_MAX];
    uint32_t domain_id;
    uint32_t participant_id;
    int standard_ports;
    uint16_t spdp_port;
    uint16_t unicast_port;
    uint32_t announce_interval_ms;
    uint32_t lease_ms;
    nb_rtps_locator_t default_unicast;
    nb_rtps_peer_t peers[NB_RTPS_MAX_PEERS];
    size_t peer_count;
    uint32_t max_remotes;

    int multicast_enabled;
    uint8_t multicast_addr[4];
    uint8_t advertise_addr[4];
    nb_rtps_guid_t local_guid;
    nb_rtps_locator_t metatraffic_unicast;
    nb_rtps_locator_t metatraffic_multicast;
    /* TCP locator, Invalid when the participant is UDP-only. */
    nb_rtps_locator_t metatraffic_tcp;
    /* SEDP/data heartbeat cadence and history depth: configured at create,
     * read-only afterwards. */
    uint32_t sedp_heartbeat_ms;
    uint32_t data_heartbeat_ms;
    uint32_t data_history_depth;

    /* ---- socket / 线程 / 锁（create 之后只读） ---- */
    struct {
        zn_socket *rx_sock;
        zn_socket *unicast_sock;
        /* Standard mapping only: user-data unicast port (d3+PG*pid). NULL means
         * user data shares unicast_sock. */
        zn_socket *data_sock;
        uint16_t data_port;
        zn_poller *poller;
        zb_thread *thread;
        zb_mutex *lock;
        /* Reusable arena for batched receive: NB_RTPS_RX_BATCH slots of
         * NB_RTPS_RX_MAX bytes, owned by the participant thread. */
        uint8_t *rx_batch;
        /* Largest datagram the bulk-publish path may build. The single-sample
         * path stays at NB_RTPS_PACKET_MAX; on loopback (huge MTU) the bulk path
         * aggregates far more samples per datagram. */
        size_t max_datagram_size;
    } net;

    /* ---- 由 net.lock 保护的动态状态：表、计数、回调快照源 ---- */
    struct {
        nb_rtps_remote_participant_t *remotes;
        nb_rtps_participant_callback_t callback;
        void *callback_user;
        nb_rtps_endpoint_callback_t endpoint_callback;
        void *endpoint_user;
        nb_rtps_data_callback_t data_callback;
        void *data_user;
        nb_rtps_status_callback_t status_callback;
        void *status_user;
        nb_rtps_local_endpoint_t *local_endpoints;
        size_t max_local_endpoints;
        size_t local_endpoint_count;
        uint32_t next_entity_key;
        nb_rtps_remote_endpoint_t *remote_endpoints;
        size_t max_remote_endpoints;
        nb_rtps_reliable_writer_t *publications_writer;
        nb_rtps_reliable_writer_t *subscriptions_writer;
        uint32_t publications_hb_count;
        uint32_t subscriptions_hb_count;
        nb_rtps_data_reader_state_t *data_readers;
        size_t max_data_readers;
        size_t data_reader_count;
        int data_reader_overflow_logged;
        /* Dense lists of the slots in use in the four fixed-size tables. The hot
         * paths (one publish, one received sample) walk these instead of the whole
         * table, which is sized for the worst case and is almost empty in practice.
         * Removal swaps the last entry into the hole; iteration order carries no
         * meaning. */
        uint32_t *local_used;
        size_t local_used_count;
        uint32_t *remote_ep_used;
        size_t remote_ep_used_count;
        uint32_t *remotes_used;
        size_t remotes_used_count;
        uint32_t *data_reader_used;
        size_t data_reader_used_count;
        nb_rtps_frag_assembly_t *frag_assemblies;
        size_t max_frag_assemblies;
        size_t frag_bytes_in_use;
        uint32_t frag_heartbeat_count;
    } tables;

    /* ---- 参与者线程节拍：仅 thread_main 读写 ---- */
    struct {
        uint64_t spdp_sn;
        uint64_t next_announce_ms;
        uint64_t next_sweep_ms;
        uint64_t next_sedp_heartbeat_ms;
        uint64_t next_data_heartbeat_ms;
        /* ParticipantMessageData cadence; 0 means no local publication needs
         * automatic assertions, so nothing is sent. */
        uint32_t pmd_interval_ms;
        uint64_t pmd_sn;
        uint64_t next_pmd_ms;
    } timers;

    /* ---- 跨线程标志与统计 ---- */
    volatile int stop;
    int started;
    uint32_t rng;
    nb_rtps_stats_t stats;
    int dump_frames;

    /* ---- TCP ---- */
    nb_rtps_tcp_t *tcp;
};

/* 状态事件在参与者锁之外回调，先入队再统一上报：
 * 数据面（rtps_datapath.c）生成，参与者调度统一发送。 */
typedef struct nb_rtps_queued_status {
    nb_rtps_status_kind_t kind;
    nb_rtps_guid_t endpoint;
    uint32_t count;
    const char *reason;
} nb_rtps_queued_status_t;

/* ---- 由 rtps_participant.c 提供给数据面的工具 ---- */
void copy_str(char *dst, size_t cap, const char *src);
zn_socket *participant_data_socket(const nb_rtps_participant_t *participant);
zn_ssize_t nb_rtps_sendto(
    nb_rtps_participant_t *participant,
    zn_socket *socket,
    const void *data,
    size_t len,
    const char *host,
    uint16_t port);
void init_header(const nb_rtps_participant_t *participant, nb_rtps_header_t *header);
nb_rtps_local_endpoint_t *find_local_endpoint(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *guid);
nb_rtps_remote_endpoint_t *find_remote_endpoint(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *guid);
nb_rtps_remote_participant_t *find_remote(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_prefix_t *prefix);
size_t endpoint_data_targets(
    nb_rtps_participant_t *participant,
    const nb_rtps_remote_endpoint_t *endpoint,
    nb_rtps_targets_t *out);
int endpoint_data_target(
    nb_rtps_participant_t *participant,
    const nb_rtps_remote_endpoint_t *endpoint,
    char *host,
    size_t cap,
    uint16_t *port);
void nb_rtps_used_add(uint32_t *list, size_t *count, size_t cap, size_t index);
void nb_rtps_used_remove(uint32_t *list, size_t *count, size_t index);
int ensure_data_readers(nb_rtps_participant_t *participant);
nb_rtps_data_reader_state_t *find_data_reader(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer,
    const nb_rtps_entity_id_t *reader);
nb_rtps_data_reader_state_t *get_data_reader(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer,
    const nb_rtps_entity_id_t *reader,
    uint64_t first_expected);
int data_endpoint_matches(
    const nb_rtps_local_endpoint_t *local,
    const nb_rtps_remote_endpoint_t *remote);
void arm_reader_deadlines(
    nb_rtps_participant_t *participant,
    const nb_rtps_remote_endpoint_t *remote);
void drop_data_readers_for_writer(nb_rtps_participant_t *participant, const nb_rtps_guid_t *writer);
nb_rtps_time_t rtps_time_from_ns(uint64_t ns);
uint64_t rtps_time_to_ns(const nb_rtps_time_t *ts);
int endpoint_qos_compatible(
    const nb_rtps_endpoint_info_t *publication,
    const nb_rtps_endpoint_info_t *subscription);
void handle_pmd_data(
    nb_rtps_participant_t *participant,
    const nb_rtps_data_view_t *data,
    const nb_rtps_guid_prefix_t *prefix);

/* ---- 由 rtps_participant.c 提供给发现面/数据面使用的工具 ---- */
int remote_target(
    const nb_rtps_remote_participant_t *remote,
    char *host,
    size_t cap,
    uint16_t *port);
void notify_endpoint(
    nb_rtps_participant_t *participant,
    nb_rtps_endpoint_event_t event,
    nb_rtps_endpoint_kind_t kind,
    const nb_rtps_endpoint_info_t *info);
void notify(
    nb_rtps_participant_t *participant,
    nb_rtps_participant_event_t event,
    const nb_rtps_participant_info_t *info);
void notify_matches(
    nb_rtps_participant_t *participant,
    const nb_rtps_endpoint_info_t *remote_info,
    nb_rtps_endpoint_kind_t remote_kind,
    int added);
void replay_retained_history(
    nb_rtps_participant_t *participant,
    const nb_rtps_endpoint_info_t *reader_info);
int locator_to_host(const nb_rtps_locator_t *locator, char *host, size_t cap, uint16_t *port);
int locator_to_host_any(const nb_rtps_locator_t *locator, char *host, size_t cap, uint16_t *port);
void send_pmd(nb_rtps_participant_t *participant, uint32_t kind);
void maybe_tcp_connect(nb_rtps_participant_t *participant, const nb_rtps_guid_prefix_t *prefix);

/* ---- 跨编译单元共享的参与者内部函数 ---- */
/* 接收 socket 排空（rtps_participant.c）由调度线程调用。 */
void drain_socket(nb_rtps_participant_t *participant, zn_socket *socket);
/* 调度线程主体与 TCP 链路维护（rtps_schedule.c / rtps_tcp_link.c）。 */
void thread_main(void *user);
void retry_tcp_links(nb_rtps_participant_t *participant);
void tcp_on_message(void *user,
                    int index,
                    const nb_rtps_header_t *header,
                    const nb_rtps_submessage_view_t *view,
                    nb_rtps_receiver_state_t *state);
void tcp_on_link(void *user, int index);
void tcp_on_unlink(void *user, int index);
/* 端点增删后重放匹配事件（rtps_matching.c）。 */
void notify_matches_for_local(nb_rtps_participant_t *participant,
                              const nb_rtps_guid_t *local_guid);
/* 匹配判定与计数（rtps_matching.c）：被匹配重放与统计共用。 */
const char *endpoint_qos_mismatch(
    const nb_rtps_endpoint_info_t *publication,
    const nb_rtps_endpoint_info_t *subscription);
uint32_t matched_remote_count(nb_rtps_participant_t *participant,
                              const nb_rtps_local_endpoint_t *local);
/* 单条子消息分派（rtps_participant.c），TCP 链路复用同一路径。 */
void handle_submessage(nb_rtps_participant_t *participant,
                       const nb_rtps_header_t *header,
                       const nb_rtps_submessage_view_t *view,
                       nb_rtps_receiver_state_t *state,
                       const char *source_host,
                       uint16_t source_port);

/* Origin of one received datagram, threaded through the receive path so the
 * handlers carry one argument instead of prefix/host/port/timestamp each. The
 * pointers borrow from the caller's stack for the duration of the call. */
typedef struct nb_rtps_rx_ctx {
    const nb_rtps_guid_prefix_t *prefix;
    const char *host;
    uint16_t port;
    const nb_rtps_time_t *source_ts; /* NULL when the datagram carried none */
} nb_rtps_rx_ctx_t;

/* ---- 由 rtps_datapath.c 提供给参与者调度使用 ---- */
void dispatch_data_view(
    nb_rtps_participant_t *participant,
    const nb_rtps_data_view_t *data,
    const nb_rtps_rx_ctx_t *rx);
nb_rtps_instance_t *get_instance(
    nb_rtps_local_endpoint_t *local,
    const uint8_t *handle,
    const nb_rtps_guid_t *writer);
void handle_data_frag(
    nb_rtps_participant_t *participant,
    const nb_rtps_data_frag_t *fragment,
    const nb_rtps_rx_ctx_t *rx);
void handle_user_acknack(
    nb_rtps_participant_t *participant,
    const nb_rtps_acknack_t *acknack,
    const nb_rtps_rx_ctx_t *rx);
void handle_user_gap(
    nb_rtps_participant_t *participant,
    const nb_rtps_gap_t *gap,
    const nb_rtps_rx_ctx_t *rx);
void handle_user_heartbeat(
    nb_rtps_participant_t *participant,
    const nb_rtps_heartbeat_t *heartbeat,
    const nb_rtps_rx_ctx_t *rx);
void handle_user_heartbeat_frag(
    nb_rtps_participant_t *participant,
    const nb_rtps_heartbeat_frag_t *heartbeat,
    const nb_rtps_rx_ctx_t *rx);
void handle_user_nack_frag(
    nb_rtps_participant_t *participant,
    const nb_rtps_nack_frag_t *nack,
    const nb_rtps_rx_ctx_t *rx);
void mark_instances_no_writers(nb_rtps_local_endpoint_t *local, const nb_rtps_guid_t *writer);
int sample_expired(const nb_rtps_local_endpoint_t *local, uint64_t sn, uint64_t now);
void send_data_heartbeats(nb_rtps_participant_t *participant);
int send_frag_nack(
    nb_rtps_participant_t *participant,
    nb_rtps_frag_assembly_t *assembly,
    uint64_t now);
int send_user_data_all(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer_guid,
    const nb_rtps_guid_t *reader_guid,
    uint64_t sn,
    uint64_t source_ns,
    const void *payload,
    size_t payload_len,
    const uint8_t *handle,
    uint32_t status,
    const nb_rtps_targets_t *targets);
void send_user_gap_all(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_t *writer_guid,
    const nb_rtps_guid_t *reader_guid,
    uint64_t gap_start,
    uint64_t gap_last,
    const nb_rtps_targets_t *targets);
int send_user_heartbeat(
    nb_rtps_participant_t *participant,
    nb_rtps_local_endpoint_t *local,
    const nb_rtps_endpoint_info_t *remote,
    uint64_t first_sn,
    uint64_t last_sn,
    const char *host,
    uint16_t port);
void send_user_heartbeat_all(
    nb_rtps_participant_t *participant,
    nb_rtps_local_endpoint_t *local,
    const nb_rtps_endpoint_info_t *reader_info,
    uint64_t first_sn,
    uint64_t last_sn,
    const nb_rtps_targets_t *targets);
void send_user_liveliness_heartbeat(
    nb_rtps_participant_t *participant,
    nb_rtps_local_endpoint_t *local,
    const nb_rtps_endpoint_info_t *remote,
    const char *host,
    uint16_t port);

/* ---- 由 rtps_discovery.c 提供给参与者调度使用 ---- */
void broadcast_sedp_sample(
    nb_rtps_participant_t *participant,
    nb_rtps_endpoint_kind_t kind,
    uint64_t sn,
    const uint8_t *payload,
    size_t payload_len,
    int dispose);
void send_local_endpoint_to_remotes(
    nb_rtps_participant_t *participant,
    const nb_rtps_local_endpoint_t *local);
void send_sedp_heartbeats(nb_rtps_participant_t *participant);
int send_spdp(nb_rtps_participant_t *participant);
void fill_local_info(const nb_rtps_participant_t *participant, nb_rtps_participant_info_t *info);
int parse_ipv4(const char *host, uint8_t out[4]);
void handle_sedp_gap(
    nb_rtps_participant_t *participant,
    nb_rtps_endpoint_kind_t kind,
    const nb_rtps_gap_t *gap,
    const nb_rtps_guid_prefix_t *source_prefix);
void handle_sedp_heartbeat(
    nb_rtps_participant_t *participant,
    nb_rtps_endpoint_kind_t kind,
    const nb_rtps_heartbeat_t *heartbeat,
    const nb_rtps_guid_prefix_t *source_prefix,
    const char *source_host,
    uint16_t source_port);
void handle_sedp_acknack(
    nb_rtps_participant_t *participant,
    nb_rtps_endpoint_kind_t kind,
    const nb_rtps_acknack_t *acknack,
    const nb_rtps_guid_prefix_t *source_prefix,
    const char *source_host,
    uint16_t source_port);
void remove_remote_endpoints_for_prefix(
    nb_rtps_participant_t *participant,
    const nb_rtps_guid_prefix_t *prefix);
void update_remote(
    nb_rtps_participant_t *participant,
    const nb_rtps_participant_info_t *info,
    const char *source_host,
    uint16_t source_port);
void handle_sedp_data(
    nb_rtps_participant_t *participant,
    nb_rtps_endpoint_kind_t kind,
    const nb_rtps_data_view_t *data,
    const nb_rtps_guid_prefix_t *source_prefix);
uint32_t lease_from_info(const nb_rtps_participant_info_t *info, uint32_t fallback);

#endif /* NB_RTPS_PARTICIPANT_PRIV_H */
