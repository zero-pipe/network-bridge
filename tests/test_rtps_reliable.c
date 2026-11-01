#include "rtps_reliable.h"

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

typedef struct resend_state {
    int count;
    uint32_t sn[8];
    char payload[8][8];
    int dispose[8];
} resend_state_t;

static int on_resend(void *user,
                     uint32_t sn,
                     const uint8_t *payload,
                     size_t len,
                     int dispose)
{
    resend_state_t *state = (resend_state_t *)user;

    if (state->count >= (int)(sizeof(state->sn) / sizeof(state->sn[0]))) {
        return NB_ERR_TOOBIG;
    }
    state->sn[state->count] = sn;
    if (len >= sizeof(state->payload[0])) {
        len = sizeof(state->payload[0]) - 1;
    }
    if (len > 0) {
        memcpy(state->payload[state->count], payload, len);
    }
    state->payload[state->count][len] = '\0';
    state->dispose[state->count] = dispose;
    state->count++;
    return NB_OK;
}

static void test_history_and_acknack(void)
{
    nb_rtps_reliable_writer_t *writer = nb_rtps_reliable_writer_create(2);
    nb_rtps_acknack_t ack;
    resend_state_t state;
    uint32_t sn = 0;
    uint32_t first = 0;
    uint32_t last = 0;
    size_t count = 0;
    const uint8_t *payload = NULL;
    size_t len = 0;
    int dispose = 0;

    CHECK(writer != NULL);
    if (!writer) {
        return;
    }
    CHECK(nb_rtps_reliable_add(writer, "a", 1, 0, &sn) == NB_OK && sn == 1);
    CHECK(nb_rtps_reliable_add(writer, "b", 1, 0, &sn) == NB_OK && sn == 2);
    CHECK(nb_rtps_reliable_add(writer, "c", 1, 0, &sn) == NB_OK && sn == 3);
    nb_rtps_reliable_state(writer, &first, &last, &count);
    CHECK(first == 2 && last == 3 && count == 2);
    CHECK(nb_rtps_reliable_get(writer, 1, &payload, &len, &dispose) == NB_ERR_NOTFOUND);
    CHECK(nb_rtps_reliable_get(writer, 2, &payload, &len, &dispose) == NB_OK);
    CHECK(len == 1 && payload[0] == 'b' && !dispose);
    CHECK(nb_rtps_reliable_get(writer, 3, &payload, &len, &dispose) == NB_OK);
    CHECK(len == 1 && payload[0] == 'c');

    memset(&ack, 0, sizeof(ack));
    ack.base.low = 2;
    ack.num_bits = 2;
    ack.bitmap[0] = 0x80000000u;
    memset(&state, 0, sizeof(state));
    CHECK(nb_rtps_reliable_process_acknack(writer, &ack, on_resend, &state, &count) == NB_OK);
    CHECK(count == 1 && state.count == 1u && state.sn[0] == 2);
    CHECK(strcmp(state.payload[0], "b") == 0);

    ack.bitmap[0] = 0x40000000u;
    memset(&state, 0, sizeof(state));
    CHECK(nb_rtps_reliable_process_acknack(writer, &ack, on_resend, &state, &count) == NB_OK);
    CHECK(count == 1 && state.count == 1u && state.sn[0] == 3);
    CHECK(strcmp(state.payload[0], "c") == 0);

    ack.bitmap[0] = 0xc0000000u;
    memset(&state, 0, sizeof(state));
    CHECK(nb_rtps_reliable_process_acknack(writer, &ack, on_resend, &state, &count) == NB_OK);
    CHECK(count == 2 && state.count == 2u);
    CHECK(state.sn[0] == 2 && state.sn[1] == 3);

    ack.base.low = 4;
    ack.num_bits = 0;
    memset(&state, 0, sizeof(state));
    CHECK(nb_rtps_reliable_process_acknack(writer, &ack, on_resend, &state, &count) == NB_OK);
    CHECK(count == 0 && state.count == 0);

    CHECK(nb_rtps_reliable_add(writer, NULL, 0, 1, &sn) == NB_OK && sn == 4);
    nb_rtps_reliable_state(writer, &first, &last, &count);
    CHECK(first == 3 && last == 4 && count == 2);
    CHECK(nb_rtps_reliable_get(writer, 4, &payload, &len, &dispose) == NB_OK);
    CHECK(len == 0 && dispose == 1);

    nb_rtps_reliable_writer_destroy(writer);
}

