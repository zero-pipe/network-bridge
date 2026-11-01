#include "rtps_reliable.h"

#include <stdlib.h>
#include <string.h>

#include <ntk.h>

typedef struct nb_rtps_reliable_sample {
    uint32_t sn;
    uint8_t *payload;
    size_t len;
    int dispose;
    uint64_t written_ms;
    /* Source timestamp published with the sample, so a retransmission keeps
     * the time the data was written instead of the time it was resent. */
    uint64_t source_ns;
    /* Keyed samples keep the instance handle and the status bits, so a
     * retransmission says the same thing the first transmission said. */
    uint32_t status;
    int has_handle;
    uint8_t handle[NB_INSTANCE_HANDLE_SIZE];
} nb_rtps_reliable_sample_t;

struct nb_rtps_reliable_writer {
    ntk_mutex *lock;
    nb_rtps_reliable_sample_t *samples;
    size_t capacity;
    size_t count;
    size_t head;
    size_t bytes;
    uint32_t next_sn;
};

/* Drop the oldest sample. The caller holds the writer lock. */
static void drop_oldest(nb_rtps_reliable_writer_t *writer)
{
    nb_rtps_reliable_sample_t *sample = &writer->samples[writer->head];

    writer->bytes -= sample->len;
    free(sample->payload);
    sample->payload = NULL;
    sample->len = 0;
    writer->head = (writer->head + 1u) % writer->capacity;
    writer->count--;
}

static nb_rtps_reliable_sample_t *find_sample(nb_rtps_reliable_writer_t *writer, uint32_t sn)
{
    size_t i;

    for (i = 0; i < writer->count; i++) {
        nb_rtps_reliable_sample_t *sample =
            &writer->samples[(writer->head + i) % writer->capacity];
        if (sample->sn == sn) {
            return sample;
        }
    }
    return NULL;
}

nb_rtps_reliable_writer_t *nb_rtps_reliable_writer_create(size_t capacity)
{
    nb_rtps_reliable_writer_t *writer;

    if (capacity == 0) {
        capacity = NB_RTPS_RELIABLE_DEFAULT_CAPACITY;
    }
    if (capacity > NB_RTPS_RELIABLE_MAX_CAPACITY) {
        capacity = NB_RTPS_RELIABLE_MAX_CAPACITY;
    }
    writer = (nb_rtps_reliable_writer_t *)calloc(1, sizeof(*writer));
    if (!writer) {
        return NULL;
    }
    writer->samples =
        (nb_rtps_reliable_sample_t *)calloc(capacity, sizeof(*writer->samples));
    writer->lock = ntk_mutex_create(NTK_MUTEX_NORMAL);
    if (!writer->samples || !writer->lock) {
        if (writer->lock) {
            ntk_mutex_destroy(writer->lock);
        }
        free(writer->samples);
        free(writer);
        return NULL;
    }
    writer->capacity = capacity;
    return writer;
}

void nb_rtps_reliable_writer_destroy(nb_rtps_reliable_writer_t *writer)
{
    size_t i;

    if (!writer) {
        return;
    }
    for (i = 0; i < writer->capacity; i++) {
        free(writer->samples[i].payload);
    }
    if (writer->lock) {
        ntk_mutex_destroy(writer->lock);
    }
    free(writer->samples);
    free(writer);
}

int nb_rtps_reliable_add(nb_rtps_reliable_writer_t *writer,
                         const void *payload,
                         size_t len,
                         int dispose,
                         uint32_t *out_sn)
{
    return nb_rtps_reliable_add_at(writer, payload, len, dispose, 0, out_sn);
}

int nb_rtps_reliable_add_at(nb_rtps_reliable_writer_t *writer,
                            const void *payload,
                            size_t len,
                            int dispose,
                            uint64_t source_ns,
                            uint32_t *out_sn)
{
    return nb_rtps_reliable_add_instance(
        writer, payload, len, dispose, source_ns, NULL, 0, out_sn);
}

int nb_rtps_reliable_add_instance(nb_rtps_reliable_writer_t *writer,
                                  const void *payload,
                                  size_t len,
                                  int dispose,
                                  uint64_t source_ns,
                                  const uint8_t *handle,
                                  uint32_t status,
                                  uint32_t *out_sn)
{
    nb_rtps_reliable_sample_t *sample;
    uint8_t *copy = NULL;
    size_t index;

    if (!writer || (len > 0 && !payload)) {
        return NB_ERR_INVALID;
    }
    if (len > 0) {
        copy = (uint8_t *)malloc(len);
        if (!copy) {
            return NB_ERR_NOMEM;
        }
        memcpy(copy, payload, len);
    }

    ntk_mutex_lock(writer->lock);
    while (writer->count > 0 &&
           (writer->count >= writer->capacity ||
            writer->bytes + len > NB_RTPS_RELIABLE_MAX_BYTES)) {
        drop_oldest(writer);
    }
    index = (writer->head + writer->count) % writer->capacity;
    writer->count++;
    writer->bytes += len;
    sample = &writer->samples[index];
    sample->sn = ++writer->next_sn;
    sample->payload = copy;
    sample->len = len;
    sample->dispose = dispose ? 1 : 0;
    sample->status = status;
    sample->has_handle = handle != NULL;
    if (handle) {
        memcpy(sample->handle, handle, NB_INSTANCE_HANDLE_SIZE);
    }
    sample->written_ms = ntk_wall_ms();
    sample->source_ns = source_ns ? source_ns : sample->written_ms * 1000000ull;
    if (out_sn) {
        *out_sn = sample->sn;
    }
    ntk_mutex_unlock(writer->lock);
    return NB_OK;
}

