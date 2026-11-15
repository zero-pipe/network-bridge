/* RTPS 出站报文构建：统一 MessageHeader + INFO_TS + (INFO_DST) + 实体子消息。 */
#include "rtps_wire.h"

#include <string.h>

/*
 * 起手链：MessageHeader → INFO_TS（可选）→ INFO_DST（可选）。
 * 返回 0 成功，-1 失败。
 */
static int wire_begin(nb_cdr_writer_t *writer,
                      uint8_t *buf,
                      size_t cap,
                      const nb_rtps_header_t *header,
                      const nb_rtps_time_t *ts,
                      const nb_rtps_guid_prefix_t *dst)
{
    if (!writer || !buf || !header) {
        return -1;
    }
    if (nb_rtps_message_init(writer, buf, cap, header) != NB_OK) {
        return -1;
    }
    if (ts && nb_rtps_info_ts(writer, ts, false) != NB_OK) {
        return -1;
    }
    if (dst && nb_rtps_info_dst(writer, dst) != NB_OK) {
        return -1;
    }
    return 0;
}

/* 收尾：确认 writer 状态良好后返回总长度，0 表示失败。 */
static size_t wire_end(nb_cdr_writer_t *writer)
{
    if (!writer || !nb_cdr_writer_ok(writer)) {
        return 0;
    }
    return nb_cdr_writer_len(writer);
}

size_t nb_rtps_wire_data(uint8_t *buf,
                         size_t cap,
                         const nb_rtps_header_t *header,
                         const nb_rtps_time_t *ts,
                         const nb_rtps_guid_prefix_t *dst,
                         const nb_rtps_data_t *data)
{
    nb_cdr_writer_t writer;

    if (!data || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_data(&writer, data) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}

size_t nb_rtps_wire_data_frag(uint8_t *buf,
                              size_t cap,
                              const nb_rtps_header_t *header,
                              const nb_rtps_time_t *ts,
                              const nb_rtps_guid_prefix_t *dst,
                              const nb_rtps_data_frag_t *fragment)
{
    nb_cdr_writer_t writer;

    if (!fragment || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_data_frag(&writer, fragment) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}

size_t nb_rtps_wire_heartbeat(uint8_t *buf,
                              size_t cap,
                              const nb_rtps_header_t *header,
                              const nb_rtps_time_t *ts,
                              const nb_rtps_guid_prefix_t *dst,
                              const nb_rtps_heartbeat_t *heartbeat)
{
    nb_cdr_writer_t writer;

    if (!heartbeat || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_heartbeat(&writer, heartbeat) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}

size_t nb_rtps_wire_heartbeat_frag(uint8_t *buf,
                                   size_t cap,
                                   const nb_rtps_header_t *header,
                                   const nb_rtps_time_t *ts,
                                   const nb_rtps_guid_prefix_t *dst,
                                   const nb_rtps_heartbeat_frag_t *heartbeat)
{
    nb_cdr_writer_t writer;

    if (!heartbeat || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_heartbeat_frag(&writer, heartbeat) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}

size_t nb_rtps_wire_acknack(uint8_t *buf,
                            size_t cap,
                            const nb_rtps_header_t *header,
                            const nb_rtps_time_t *ts,
                            const nb_rtps_guid_prefix_t *dst,
                            const nb_rtps_acknack_t *acknack)
{
    nb_cdr_writer_t writer;

    if (!acknack || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_acknack(&writer, acknack) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}

size_t nb_rtps_wire_gap(uint8_t *buf,
                        size_t cap,
                        const nb_rtps_header_t *header,
                        const nb_rtps_time_t *ts,
                        const nb_rtps_guid_prefix_t *dst,
                        const nb_rtps_gap_t *gap)
{
    nb_cdr_writer_t writer;

    if (!gap || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_gap(&writer, gap) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}

size_t nb_rtps_wire_nack_frag(uint8_t *buf,
                              size_t cap,
                              const nb_rtps_header_t *header,
                              const nb_rtps_time_t *ts,
                              const nb_rtps_guid_prefix_t *dst,
                              const nb_rtps_nack_frag_t *nack)
{
    nb_cdr_writer_t writer;

    if (!nack || wire_begin(&writer, buf, cap, header, ts, dst) != 0) {
        return 0;
    }
    if (nb_rtps_nack_frag(&writer, nack) != NB_OK) {
        return 0;
    }
    return wire_end(&writer);
}
