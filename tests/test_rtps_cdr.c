#include "rtps_cdr.h"

#define NB_LOG_TAG "test"
#include <nb/log.h>

#include <string.h>

static int g_fail;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            nb_log_error("FAIL %s:%d: %s", __FILE__, __LINE__, #cond);  \
            g_fail = 1;                                                 \
        }                                                               \
    } while (0)

/*
 * Pinned encoding: little-endian CDR with 2/4/8-byte alignment and zero
 * padding, strings as a u32 length (including the NUL) followed by the bytes,
 * and octet arrays without alignment. The buffer is poisoned first so a codec
 * that leaves alignment padding uninitialised fails the comparison.
 */
static const uint8_t g_expected[] = {
    0x11, 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
    0xbb, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0x03, 0x00, 0x00, 0x00, 0x68, 0x69, 0x00, 0x01,
    0x02,
};

static void fill_expected(nb_cdr_writer_t *writer)
{
    nb_cdr_put_u8(writer, 0x11);
    nb_cdr_put_u32(writer, 0x11223344u);
    nb_cdr_put_u16(writer, 0xaabbu);
    nb_cdr_put_u64(writer, 0x1122334455667788ull);
    nb_cdr_put_string(writer, "hi");
    nb_cdr_put_bytes(writer, "\x01\x02", 2);
}

static void test_writer_golden(void)
{
    uint8_t buf[64];
    nb_cdr_writer_t writer;

    memset(buf, 0xcc, sizeof(buf));
    nb_cdr_writer_init(&writer, buf, sizeof(buf));
    fill_expected(&writer);
    CHECK(nb_cdr_writer_ok(&writer));
    CHECK(nb_cdr_writer_len(&writer) == sizeof(g_expected));
    CHECK(memcmp(buf, g_expected, sizeof(g_expected)) == 0);
}

static void test_reader_golden(void)
{
    nb_cdr_reader_t reader;
    const uint8_t *bytes;

    nb_cdr_reader_init(&reader, g_expected, sizeof(g_expected));
    CHECK(nb_cdr_get_u8(&reader) == 0x11);
    CHECK(nb_cdr_get_u32(&reader) == 0x11223344u);
    CHECK(nb_cdr_get_u16(&reader) == 0xaabbu);
    CHECK(nb_cdr_get_u64(&reader) == 0x1122334455667788ull);
    {
        char text[4];

        CHECK(nb_cdr_get_string(&reader, text, sizeof(text)));
        CHECK(strcmp(text, "hi") == 0);
    }
    bytes = nb_cdr_get_bytes(&reader, 2);
    CHECK(bytes != NULL);
    if (bytes) {
        /* Zero-copy: the pointer addresses the caller's buffer. */
        CHECK(bytes == g_expected + 31);
        CHECK(bytes[0] == 0x01 && bytes[1] == 0x02);
    }
    CHECK(nb_cdr_reader_ok(&reader));
    CHECK(nb_cdr_reader_remaining(&reader) == 0);
}

static void test_string_edge_cases(void)
{
    uint8_t buf[16];
    nb_cdr_writer_t writer;
    nb_cdr_reader_t reader;
    char small[2];

    nb_cdr_writer_init(&writer, buf, sizeof(buf));
    nb_cdr_put_string(&writer, "");
    CHECK(nb_cdr_writer_ok(&writer));
    CHECK(nb_cdr_writer_len(&writer) == 5);

    /* Skip-copy mode consumes the value without a destination. */
    nb_cdr_reader_init(&reader, buf, nb_cdr_writer_len(&writer));
    CHECK(nb_cdr_get_string(&reader, NULL, 0));
    CHECK(nb_cdr_reader_ok(&reader));

    /* Too small a destination fails instead of truncating. */
    nb_cdr_writer_init(&writer, buf, sizeof(buf));
    nb_cdr_put_string(&writer, "hi");
    CHECK(nb_cdr_writer_ok(&writer));
    nb_cdr_reader_init(&reader, buf, nb_cdr_writer_len(&writer));
    CHECK(!nb_cdr_get_string(&reader, small, sizeof(small)));
    CHECK(!nb_cdr_reader_ok(&reader));
}

static void test_bounds(void)
{
    uint8_t buf[8];
    nb_cdr_writer_t writer;
    nb_cdr_reader_t reader;
    uint32_t value = 0;

    nb_cdr_writer_init(&writer, buf, 4);
    nb_cdr_put_u32(&writer, 1);
    CHECK(nb_cdr_writer_ok(&writer));
    nb_cdr_put_u8(&writer, 2);
    CHECK(!nb_cdr_writer_ok(&writer));
    CHECK(nb_cdr_writer_len(&writer) == 4);

    /* Alignment padding that does not fit must fail, not overrun. */
    nb_cdr_writer_init(&writer, buf, 4);
    nb_cdr_put_u8(&writer, 1);
    nb_cdr_put_u64(&writer, 2);
    CHECK(!nb_cdr_writer_ok(&writer));

    nb_cdr_reader_init(&reader, buf, 3);
    CHECK(nb_cdr_get_u32(&reader) == 0);
    CHECK(!nb_cdr_reader_ok(&reader));

    /* A sequence length larger than the buffer must not expose a pointer. */
    nb_cdr_reader_init(&reader, buf, 8);
    value = 0xffffffffu;
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xff;
    buf[1] = 0xff;
    buf[2] = 0xff;
    buf[3] = 0xff;
    nb_cdr_reader_init(&reader, buf, sizeof(buf));
    CHECK(nb_cdr_get_u32(&reader) == value);
    CHECK(nb_cdr_get_bytes(&reader, 0xffffffffu) == NULL);
    CHECK(!nb_cdr_reader_ok(&reader));
}

int main(void)
{
    test_writer_golden();
    test_reader_golden();
    test_string_edge_cases();
    test_bounds();
    if (g_fail) {
        nb_log_error("rtps cdr tests failed");
        return 1;
    }
    nb_log_info("rtps cdr tests passed");
    return 0;
}
