#include "rtps_cdr.h"
#include "rtps_data.h"
#include "rtps_message.h"
#include "rtps_param.h"
#include "rtps_participant.h"
#include "rtps_reliable.h"
#include "rtps_sedp.h"
#include "rtps_spdp.h"

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

static void test_cdr_roundtrip(void)
{
    uint8_t buf[128];
    nb_cdr_writer_t writer;
    nb_cdr_reader_t reader;
    char text[16];

    nb_cdr_writer_init(&writer, buf, sizeof(buf));
    nb_cdr_put_u8(&writer, 0xabu);
    nb_cdr_put_u32(&writer, 0x11223344u);
    nb_cdr_put_u64(&writer, 0x0102030405060708ull);
    nb_cdr_put_string(&writer, "hello");
    CHECK(nb_cdr_writer_ok(&writer));
    CHECK(nb_cdr_writer_len(&writer) == 1 + 3 + 4 + 8 + 4 + 6);
    CHECK(buf[0] == 0xab);
    CHECK(buf[4] == 0x44 && buf[5] == 0x33 && buf[6] == 0x22 && buf[7] == 0x11);
    CHECK(buf[8] == 0x08 && buf[15] == 0x01);

    nb_cdr_reader_init(&reader, buf, nb_cdr_writer_len(&writer));
    CHECK(nb_cdr_get_u8(&reader) == 0xabu);
    CHECK(nb_cdr_get_u32(&reader) == 0x11223344u);
    CHECK(nb_cdr_get_u64(&reader) == 0x0102030405060708ull);
    CHECK(nb_cdr_get_string(&reader, text, sizeof(text)));
    CHECK(strcmp(text, "hello") == 0);
    CHECK(nb_cdr_reader_ok(&reader));
    CHECK(nb_cdr_reader_remaining(&reader) == 0);

    /* Reading past the end must fail without touching memory. */
    nb_cdr_reader_init(&reader, buf, 4);
    CHECK(nb_cdr_get_u64(&reader) == 0);
    CHECK(!nb_cdr_reader_ok(&reader));
}

static void test_dds_message_cdr(void)
{
    static const uint8_t bytes[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22};
    static const uint8_t cyclone_bytes[] = {
        0x00, 0x01, 0x00, 0x01,
        0x07, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x00
    };
    nb_rtps_dds_message_t message;
    nb_rtps_dds_message_t parsed;
    uint8_t buf[64];
    uint8_t small[14];
    size_t len = 0;

    CHECK(strcmp(NB_RTPS_DDS_MESSAGE_TYPE, "dds::Message") == 0);
    message.payload = bytes;
    message.payload_len = sizeof(bytes);
    CHECK(nb_rtps_dds_message_serialize(&message, buf, sizeof(buf), &len) == NB_OK);
    CHECK(len == 4u + 4u + sizeof(bytes));
    CHECK(buf[0] == 0x00 && buf[1] == 0x01 && buf[2] == 0x00 && buf[3] == 0x00);
    CHECK(buf[4] == 0x07 && buf[5] == 0x00 && buf[6] == 0x00 && buf[7] == 0x00);
    CHECK(memcmp(buf + 8, bytes, sizeof(bytes)) == 0);

    CHECK(nb_rtps_dds_message_deserialize(buf, len, &parsed) == NB_OK);
    CHECK(parsed.payload == buf + 8);
    CHECK(parsed.payload_len == sizeof(bytes));
    CHECK(memcmp(parsed.payload, bytes, sizeof(bytes)) == 0);

    /* Cyclone DDS emits option bits 0x0001 after the CDR_LE identifier. */
    CHECK(nb_rtps_dds_message_deserialize(
              cyclone_bytes, sizeof(cyclone_bytes), &parsed) == NB_OK);
    CHECK(parsed.payload == cyclone_bytes + 8);
    CHECK(parsed.payload_len == sizeof(bytes));
    CHECK(memcmp(parsed.payload, cyclone_bytes + 8, sizeof(bytes)) == 0);

    /* Truncated payload must fail. */
    CHECK(nb_rtps_dds_message_deserialize(buf, len - 1u, &parsed) != NB_OK);
    /* Bad encapsulation header must fail. */
    buf[1] = 0x00;
    CHECK(nb_rtps_dds_message_deserialize(buf, len, &parsed) == NB_ERR_NOT_SUPPORTED);
    buf[1] = 0x01;

    /* Empty sequence is valid and still points into the input buffer. */
    message.payload = NULL;
    message.payload_len = 0;
    CHECK(nb_rtps_dds_message_serialize(&message, buf, sizeof(buf), &len) == NB_OK);
    CHECK(len == 8u);
    CHECK(nb_rtps_dds_message_deserialize(buf, len, &parsed) == NB_OK);
    CHECK(parsed.payload == buf + 8);
    CHECK(parsed.payload_len == 0);

    /* Non-empty payload requires a valid pointer. */
    message.payload = NULL;
    message.payload_len = 1;
    CHECK(nb_rtps_dds_message_serialize(&message, buf, sizeof(buf), &len) != NB_OK);

    /* A buffer that cannot hold header + length + payload must fail. */
    message.payload = bytes;
    message.payload_len = sizeof(bytes);
    CHECK(nb_rtps_dds_message_serialize(
              &message, small, sizeof(small), &len) != NB_OK);
}