int nb_rtps_reliable_get(const nb_rtps_reliable_writer_t *writer,
                         uint32_t sn,
                         const uint8_t **payload,
                         size_t *len,
                         int *dispose)
{
    return nb_rtps_reliable_get_instance(
        writer, sn, payload, len, dispose, NULL, NULL, NULL);
}

int nb_rtps_reliable_get_instance(const nb_rtps_reliable_writer_t *writer,
                                  uint32_t sn,
                                  const uint8_t **payload,
                                  size_t *len,
                                  int *dispose,
                                  uint8_t handle[NB_INSTANCE_HANDLE_SIZE],
                                  int *has_handle,
                                  uint32_t *status)
{
    nb_rtps_reliable_sample_t *sample;

    if (!writer) {
        return NB_ERR_INVALID;
    }
    ntk_mutex_lock(writer->lock);
    sample = find_sample((nb_rtps_reliable_writer_t *)writer, sn);
    if (!sample) {
        ntk_mutex_unlock(writer->lock);
        return NB_ERR_NOTFOUND;
    }
    if (payload) {
        *payload = sample->payload;
    }
    if (len) {
        *len = sample->len;
    }
    if (dispose) {
        *dispose = sample->dispose;
    }
    if (handle && sample->has_handle) {
        memcpy(handle, sample->handle, NB_INSTANCE_HANDLE_SIZE);
    }
    if (has_handle) {
        *has_handle = sample->has_handle;
    }
    if (status) {
        *status = sample->status;
    }
    ntk_mutex_unlock(writer->lock);
    return NB_OK;
}

int nb_rtps_reliable_sample_ms(const nb_rtps_reliable_writer_t *writer,
                               uint32_t sn,
                               uint64_t *out_ms)
{
    nb_rtps_reliable_sample_t *sample;

    if (!writer || !out_ms) {
        return NB_ERR_INVALID;
    }
    ntk_mutex_lock(writer->lock);
    sample = find_sample((nb_rtps_reliable_writer_t *)writer, sn);
    if (!sample) {
        ntk_mutex_unlock(writer->lock);
        return NB_ERR_NOTFOUND;
    }
    *out_ms = sample->written_ms;
    ntk_mutex_unlock(writer->lock);
    return NB_OK;
}

int nb_rtps_reliable_source_ns(const nb_rtps_reliable_writer_t *writer,
                               uint32_t sn,
                               uint64_t *out_ns)
{
    nb_rtps_reliable_sample_t *sample;

    if (!writer || !out_ns) {
        return NB_ERR_INVALID;
    }
    ntk_mutex_lock(writer->lock);
    sample = find_sample((nb_rtps_reliable_writer_t *)writer, sn);
    if (!sample) {
        ntk_mutex_unlock(writer->lock);
        return NB_ERR_NOTFOUND;
    }
    *out_ns = sample->source_ns;
    ntk_mutex_unlock(writer->lock);
    return NB_OK;
}

void nb_rtps_reliable_state(const nb_rtps_reliable_writer_t *writer,
                            uint32_t *first_sn,
                            uint32_t *last_sn,
                            size_t *sample_count)
{
    uint32_t first;
    uint32_t last;
    size_t count;

    if (!writer) {
        return;
    }
    ntk_mutex_lock(writer->lock);
    count = writer->count;
    if (count == 0) {
        first = writer->next_sn + 1u;
        last = writer->next_sn;
    } else {
        first = writer->samples[writer->head].sn;
        last = writer->samples[(writer->head + count - 1) % writer->capacity].sn;
    }
    ntk_mutex_unlock(writer->lock);

    if (first_sn) {
        *first_sn = first;
    }
    if (last_sn) {
        *last_sn = last;
    }
    if (sample_count) {
        *sample_count = count;
    }
}

int nb_rtps_reliable_process_acknack(nb_rtps_reliable_writer_t *writer,
                                     const nb_rtps_acknack_t *acknack,
                                     nb_rtps_reliable_resend_fn resend,
                                     void *user,
                                     size_t *resent)
{
    uint32_t i;
    size_t sent = 0;
    int rc = NB_OK;

    if (!writer || !acknack || !resend) {
        return NB_ERR_INVALID;
    }
    if (acknack->num_bits > NB_RTPS_ACKNACK_MAX_BITS ||
        acknack->base.high != 0) {
        return NB_ERR_NOT_SUPPORTED;
    }

    ntk_mutex_lock(writer->lock);
    for (i = 0; i < acknack->num_bits; i++) {
        nb_rtps_reliable_sample_t *sample;
        uint32_t sn;

        if ((acknack->bitmap[i / 32u] & (1u << (31u - (i % 32u)))) == 0) {
            continue;
        }
        sn = acknack->base.low + i;
        sample = find_sample(writer, sn);
        if (!sample) {
            continue;
        }
        rc = resend(user, sample->sn, sample->payload, sample->len, sample->dispose);
        if (rc != NB_OK) {
            break;
        }
        sent++;
    }
    ntk_mutex_unlock(writer->lock);
    if (resent) {
        *resent = sent;
    }
    return rc;
}

