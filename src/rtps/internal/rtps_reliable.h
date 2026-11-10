#ifndef NB_RTPS_RELIABLE_H
#define NB_RTPS_RELIABLE_H

/*
 * Bounded reliable writer history for SEDP.
 *
 * The writer keeps the most recent samples and retransmits samples requested
 * by an ACKNACK. It has no per-reader state: ACKNACK retransmissions go back to
 * the sender address supplied by the caller.
 */

#include <stddef.h>
#include <stdint.h>

#include <nb/base/error.h>
#include <nb/base/sample_info.h>

#include "rtps_message.h"

#define NB_RTPS_RELIABLE_DEFAULT_CAPACITY 8u
#define NB_RTPS_RELIABLE_MAX_CAPACITY 4096u
/* Byte budget per writer history. Samples are dropped from the oldest end
 * when the budget would be exceeded, regardless of the sample capacity, so a
 * deep configuration cannot pin an unbounded amount of memory. */
#define NB_RTPS_RELIABLE_MAX_BYTES (256u * 1024u)

typedef struct nb_rtps_reliable_writer nb_rtps_reliable_writer_t;

/* Called while the writer lock is held. Return NB_OK to continue. */
typedef int (*nb_rtps_reliable_resend_fn)(void *user,
                                          uint64_t sn,
                                          const uint8_t *payload,
                                          size_t len,
                                          int dispose);

nb_rtps_reliable_writer_t *nb_rtps_reliable_writer_create(size_t capacity);
void nb_rtps_reliable_writer_destroy(nb_rtps_reliable_writer_t *writer);

/* Test / restart helper: start the next add at next_sn + 1. Internal. */
void nb_rtps_reliable_seed(nb_rtps_reliable_writer_t *writer, uint64_t next_sn);

/* Append a sample. The sequence number starts at 1. When the history is full,
 * or when adding the sample would exceed NB_RTPS_RELIABLE_MAX_BYTES, the
 * oldest samples are dropped. Callers detect the eviction by comparing the
 * first sequence number before and after the call and must announce the
 * dropped range with GAP. */
int nb_rtps_reliable_add(nb_rtps_reliable_writer_t *writer,
                         const void *payload,
                         size_t len,
                         int dispose,
                         uint64_t *out_sn);

/* Same, with the source timestamp to put on the wire. A retransmission must
 * carry the timestamp the sample was written with, otherwise a BY_SOURCE
 * timestamp reader would order the resend as if it were new data. Zero means
 * "use the current wall clock". */
int nb_rtps_reliable_add_at(nb_rtps_reliable_writer_t *writer,
                            const void *payload,
                            size_t len,
                            int dispose,
                            uint64_t source_ns,
                            uint64_t *out_sn);

/*
 * Same as nb_rtps_reliable_add_at, for a keyed sample: `handle` is the DDS
 * instance handle the sample belongs to and `status` the PID_STATUS_INFO bits
 * (0 live, 1 disposed, 2 unregistered). Both are kept so a retransmission
 * carries them again instead of looking like a fresh live sample.
 */
int nb_rtps_reliable_add_instance(nb_rtps_reliable_writer_t *writer,
                                  const void *payload,
                                  size_t len,
                                  int dispose,
                                  uint64_t source_ns,
                                  const uint8_t *handle,
                                  uint32_t status,
                                  uint64_t *out_sn);

/* Return a sample. The pointer is valid until the next add or destroy. */
int nb_rtps_reliable_get(const nb_rtps_reliable_writer_t *writer,
                         uint64_t sn,
                         const uint8_t **payload,
                         size_t *len,
                         int *dispose);

/* Instance metadata stored with a sample by nb_rtps_reliable_add_instance. */
int nb_rtps_reliable_get_instance(const nb_rtps_reliable_writer_t *writer,
                                  uint64_t sn,
                                  const uint8_t **payload,
                                  size_t *len,
                                  int *dispose,
                                  uint8_t handle[NB_INSTANCE_HANDLE_SIZE],
                                  int *has_handle,
                                  uint32_t *status);

/* Wall-clock time at which a sample was added, for lifespan enforcement. */
int nb_rtps_reliable_sample_ms(const nb_rtps_reliable_writer_t *writer,
                               uint64_t sn,
                               uint64_t *out_ms);

/* Source timestamp in nanoseconds, as recorded by nb_rtps_reliable_add_at. */
int nb_rtps_reliable_source_ns(const nb_rtps_reliable_writer_t *writer,
                               uint64_t sn,
                               uint64_t *out_ns);

/* first_sn > last_sn when the history is empty. */
void nb_rtps_reliable_state(const nb_rtps_reliable_writer_t *writer,
                            uint64_t *first_sn,
                            uint64_t *last_sn,
                            size_t *sample_count);

/* Retransmit each sequence number whose bitmap bit is set. Sequence numbers
 * that have already been evicted are ignored. */
int nb_rtps_reliable_process_acknack(nb_rtps_reliable_writer_t *writer,
                                     const nb_rtps_acknack_t *acknack,
                                     nb_rtps_reliable_resend_fn resend,
                                     void *user,
                                     size_t *resent);

/*
 * Reader-side receive window.
 *
 * Tracks the next expected sequence number plus a 256-sample bitmap of
 * samples already seen. The window advances over contiguous received
 * samples, so a HEARTBEAT can be answered with an ACKNACK that requests only
 * the samples that are still missing.
 */
typedef struct nb_rtps_reader_state {
    uint64_t next_expected;
    uint32_t received[NB_RTPS_ACKNACK_MAX_WORDS];
    uint32_t ack_count;
} nb_rtps_reader_state_t;

/* Restart the window at first_sn (normally 1). */
void nb_rtps_reader_state_reset(nb_rtps_reader_state_t *state, uint64_t first_sn);

/*
 * Record a received sample. Returns 1 when the caller should deliver it, and 0
 * for duplicates, samples older than the window, and samples too far ahead to
 * track. A sample too far ahead is reported as new but is not recorded; the
 * reader falls back to asking for it again through the next ACKNACK.
 */
int nb_rtps_reader_on_sample(nb_rtps_reader_state_t *state, uint64_t sn);

/* Mark the samples announced by a GAP as received without delivering them. */
void nb_rtps_reader_on_gap(nb_rtps_reader_state_t *state, const nb_rtps_gap_t *gap);

/* Whether sn has already been recorded, or lies below the window. Non-mutating;
 * used to observe that a sample was consumed without being delivered. */
int nb_rtps_reader_has_sample(const nb_rtps_reader_state_t *state, uint64_t sn);

/*
 * Fill base, bitmap and count for an ACKNACK covering up to last_sn. Bits are
 * set for samples that are still missing. Returns the number of missing
 * samples; a zero return still carries a valid, empty ACKNACK.
 */
uint32_t nb_rtps_reader_build_acknack(nb_rtps_reader_state_t *state,
                                      uint64_t last_sn,
                                      nb_rtps_acknack_t *acknack);

#endif /* NB_RTPS_RELIABLE_H */