static void test_param_roundtrip(void)
{
    uint8_t buf[512];
    nb_rtps_param_writer_t writer;
    nb_rtps_param_reader_t reader;
    nb_rtps_guid_t guid;
    nb_rtps_locator_t locator;
    nb_rtps_duration_t lease;
    uint16_t pid;
    const uint8_t *value;
    size_t value_len;
    uint8_t first;
    uint8_t second;
    char name[32];
    int rc;
    int seen = 0;

    memset(&guid, 0, sizeof(guid));
    guid.prefix.bytes[0] = 0x11;
    guid.prefix.bytes[11] = 0x22;
    guid.entity.bytes[3] = 0xc2;
    nb_rtps_locator_ipv4(&locator, 192, 168, 1, 10, 7400);
    lease.seconds = 20;
    lease.fraction = 0;

    nb_rtps_param_writer_init(&writer, buf, sizeof(buf));
    CHECK(nb_rtps_param_put_octets2(&writer, NB_RTPS_PID_PROTOCOL_VERSION, 2, 4) == NB_OK);
    CHECK(nb_rtps_param_put_octets2(&writer, NB_RTPS_PID_VENDORID, 0x01, 0x9a) == NB_OK);
    CHECK(nb_rtps_param_put_guid(&writer, NB_RTPS_PID_PARTICIPANT_GUID, &guid) == NB_OK);
    CHECK(nb_rtps_param_put_duration(
              &writer, NB_RTPS_PID_PARTICIPANT_LEASE_DURATION, &lease) == NB_OK);
    CHECK(nb_rtps_param_put_u32(&writer,
                                NB_RTPS_PID_BUILTIN_ENDPOINT_SET,
                                NB_RTPS_BUILTIN_ENDPOINT_PUBLICATIONS_ANNOUNCER) == NB_OK);
    CHECK(nb_rtps_param_put_locator(
              &writer, NB_RTPS_PID_METATRAFFIC_UNICAST_LOCATOR, &locator) == NB_OK);
    CHECK(nb_rtps_param_put_string(&writer, NB_RTPS_PID_ENTITY_NAME, "node-a") == NB_OK);
    CHECK(nb_rtps_param_writer_finish(&writer) == NB_OK);
    CHECK(nb_rtps_param_writer_ok(&writer));
    CHECK(nb_rtps_param_writer_len(&writer) % 4 == 0);

    nb_rtps_param_reader_init(&reader, buf, nb_rtps_param_writer_len(&writer));
    for (;;) {
        rc = nb_rtps_param_reader_next(&reader, &pid, &value, &value_len);
        if (rc <= 0) {
            CHECK(rc == 0);
            break;
        }
        CHECK(reader.pos % 4 == 0);
        if (pid == NB_RTPS_PID_PROTOCOL_VERSION) {
            CHECK(nb_rtps_param_get_octets2(value, value_len, &first, &second) == NB_OK);
            CHECK(first == 2 && second == 4);
            seen |= 1;
        } else if (pid == NB_RTPS_PID_PARTICIPANT_GUID) {
            nb_rtps_guid_t parsed;
            CHECK(nb_rtps_param_get_guid(value, value_len, &parsed) == NB_OK);
            CHECK(nb_rtps_guid_equal(&parsed, &guid));
            seen |= 2;
        } else if (pid == NB_RTPS_PID_PARTICIPANT_LEASE_DURATION) {
            nb_rtps_duration_t parsed;
            CHECK(nb_rtps_param_get_duration(value, value_len, &parsed) == NB_OK);
            CHECK(parsed.seconds == 20 && parsed.fraction == 0);
            seen |= 4;
        } else if (pid == NB_RTPS_PID_METATRAFFIC_UNICAST_LOCATOR) {
            nb_rtps_locator_t parsed;
            CHECK(nb_rtps_param_get_locator(value, value_len, &parsed) == NB_OK);
            CHECK(parsed.kind == NB_RTPS_LOCATOR_KIND_UDPV4);
            CHECK(parsed.port == 7400);
            CHECK(parsed.address[12] == 192 && parsed.address[13] == 168);
            CHECK(parsed.address[14] == 1 && parsed.address[15] == 10);
            seen |= 8;
        } else if (pid == NB_RTPS_PID_ENTITY_NAME) {
            CHECK(nb_rtps_param_get_string(value, value_len, name, sizeof(name)) == NB_OK);
            CHECK(strcmp(name, "node-a") == 0);
            seen |= 16;
        }
    }
    CHECK(nb_rtps_param_reader_ok(&reader));
    CHECK(seen == 31);

    /* Truncated value length is rejected. */
    nb_rtps_param_reader_init(&reader, buf, 5);
    rc = nb_rtps_param_reader_next(&reader, &pid, &value, &value_len);
    CHECK(rc < 0);
    CHECK(!nb_rtps_param_reader_ok(&reader));
}

static void test_message_data(void)
{
    uint8_t payload[256];
    uint8_t message[512];
    nb_rtps_param_writer_t params;
    nb_rtps_header_t header;
    nb_rtps_header_t parsed_header;
    nb_rtps_time_t ts;
    nb_rtps_guid_t guid;
    nb_rtps_data_t data;
    nb_cdr_writer_t writer;
    nb_rtps_submessage_view_t view;
    nb_rtps_data_view_t data_view;
    size_t offset;
    size_t payload_len;
    int rc;
    int submessage_count = 0;
    int saw_ts = 0;
    int saw_data = 0;

    memset(&guid, 0, sizeof(guid));
    guid.prefix.bytes[0] = 0xaa;
    guid.entity = nb_rtps_eid_spdp_writer;
    nb_rtps_param_writer_init(&params, payload, sizeof(payload));
    CHECK(nb_rtps_param_put_octets2(&params, NB_RTPS_PID_PROTOCOL_VERSION, 2, 4) == NB_OK);
    CHECK(nb_rtps_param_put_guid(&params, NB_RTPS_PID_PARTICIPANT_GUID, &guid) == NB_OK);
    CHECK(nb_rtps_param_writer_finish(&params) == NB_OK);
    payload_len = nb_rtps_param_writer_len(&params);

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header.guid_prefix = guid.prefix;
    ts.seconds = 1234;
    ts.fraction = 0x80000000u;
    memset(&data, 0, sizeof(data));
    data.reader_id = nb_rtps_eid_unknown;
    data.writer_id = nb_rtps_eid_spdp_writer;
    data.writer_sn.high = 0;
    data.writer_sn.low = 7;
    data.payload = payload;
    data.payload_len = payload_len;

    rc = nb_rtps_message_init(&writer, message, sizeof(message), &header);
    CHECK(rc == NB_OK);
    CHECK(nb_rtps_info_ts(&writer, &ts, false) == NB_OK);
    CHECK(nb_rtps_data(&writer, &data) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));

    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &parsed_header, &offset) ==
          NB_OK);
    CHECK(parsed_header.version_major == 2 && parsed_header.version_minor == 4);
    CHECK(nb_rtps_guid_prefix_equal(&parsed_header.guid_prefix, &guid.prefix));
    while ((rc = nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view)) == 1) {
        submessage_count++;
        if (view.id == NB_RTPS_SUBMSG_INFO_TS) {
            CHECK(view.body_len == 8);
            saw_ts = 1;
        } else if (view.id == NB_RTPS_SUBMSG_DATA) {
            CHECK(nb_rtps_data_decode(view.flags, view.body, view.body_len, &data_view) == NB_OK);
            CHECK((view.flags & NB_RTPS_FLAG_DATA) != 0);
            CHECK((view.flags & NB_RTPS_FLAG_KEY) == 0);
            CHECK(!data_view.dispose);
            CHECK(!data_view.key);
            CHECK(nb_rtps_entity_id_equal(&data_view.writer_id, &nb_rtps_eid_spdp_writer));
            CHECK(nb_rtps_entity_id_equal(&data_view.reader_id, &nb_rtps_eid_unknown));
            CHECK(data_view.writer_sn.high == 0 && data_view.writer_sn.low == 7);
            CHECK(data_view.payload_len >= payload_len);
            CHECK(memcmp(data_view.payload, payload, payload_len) == 0);
            saw_data = 1;
        }
    }
    CHECK(rc == 0);
    CHECK(submessage_count == 2);
    CHECK(saw_ts && saw_data);

    /* A truncated message is rejected. */
    CHECK(nb_rtps_message_begin(message, 12, &parsed_header, &offset) != NB_OK);
    offset = 20;
    CHECK(nb_rtps_message_next(message, 24, &offset, &view) < 0);
}