void nb_rtps_reader_state_reset(nb_rtps_reader_state_t *state, uint32_t first_sn)
{
    if (!state) {
        return;
    }
    state->next_expected = first_sn ? first_sn : 1u;
    memset(state->received, 0, sizeof(state->received));
    state->ack_count = 0;
}

static void reader_mark_received(nb_rtps_reader_state_t *state, uint32_t sn)
{
    uint32_t offset;
    uint32_t i;

    if (sn < state->next_expected) {
        return;
    }
    offset = sn - state->next_expected;
    if (offset >= NB_RTPS_ACKNACK_MAX_BITS) {
        return;
    }
    state->received[offset / 32u] |= 1u << (offset % 32u);
    while ((state->received[0] & 1u) != 0) {
        for (i = 0; i + 1u < NB_RTPS_ACKNACK_MAX_WORDS; i++) {
            state->received[i] =
                (state->received[i] >> 1) | (state->received[i + 1u] << 31);
        }
        state->received[NB_RTPS_ACKNACK_MAX_WORDS - 1u] >>= 1;
        state->next_expected++;
    }
}

int nb_rtps_reader_on_sample(nb_rtps_reader_state_t *state, uint32_t sn)
{
    uint32_t offset;

    if (!state) {
        return 0;
    }
    if (sn < state->next_expected) {
        return 0;
    }
    offset = sn - state->next_expected;
    if (offset >= NB_RTPS_ACKNACK_MAX_BITS) {
        /* Too far ahead to track: deliver, but ask again on the next ACKNACK. */
        return 1;
    }
    if ((state->received[offset / 32u] & (1u << (offset % 32u))) != 0) {
        return 0;
    }
    reader_mark_received(state, sn);
    return 1;
}

void nb_rtps_reader_on_gap(nb_rtps_reader_state_t *state, const nb_rtps_gap_t *gap)
{
    uint32_t i;

    if (!state || !gap) {
        return;
    }
    if (gap->gap_start.high != 0 || gap->base.high != 0) {
        return;
    }
    /* [gapStart, bitmapBase) is unavailable, as are the set bitmap bits
     * (RTPS 2.5 8.3.7.4). A writer that withdrew a contiguous range sends an
     * empty bitmap, which is the common case for our own writers. */
    if (gap->base.low > gap->gap_start.low) {
        uint32_t end = gap->base.low;
        uint32_t sn;

        if (end - gap->gap_start.low > NB_RTPS_ACKNACK_MAX_BITS) {
            end = gap->gap_start.low + NB_RTPS_ACKNACK_MAX_BITS;
        }
        for (sn = gap->gap_start.low; sn < end; sn++) {
            reader_mark_received(state, sn);
        }
    } else {
        reader_mark_received(state, gap->gap_start.low);
    }
    for (i = 0; i < gap->num_bits; i++) {
        if ((gap->bitmap[i / 32u] & (1u << (31u - (i % 32u)))) == 0) {
            continue;
        }
        reader_mark_received(state, gap->base.low + i);
    }
}

int nb_rtps_reader_has_sample(const nb_rtps_reader_state_t *state, uint32_t sn)
{
    uint32_t offset;

    if (!state || sn == 0) {
        return 0;
    }
    if (sn < state->next_expected) {
        return 1;
    }
    offset = sn - state->next_expected;
    if (offset >= NB_RTPS_ACKNACK_MAX_BITS) {
        return 0;
    }
    return (state->received[offset / 32u] & (1u << (offset % 32u))) != 0;
}

uint32_t nb_rtps_reader_build_acknack(nb_rtps_reader_state_t *state,
                                      uint32_t last_sn,
                                      nb_rtps_acknack_t *acknack)
{
    uint32_t missing = 0;
    uint32_t base;
    uint32_t num_bits = 0;
    uint32_t i;

    if (!state || !acknack) {
        return 0;
    }
    memset(acknack, 0, sizeof(*acknack));
    base = state->next_expected;
    acknack->base.high = 0;
    acknack->base.low = base;
    if (last_sn >= base) {
        uint64_t span = (uint64_t)last_sn - (uint64_t)base + 1u;

        num_bits = span > NB_RTPS_ACKNACK_MAX_BITS
                       ? NB_RTPS_ACKNACK_MAX_BITS
                       : (uint32_t)span;
        for (i = 0; i < num_bits; i++) {
            if ((state->received[i / 32u] & (1u << (i % 32u))) != 0) {
                continue;
            }
            acknack->bitmap[i / 32u] |= 1u << (31u - (i % 32u));
            missing++;
        }
    }
    acknack->num_bits = num_bits;
    acknack->count = ++state->ack_count;
    return missing;
}