/*
 * The history is bounded by bytes as well as by sample count, so a deep
 * configuration cannot pin an unbounded amount of memory.
 */
static void test_history_byte_budget(void)
{
    enum { SAMPLE_SIZE = 100 * 1024, SAMPLE_COUNT = 4 };
    static uint8_t sample[SAMPLE_SIZE];
    nb_rtps_reliable_writer_t *writer = nb_rtps_reliable_writer_create(64);
    uint32_t first = 0;
    uint32_t last = 0;
    size_t count = 0;
    size_t i;

    CHECK(writer != NULL);
    if (!writer) {
        return;
    }
    memset(sample, 0x5a, sizeof(sample));
    for (i = 0; i < SAMPLE_COUNT; i++) {
        CHECK(nb_rtps_reliable_add(writer, sample, sizeof(sample), 0, NULL) == NB_OK);
    }
    nb_rtps_reliable_state(writer, &first, &last, &count);
    /* The 256 KiB budget holds only two of the 100 KiB samples. */
    CHECK(count == 2);
    CHECK(first == (uint32_t)(SAMPLE_COUNT - 1));
    CHECK(last == (uint32_t)SAMPLE_COUNT);
    nb_rtps_reliable_writer_destroy(writer);
}

static void test_reader_window(void)
{
    nb_rtps_reader_state_t state;
    nb_rtps_acknack_t ack;
    nb_rtps_gap_t gap;
    uint32_t missing;

    nb_rtps_reader_state_reset(&state, 1);
    CHECK(state.next_expected == 1);

    /* Out-of-order sample: tracked but the window does not advance yet. */
    CHECK(nb_rtps_reader_on_sample(&state, 2) == 1);
    CHECK(state.next_expected == 1);
    /* The duplicate must not be delivered twice. */
    CHECK(nb_rtps_reader_on_sample(&state, 2) == 0);
    /* Filling the hole advances the window over both samples. */
    CHECK(nb_rtps_reader_on_sample(&state, 1) == 1);
    CHECK(state.next_expected == 3);
    CHECK(nb_rtps_reader_on_sample(&state, 1) == 0);

    /* Nothing missing: the ACKNACK has an empty bitmap. */
    missing = nb_rtps_reader_build_acknack(&state, 2, &ack);
    CHECK(missing == 0);
    CHECK(ack.base.low == 3);
    CHECK(ack.num_bits == 0);
    CHECK(ack.count == 1);

    /* 3..6 are outstanding; mark 4 and 6 through two gaps, then 3 and 5
     * arrive. The window must advance to 7. */
    memset(&gap, 0, sizeof(gap));
    gap.gap_start.low = 3;
    nb_rtps_reader_on_gap(&state, &gap);
    CHECK(state.next_expected == 4);
    CHECK(nb_rtps_reader_on_sample(&state, 4) == 1);
    CHECK(state.next_expected == 5);
    gap.gap_start.low = 5;
    nb_rtps_reader_on_gap(&state, &gap);
    CHECK(nb_rtps_reader_on_sample(&state, 6) == 1);
    CHECK(state.next_expected == 7);

    /* Sequence-number sets use MSB-first bits. */
    CHECK(nb_rtps_reader_on_sample(&state, 9) == 1);
    missing = nb_rtps_reader_build_acknack(&state, 9, &ack);
    CHECK(missing == 2);
    CHECK(ack.base.low == 7);
    CHECK(ack.num_bits == 3);
    CHECK(ack.bitmap[0] == 0xc0000000u);
    CHECK(ack.count == 2);

    /* A gap announced through the set form advances over members only. */
    memset(&gap, 0, sizeof(gap));
    gap.gap_start.low = 7;
    gap.base.low = 8;
    gap.num_bits = 1;
    gap.bitmap[0] = 0x80000000u; /* gap 8 */
    nb_rtps_reader_on_gap(&state, &gap);
    /* Sample 9 was already received, so closing 7 and 8 advances past it. */
    CHECK(state.next_expected == 10);

    /* Samples beyond the 256-entry window are delivered but not tracked, so a
     * later ACKNACK still covers them. */
    CHECK(nb_rtps_reader_on_sample(&state, 10 + NB_RTPS_ACKNACK_MAX_BITS + 1u) == 1);
    CHECK(state.next_expected == 10);
}

int main(void)
{
    test_history_and_acknack();
    test_history_byte_budget();
    test_reader_window();
    if (g_fail) {
        nb_log_error("rtps reliable tests failed");
        return 1;
    }
    printf("rtps reliable tests passed\n");
    return 0;
}