static void test_message_data_frag(void)
{
    uint8_t payload[40];
    uint8_t message[256];
    nb_rtps_header_t header;
    nb_rtps_data_frag_t fragment;
    nb_rtps_data_frag_t parsed;
    nb_cdr_writer_t writer;
    nb_rtps_submessage_view_t view;
    size_t offset = 0;
    size_t i;

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header.guid_prefix.bytes[0] = 0x5a;

    /* Two full fragments, then a 8-byte tail: 40 = 16 + 16 + 8. */
    memset(&fragment, 0, sizeof(fragment));
    fragment.reader_id = nb_rtps_eid_unknown;
    fragment.writer_id.bytes[0] = 0x04;
    fragment.writer_id.bytes[1] = 0x02;
    fragment.writer_sn.high = 0;
    fragment.writer_sn.low = 9;
    fragment.fragment_starting_num = 1;
    fragment.fragments_in_submessage = 2;
    fragment.fragment_size = 16;
    fragment.sample_size = 40;
    fragment.payload = payload;
    fragment.payload_len = 32;

    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_data_frag(&writer, &fragment) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));

    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(view.id == NB_RTPS_SUBMSG_DATA_FRAG);
    CHECK((view.flags & NB_RTPS_FLAG_ENDIAN_LITTLE) != 0);
    CHECK(nb_rtps_data_frag_decode(view.flags, view.body, view.body_len, &parsed) == NB_OK);
    CHECK(parsed.fragment_starting_num == 1);
    CHECK(parsed.fragments_in_submessage == 2);
    CHECK(parsed.fragment_size == 16);
    CHECK(parsed.sample_size == 40);
    CHECK(parsed.writer_sn.low == 9);
    CHECK(parsed.payload_len == 32);
    CHECK(memcmp(parsed.payload, payload, 32) == 0);
    CHECK(!parsed.key);

    /* The tail fragment carries only the remaining eight octets. */
    fragment.fragment_starting_num = 3;
    fragment.fragments_in_submessage = 1;
    fragment.payload = payload + 32;
    fragment.payload_len = 8;
    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_data_frag(&writer, &fragment) == NB_OK);
    offset = 0;
    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(nb_rtps_data_frag_decode(view.flags, view.body, view.body_len, &parsed) == NB_OK);
    CHECK(parsed.payload_len == 8);
    CHECK(memcmp(parsed.payload, payload + 32, 8) == 0);

    /* Malformed fragments are rejected. */
    CHECK(nb_rtps_data_frag_decode(view.flags, view.body, 31, &parsed) != NB_OK);
    fragment.fragment_starting_num = 0;
    CHECK(nb_rtps_data_frag(&writer, &fragment) != NB_OK ||
          !nb_cdr_writer_ok(&writer));
    fragment.fragment_starting_num = 4; /* offset 48 >= sample size 40 */
    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_data_frag(&writer, &fragment) == NB_OK);
    offset = 0;
    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(nb_rtps_data_frag_decode(view.flags, view.body, view.body_len, &parsed) != NB_OK);

    /* A payload shorter than the declared fragments is rejected. */
    fragment.fragment_starting_num = 1;
    fragment.fragments_in_submessage = 2;
    fragment.payload_len = 16;
    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_data_frag(&writer, &fragment) == NB_OK);
    offset = 0;
    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(nb_rtps_data_frag_decode(view.flags, view.body, view.body_len, &parsed) != NB_OK);
}

static void test_message_frag_repair(void)
{
    uint8_t message[256];
    nb_rtps_header_t header;
    nb_rtps_header_t parsed_header;
    nb_rtps_nack_frag_t nack;
    nb_rtps_nack_frag_t parsed_nack;
    nb_rtps_heartbeat_frag_t heartbeat;
    nb_rtps_heartbeat_frag_t parsed_heartbeat;
    nb_cdr_writer_t writer;
    nb_rtps_submessage_view_t view;
    size_t offset = 0;

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    header.guid_prefix.bytes[0] = 0x5a;

    /* NACK_FRAG asks for fragments 3 and 5 of a sample. */
    memset(&nack, 0, sizeof(nack));
    nack.reader_id = nb_rtps_eid_unknown;
    nack.writer_id.bytes[0] = 0x04;
    nack.writer_id.bytes[1] = 0x02;
    nack.writer_sn.high = 0;
    nack.writer_sn.low = 11;
    nack.bitmap_base = 3;
    nack.num_bits = 32;
    nack.bitmap[0] = 0x90000000u; /* bits 0 and 2: relative fragments 3 and 5 */
    nack.count = 7;

    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_nack_frag(&writer, &nack) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));
    offset = 0;
    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &parsed_header, &offset) ==
          NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(view.id == NB_RTPS_SUBMSG_NACK_FRAG);
    /* readerId + writerId + SN + bitmapBase + numBits + bitmap + count */
    CHECK(view.body_len == 32);
    CHECK(nb_rtps_nack_frag_decode(view.body, view.body_len, &parsed_nack) == NB_OK);
    CHECK(nb_rtps_entity_id_equal(&parsed_nack.reader_id, &nack.reader_id));
    CHECK(memcmp(parsed_nack.writer_id.bytes, nack.writer_id.bytes, NB_RTPS_ENTITY_ID_SIZE) == 0);
    CHECK(parsed_nack.writer_sn.low == 11);
    CHECK(parsed_nack.bitmap_base == 3);
    CHECK(parsed_nack.num_bits == 32);
    CHECK(parsed_nack.bitmap[0] == 0x90000000u);
    CHECK(parsed_nack.count == 7);

    /* Truncated and oversized bitmaps are rejected. */
    CHECK(nb_rtps_nack_frag_decode(view.body, view.body_len - 1, &parsed_nack) != NB_OK);
    nack.num_bits = NB_RTPS_FRAG_NUMBER_SET_MAX_BITS + 1u;
    CHECK(nb_rtps_nack_frag(&writer, &nack) != NB_OK || !nb_cdr_writer_ok(&writer));

    /* HEARTBEAT_FRAG announces the fragment count of a sample. */
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.reader_id = nb_rtps_eid_unknown;
    heartbeat.writer_id.bytes[0] = 0x04;
    heartbeat.writer_id.bytes[1] = 0x02;
    heartbeat.writer_sn.high = 0;
    heartbeat.writer_sn.low = 12;
    heartbeat.last_fragment_num = 64;
    heartbeat.count = 3;

    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_heartbeat_frag(&writer, &heartbeat) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));
    offset = 0;
    CHECK(nb_rtps_message_begin(message, nb_cdr_writer_len(&writer), &parsed_header, &offset) ==
          NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(view.id == NB_RTPS_SUBMSG_HEARTBEAT_FRAG);
    /* readerId + writerId + SN + lastFragmentNum + count; no fragment size. */
    CHECK(view.body_len == 24);
    CHECK(nb_rtps_heartbeat_frag_decode(view.body, view.body_len, &parsed_heartbeat) == NB_OK);
    CHECK(parsed_heartbeat.writer_sn.low == 12);
    CHECK(parsed_heartbeat.last_fragment_num == 64);
    CHECK(parsed_heartbeat.count == 3);
    CHECK(nb_rtps_heartbeat_frag_decode(view.body, 20, &parsed_heartbeat) != NB_OK);
}

static void test_heartbeat_acknack(void)
{
    uint8_t buf[256];
    nb_cdr_writer_t writer;
    nb_rtps_header_t header;
    nb_rtps_heartbeat_t heartbeat;
    nb_rtps_heartbeat_t parsed_heartbeat;
    nb_rtps_acknack_t acknack;
    nb_rtps_acknack_t parsed_acknack;
    nb_rtps_submessage_view_t view;
    size_t offset;
    int rc;
    int count = 0;

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    memset(&heartbeat, 0, sizeof(heartbeat));
    heartbeat.reader_id = nb_rtps_eid_unknown;
    heartbeat.writer_id = nb_rtps_eid_sedp_publications_writer;
    heartbeat.first_sn.low = 1;
    heartbeat.last_sn.low = 9;
    heartbeat.count = 3;
    /* MANUAL_BY_TOPIC asserts liveliness through a heartbeat with the L flag. */
    heartbeat.liveliness = 1;
    memset(&acknack, 0, sizeof(acknack));
    acknack.reader_id = nb_rtps_eid_sedp_publications_reader;
    acknack.writer_id = nb_rtps_eid_sedp_publications_writer;
    acknack.base.low = 1;
    acknack.num_bits = 33;
    acknack.bitmap[0] = 0xffffffffu;
    acknack.bitmap[1] = 0x00000001u;
    acknack.count = 11;

    CHECK(nb_rtps_message_init(&writer, buf, sizeof(buf), &header) == NB_OK);
    CHECK(nb_rtps_heartbeat(&writer, &heartbeat) == NB_OK);
    CHECK(nb_rtps_acknack(&writer, &acknack) == NB_OK);
    CHECK(nb_rtps_message_begin(buf, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    while ((rc = nb_rtps_message_next(buf, nb_cdr_writer_len(&writer), &offset, &view)) == 1) {
        count++;
        if (view.id == NB_RTPS_SUBMSG_HEARTBEAT) {
            CHECK((view.flags & NB_RTPS_FLAG_HEARTBEAT_LIVELINESS) != 0);
            /* ack_requested is zero, so the encoder must set FINAL. */
            CHECK((view.flags & NB_RTPS_FLAG_HEARTBEAT_FINAL) != 0);
            CHECK(nb_rtps_heartbeat_decode(view.flags, view.body, view.body_len,
                                           &parsed_heartbeat) == NB_OK);
            CHECK(parsed_heartbeat.liveliness == 1);
            CHECK(parsed_heartbeat.ack_requested == 0);
            CHECK(nb_rtps_entity_id_equal(&parsed_heartbeat.writer_id,
                                          &nb_rtps_eid_sedp_publications_writer));
            CHECK(parsed_heartbeat.first_sn.low == 1);
            CHECK(parsed_heartbeat.last_sn.low == 9);
            CHECK(parsed_heartbeat.count == 3);
        } else if (view.id == NB_RTPS_SUBMSG_ACKNACK) {
            /* This ACKNACK asks for samples, so it must not claim "no
             * response required". */
            CHECK((view.flags & NB_RTPS_ACKNACK_FLAG_FINAL) == 0);
            CHECK(nb_rtps_acknack_decode(view.body, view.body_len, &parsed_acknack) == NB_OK);
            CHECK(parsed_acknack.num_bits == 33);
            CHECK(parsed_acknack.bitmap[0] == 0xffffffffu);
            CHECK(parsed_acknack.bitmap[1] == 0x00000001u);
            CHECK(parsed_acknack.count == 11);
        }
    }
    CHECK(rc == 0);
    CHECK(count == 2);
}

static void test_gap(void)
{
    uint8_t buf[256];
    nb_cdr_writer_t writer;
    nb_rtps_header_t header;
    nb_rtps_gap_t gap;
    nb_rtps_gap_t parsed;
    nb_rtps_submessage_view_t view;
    uint8_t truncated[36];
    size_t offset;
    int rc;
    int count = 0;

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;

    memset(&gap, 0, sizeof(gap));
    gap.reader_id = nb_rtps_eid_unknown;
    gap.writer_id = nb_rtps_eid_sedp_publications_writer;
    gap.gap_start.low = 2;
    gap.base.low = 5;
    gap.num_bits = 3;
    gap.bitmap[0] = 0xa0000000u; /* gaps 5 and 7 */

    CHECK(nb_rtps_message_init(&writer, buf, sizeof(buf), &header) == NB_OK);
    CHECK(nb_rtps_gap(&writer, &gap) == NB_OK);
    CHECK(nb_rtps_message_begin(buf, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    while ((rc = nb_rtps_message_next(buf, nb_cdr_writer_len(&writer), &offset, &view)) == 1) {
        count++;
        CHECK(view.id == NB_RTPS_SUBMSG_GAP);
        CHECK(nb_rtps_gap_decode(view.body, view.body_len, &parsed) == NB_OK);
        CHECK(nb_rtps_entity_id_equal(&parsed.writer_id,
                                      &nb_rtps_eid_sedp_publications_writer));
        CHECK(nb_rtps_entity_id_equal(&parsed.reader_id, &nb_rtps_eid_unknown));
        CHECK(parsed.gap_start.low == 2);
        CHECK(parsed.base.low == 5);
        CHECK(parsed.num_bits == 3);
        CHECK(parsed.bitmap[0] == 0xa0000000u);
    }
    CHECK(rc == 0);
    CHECK(count == 1);

    /* A submessage header plus a body shorter than the fixed fields must fail
     * without reading past the buffer. */
    memcpy(truncated, buf + NB_RTPS_HEADER_SIZE, sizeof(truncated));
    CHECK(nb_rtps_gap_decode(truncated, 27, &parsed) == NB_ERR_BAD_MESSAGE);
    CHECK(nb_rtps_gap_decode(truncated, 4, &parsed) == NB_ERR_BAD_MESSAGE);
}

/*
 * SEDP samples travel inline in one DATA submessage, so the serialized
 * ParameterList must stay below NB_RTPS_DATA_PAYLOAD_MAX. Every variable-length
 * member is bounded by the public nb_rtps_endpoint_info_t buffers; this test
 * fills all of them to the maximum and pins the resulting size.
 */
static void test_sedp_payload_bound(void)
{
    uint8_t buf[4096];
    nb_rtps_endpoint_info_t info;
    size_t len = 0;
    size_t i;

    memset(&info, 0, sizeof(info));
    info.endpoint_guid.prefix.bytes[0] = 0x11;
    info.endpoint_guid.entity = nb_rtps_eid_spdp_writer;
    memset(info.topic_name, 't', sizeof(info.topic_name) - 1u);
    memset(info.type_name, 'T', sizeof(info.type_name) - 1u);
    memset(info.partition, 'p', sizeof(info.partition) - 1u);
    info.reliability = NB_RTPS_RELIABILITY_RELIABLE;
    info.durability = NB_RTPS_DURABILITY_VOLATILE;
    info.history_kind = NB_RTPS_HISTORY_KEEP_LAST;
    info.history_depth = 16;
    info.unicast_count = NB_RTPS_SEDP_LOCATORS_MAX;
    info.multicast_count = NB_RTPS_SEDP_LOCATORS_MAX;
    for (i = 0; i < NB_RTPS_SEDP_LOCATORS_MAX; i++) {
        info.unicast[i].kind = NB_RTPS_LOCATOR_KIND_UDPV4;
        info.unicast[i].port = 7410;
        info.multicast[i].kind = NB_RTPS_LOCATOR_KIND_UDPV4;
        info.multicast[i].port = 7400;
    }
    info.user_data_len = sizeof(info.user_data);
    memset(info.user_data, 'u', info.user_data_len);

    CHECK(nb_rtps_sedp_build(&info, buf, sizeof(buf), &len) == NB_OK);
    nb_log_info("maximal SEDP payload is %u bytes", (unsigned)len);
    CHECK(len > 0);
    /* Measured 900 bytes with every buffer full; 1000 keeps a guard band
     * without pinning the exact parameter layout. */
    CHECK(len <= 1000);
    CHECK(len <= NB_RTPS_DATA_PAYLOAD_MAX);

    /* Round trip keeps the strings and the locator counts. */
    {
        nb_rtps_endpoint_info_t parsed;

        CHECK(nb_rtps_sedp_parse(buf, len, &parsed) == NB_OK);
        CHECK(strcmp(parsed.topic_name, info.topic_name) == 0);
        CHECK(strcmp(parsed.type_name, info.type_name) == 0);
        CHECK(parsed.unicast_count == info.unicast_count);
        CHECK(parsed.user_data_len == info.user_data_len);
    }
}

static void test_spdp_roundtrip(void)
{
    uint8_t buf[512];
    nb_rtps_participant_info_t info;
    nb_rtps_participant_info_t parsed;
    nb_rtps_param_writer_t writer;
    nb_rtps_duration_t lease;
    size_t len = 0;

    memset(&info, 0, sizeof(info));
    info.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    info.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    info.guid.prefix.bytes[0] = 0x12;
    info.guid.prefix.bytes[11] = 0x34;
    info.guid.entity = nb_rtps_eid_spdp_writer;
    info.lease_duration.seconds = 20;
    info.builtin_endpoints = NB_RTPS_BUILTIN_ENDPOINT_PARTICIPANT_ANNOUNCER |
                             NB_RTPS_BUILTIN_ENDPOINT_PUBLICATIONS_ANNOUNCER;
    nb_rtps_locator_ipv4(&info.metatraffic_unicast, 192, 168, 1, 10, 7410);
    nb_rtps_locator_ipv4(&info.default_unicast, 192, 168, 1, 10, 42420);
    memcpy(info.name, "node-a", 7);
    info.user_data[0] = 0xde;
    info.user_data[1] = 0xad;
    info.user_data_len = 2;

    CHECK(nb_rtps_spdp_build(&info, buf, sizeof(buf), &len) == NB_OK);
    CHECK(len > 0 && len % 4 == 0);
    CHECK(len >= 4);
    CHECK(buf[0] == 0x00);
    CHECK(buf[1] == (uint8_t)NB_RTPS_ENCAPSULATION_PL_CDR_LE);
    CHECK(buf[2] == 0x00 && buf[3] == 0x00);
    CHECK(nb_rtps_spdp_parse(buf, len, &parsed) == NB_OK);
    CHECK(parsed.vendor_id[0] == NB_RTPS_VENDOR_ID_0);
    CHECK(nb_rtps_guid_equal(&parsed.guid, &info.guid));
    CHECK(parsed.lease_duration.seconds == 20);
    CHECK(parsed.builtin_endpoints == info.builtin_endpoints);
    CHECK(parsed.metatraffic_unicast.port == 7410);
    CHECK(parsed.metatraffic_unicast.address[12] == 192);
    CHECK(parsed.metatraffic_unicast.address[13] == 168);
    CHECK(parsed.metatraffic_unicast.address[14] == 1);
    CHECK(parsed.metatraffic_unicast.address[15] == 10);
    CHECK(parsed.default_unicast.port == 42420);
    CHECK(strcmp(parsed.name, "node-a") == 0);
    CHECK(parsed.user_data_len == 2);
    CHECK(parsed.user_data[0] == 0xde && parsed.user_data[1] == 0xad);

    /* Missing mandatory participant GUID. */
    lease.seconds = 20;
    lease.fraction = 0;
    nb_rtps_param_writer_init(&writer, buf, sizeof(buf));
    CHECK(nb_rtps_param_put_octets2(&writer, NB_RTPS_PID_PROTOCOL_VERSION, 2, 4) == NB_OK);
    CHECK(nb_rtps_param_put_duration(
              &writer, NB_RTPS_PID_PARTICIPANT_LEASE_DURATION, &lease) == NB_OK);
    CHECK(nb_rtps_param_put_u32(&writer, NB_RTPS_PID_BUILTIN_ENDPOINT_SET, 1) == NB_OK);
    CHECK(nb_rtps_param_put_locator(
              &writer, NB_RTPS_PID_METATRAFFIC_UNICAST_LOCATOR,
              &info.metatraffic_unicast) == NB_OK);
    CHECK(nb_rtps_param_writer_finish(&writer) == NB_OK);
    CHECK(nb_rtps_spdp_parse(buf, nb_rtps_param_writer_len(&writer), &parsed) ==
          NB_ERR_BAD_MESSAGE);

    /* Truncated payload. */
    CHECK(nb_rtps_spdp_build(&info, buf, sizeof(buf), &len) == NB_OK);
    CHECK(nb_rtps_spdp_parse(buf, len - 2, &parsed) != NB_OK);
}

/*
 * ParticipantMessageData: the wire layout has to match Cyclone DDS byte for
 * byte, because it is how an automatic liveliness lease is renewed. A 28-byte
 * payload is a 4-byte CDR_LE header, the 12-byte prefix, the kind, and a
 * one-octet sequence padded to four bytes.
 */
static void test_participant_message_data(void)
{
    uint8_t payload[64];
    nb_rtps_pmd_t pmd;
    nb_rtps_pmd_t parsed;
    size_t len = 0;
    size_t i;

    memset(&pmd, 0, sizeof(pmd));
    for (i = 0; i < NB_RTPS_GUID_PREFIX_SIZE; i++) {
        pmd.participant.bytes[i] = (uint8_t)(0x10u + i);
    }
    pmd.kind = NB_RTPS_PMD_KIND_AUTOMATIC;
    CHECK(nb_rtps_pmd_encode(&pmd, payload, sizeof(payload), &len) == NB_OK);
    CHECK(len == 28);
    CHECK(payload[0] == 0x00 && payload[1] == 0x01);
    CHECK(payload[2] == 0x00 && payload[3] == 0x00);
    CHECK(payload[4] == 0x10 && payload[5] == 0x11);
    CHECK(payload[16] == NB_RTPS_PMD_KIND_AUTOMATIC);
    CHECK(payload[17] == 0 && payload[18] == 0 && payload[19] == 0);
    /* sequence<octet> length 1 followed by the octet and three pad bytes. */
    CHECK(payload[20] == 1 && payload[21] == 0 && payload[22] == 0 && payload[23] == 0);
    CHECK(payload[24] == 0);

    memset(&parsed, 0, sizeof(parsed));
    CHECK(nb_rtps_pmd_decode(payload, len, &parsed) == NB_OK);
    CHECK(parsed.kind == NB_RTPS_PMD_KIND_AUTOMATIC);
    CHECK(nb_rtps_guid_prefix_equal(&parsed.participant, &pmd.participant));

    /* A manual assertion decodes too: peers use it for manual liveliness. */
    pmd.kind = NB_RTPS_PMD_KIND_MANUAL;
    CHECK(nb_rtps_pmd_encode(&pmd, payload, sizeof(payload), &len) == NB_OK);
    CHECK(nb_rtps_pmd_decode(payload, len, &parsed) == NB_OK);
    CHECK(parsed.kind == NB_RTPS_PMD_KIND_MANUAL);

    /* Truncated header, truncated body, and a sequence longer than the
     * payload are all rejected instead of read out of bounds. */
    CHECK(nb_rtps_pmd_decode(payload, 3, &parsed) != NB_OK);
    CHECK(nb_rtps_pmd_decode(payload, 20, &parsed) != NB_OK);
    memcpy(payload + NB_RTPS_CDR_HEADER_SIZE + NB_RTPS_GUID_PREFIX_SIZE + 4u,
           "\x08\x00\x00\x00",
           4);
    CHECK(nb_rtps_pmd_decode(payload, 28, &parsed) != NB_OK);
}

/*
 * INFO_TS carries the source timestamp that BY_SOURCE_TIMESTAMP ordering
 * compares, so it has to survive a round trip and reject bodies it cannot read.
 */
/*
 * The status parameters a writer puts in a DATA inline QoS. A dispose is
 * PID_KEY_HASH plus PID_STATUS_INFO with the disposed bit; an unregister sets
 * the unregistered bit instead.
 */
static void test_inline_qos_status(void)
{
    uint8_t qos[64];
    uint8_t handle[NB_RTPS_KEY_HASH_SIZE];
    uint8_t key_hash[NB_RTPS_KEY_HASH_SIZE];
    uint8_t disposed[4] = {0x00, 0x00, 0x00, 0x01};
    uint8_t unregistered[4] = {0x00, 0x00, 0x00, 0x02};
    nb_rtps_param_writer_t writer;
    size_t len;
    int is_disposed = -1;
    int is_unregistered = -1;
    int has_key = -1;
    int i;

    for (i = 0; i < (int)sizeof(handle); i++) {
        handle[i] = (uint8_t)(0xa0 + i);
    }

    nb_rtps_param_writer_init(&writer, qos, sizeof(qos));
    CHECK(nb_rtps_param_put_bytes(
              &writer, NB_RTPS_PID_KEY_HASH, handle, sizeof(handle)) == NB_OK);
    CHECK(nb_rtps_param_put_bytes(
              &writer, NB_RTPS_PID_STATUS_INFO, disposed, sizeof(disposed)) == NB_OK);
    CHECK(nb_rtps_param_writer_finish(&writer) == NB_OK);
    len = nb_rtps_param_writer_len(&writer);

    CHECK(nb_rtps_inline_qos_status(
              qos, len, &is_disposed, &is_unregistered, key_hash, &has_key) == NB_OK);
    CHECK(is_disposed == 1);
    CHECK(is_unregistered == 0);
    CHECK(has_key == 1);
    CHECK(memcmp(key_hash, handle, sizeof(handle)) == 0);

    nb_rtps_param_writer_init(&writer, qos, sizeof(qos));
    CHECK(nb_rtps_param_put_bytes(
              &writer, NB_RTPS_PID_KEY_HASH, handle, sizeof(handle)) == NB_OK);
    CHECK(nb_rtps_param_put_bytes(
              &writer, NB_RTPS_PID_STATUS_INFO, unregistered, sizeof(unregistered)) ==
          NB_OK);
    CHECK(nb_rtps_param_writer_finish(&writer) == NB_OK);
    len = nb_rtps_param_writer_len(&writer);

    CHECK(nb_rtps_inline_qos_status(
              qos, len, &is_disposed, &is_unregistered, key_hash, &has_key) == NB_OK);
    CHECK(is_disposed == 0);
    CHECK(is_unregistered == 1);
    CHECK(has_key == 1);

    /* No inline QoS at all clears every output. */
    is_disposed = 1;
    is_unregistered = 1;
    has_key = 1;
    CHECK(nb_rtps_inline_qos_status(
              NULL, 0, &is_disposed, &is_unregistered, key_hash, &has_key) == NB_OK);
    CHECK(is_disposed == 0);
    CHECK(is_unregistered == 0);
    CHECK(has_key == 0);

    /* An empty parameter list behaves the same way. */
    nb_rtps_param_writer_init(&writer, qos, sizeof(qos));
    CHECK(nb_rtps_param_writer_finish(&writer) == NB_OK);
    has_key = 1;
    CHECK(nb_rtps_inline_qos_status(qos,
                                    nb_rtps_param_writer_len(&writer),
                                    &is_disposed,
                                    &is_unregistered,
                                    key_hash,
                                    &has_key) == NB_OK);
    CHECK(has_key == 0);
}

static void test_info_ts_roundtrip(void)
{
    uint8_t message[128];
    nb_rtps_header_t header;
    nb_rtps_time_t ts;
    nb_rtps_time_t parsed;
    nb_cdr_writer_t writer;
    nb_rtps_submessage_view_t view;
    size_t offset = 0;

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;

    ts.seconds = 1234;
    ts.fraction = 0x80000000u; /* exactly half a second */
    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_info_ts(&writer, &ts, false) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));

    CHECK(nb_rtps_message_begin(
              message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(view.id == NB_RTPS_SUBMSG_INFO_TS);
    CHECK(nb_rtps_info_ts_decode(view.flags, view.body, view.body_len, &parsed) == NB_OK);
    CHECK(parsed.seconds == ts.seconds);
    CHECK(parsed.fraction == ts.fraction);

    /* A short body is not readable, and a big-endian body is rejected rather
     * than decoded with the wrong byte order. */
    CHECK(nb_rtps_info_ts_decode(NB_RTPS_FLAG_ENDIAN_LITTLE, view.body, 4, &parsed) !=
          NB_OK);
    CHECK(nb_rtps_info_ts_decode(0, view.body, view.body_len, &parsed) != NB_OK);
}

static void test_info_dst_src_roundtrip(void)
{
    uint8_t message[128];
    nb_rtps_header_t header;
    nb_rtps_guid_prefix_t sent;
    nb_rtps_guid_prefix_t parsed;
    nb_cdr_writer_t writer;
    nb_rtps_submessage_view_t view;
    size_t offset = 0;
    size_t i;

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    for (i = 0; i < NB_RTPS_GUID_PREFIX_SIZE; i++) {
        sent.bytes[i] = (uint8_t)(0x10u + i);
    }

    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_info_dst(&writer, &sent) == NB_OK);
    CHECK(nb_rtps_info_src(&writer, &sent) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));

    CHECK(nb_rtps_message_begin(
              message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(view.id == NB_RTPS_SUBMSG_INFO_DST);
    CHECK(nb_rtps_info_dst_decode(view.flags, view.body, view.body_len, &parsed) == NB_OK);
    CHECK(memcmp(parsed.bytes, sent.bytes, sizeof(sent.bytes)) == 0);

    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &view) == 1);
    CHECK(view.id == NB_RTPS_SUBMSG_INFO_SRC);
    CHECK(nb_rtps_info_src_decode(view.flags, view.body, view.body_len, &parsed) == NB_OK);
    CHECK(memcmp(parsed.bytes, sent.bytes, sizeof(sent.bytes)) == 0);

    /* Short bodies and big-endian encoding are rejected. */
    CHECK(nb_rtps_info_dst_decode(NB_RTPS_FLAG_ENDIAN_LITTLE, view.body, 4, &parsed) !=
          NB_OK);
    CHECK(nb_rtps_info_src_decode(NB_RTPS_FLAG_ENDIAN_LITTLE, view.body, 8, &parsed) !=
          NB_OK);
    CHECK(nb_rtps_info_dst_decode(0, view.body, view.body_len, &parsed) != NB_OK);
    CHECK(nb_rtps_info_src_decode(0, view.body, view.body_len, &parsed) != NB_OK);
}

static void test_sequence_number_64(void)
{
    nb_rtps_reliable_writer_t *writer = nb_rtps_reliable_writer_create(8);
    nb_rtps_reader_state_t reader;
    nb_rtps_acknack_t acknack;
    uint64_t sn = 0;
    uint64_t first = 0;
    uint64_t last = 0;
    const uint8_t *payload = NULL;
    size_t len = 0;
    int dispose = 0;

    CHECK(writer != NULL);
    if (!writer) {
        return;
    }
    nb_rtps_reliable_seed(writer, 0xfffffffeull);
    CHECK(nb_rtps_reliable_add(writer, "a", 1, 0, &sn) == NB_OK);
    CHECK(sn == 0xffffffffull);
    CHECK(nb_rtps_reliable_add(writer, "b", 1, 0, &sn) == NB_OK);
    CHECK(sn == 0x100000000ull);
    nb_rtps_reliable_state(writer, &first, &last, NULL);
    CHECK(first == 0xffffffffull);
    CHECK(last == 0x100000000ull);
    CHECK(nb_rtps_reliable_get(writer, 0x100000000ull, &payload, &len, &dispose) == NB_OK);
    CHECK(len == 1 && payload && payload[0] == 'b');
    nb_rtps_reliable_writer_destroy(writer);

    /* Reader window across the 2^32 boundary. */
    nb_rtps_reader_state_reset(&reader, 0xffffffffull);
    CHECK(nb_rtps_reader_on_sample(&reader, 0xffffffffull) == 1);
    CHECK(reader.next_expected == 0x100000000ull);
    CHECK(nb_rtps_reader_on_sample(&reader, 0x100000002ull) == 1);
    CHECK(nb_rtps_reader_on_sample(&reader, 0x100000001ull) == 1);
    CHECK(nb_rtps_reader_build_acknack(&reader, 0x100000002ull, &acknack) == 1);
    CHECK(acknack.base.high == 1 && acknack.base.low == 0);
    CHECK(acknack.num_bits == 3);
    CHECK((acknack.bitmap[0] & 0x80000000u) != 0);
    CHECK(nb_rtps_reader_on_sample(&reader, 0x100000000ull) == 1);
    CHECK(reader.next_expected == 0x100000003ull);
}


static void test_sequence_number_wire(void)
{
    uint8_t message[128];
    nb_rtps_header_t header;
    nb_rtps_data_t data;
    nb_rtps_data_view_t view;
    nb_cdr_writer_t writer;
    nb_rtps_submessage_view_t sub;
    size_t offset = 0;
    static const uint8_t body[4] = {0, 1, 0, 0};

    memset(&header, 0, sizeof(header));
    header.version_major = NB_RTPS_PROTOCOL_VERSION_MAJOR;
    header.version_minor = NB_RTPS_PROTOCOL_VERSION_MINOR;
    header.vendor_id[0] = NB_RTPS_VENDOR_ID_0;
    header.vendor_id[1] = NB_RTPS_VENDOR_ID_1;
    memset(&data, 0, sizeof(data));
    data.writer_sn.high = 1;
    data.writer_sn.low = 2;
    data.payload = body;
    data.payload_len = sizeof(body);

    CHECK(nb_rtps_message_init(&writer, message, sizeof(message), &header) == NB_OK);
    CHECK(nb_rtps_data(&writer, &data) == NB_OK);
    CHECK(nb_cdr_writer_ok(&writer));
    CHECK(nb_rtps_message_begin(
              message, nb_cdr_writer_len(&writer), &header, &offset) == NB_OK);
    CHECK(nb_rtps_message_next(message, nb_cdr_writer_len(&writer), &offset, &sub) == 1);
    CHECK(sub.id == NB_RTPS_SUBMSG_DATA);
    CHECK(nb_rtps_data_decode(sub.flags, sub.body, sub.body_len, &view) == NB_OK);
    CHECK(nb_rtps_sn_to_u64(&view.writer_sn) == 0x100000002ull);
}

int main(void)
{
    test_cdr_roundtrip();
    test_dds_message_cdr();
    test_param_roundtrip();
    test_message_data();
    test_message_data_frag();
    test_message_frag_repair();
    test_heartbeat_acknack();
    test_participant_message_data();
    test_inline_qos_status();
    test_info_ts_roundtrip();
    test_info_dst_src_roundtrip();
    test_sequence_number_64();
    test_sequence_number_wire();
    test_sedp_payload_bound();
    test_gap();
    test_spdp_roundtrip();
    if (g_fail) {
        nb_log_error("rtps wire tests failed");
        return 1;
    }
    printf("rtps wire tests passed\n");
    return 0;
}
