#ifndef RTPS_READER_H
#define RTPS_READER_H

/*
 * DataReader: receives samples on one topic.
 *
 * Two delivery modes can be used together:
 *
 *   callback   the data callback runs on the participant thread with a
 *              zero-copy view valid only for the call;
 *   history    rtps_reader_enable_history() keeps a bounded KEEP_LAST queue
 *              that read()/take()/wait sets consume.
 *
 * read() returns views owned by the history (valid until the next take,
 * destroy, or the sample is evicted); take() returns owned copies that the
 * caller returns with rtps_sample_release(). A callback-only reader has no
 * history and read()/take() report zero samples.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_qos.h"
#include "rtps_topic.h"
#include "rtps_types.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtps_reader rtps_reader_t;
typedef struct rtps_subscriber rtps_subscriber_t;

/*
 * samples points at `count` entries; data is valid only during the call. The
 * callback runs on the participant thread and must not call back into the
 * participant.
 */
typedef void (*rtps_data_callback_t)(void *user,
                                     rtps_reader_t *reader,
                                     const rtps_sample_t *samples,
                                     size_t count);

RTPS_API int rtps_reader_create(rtps_topic_t *topic,
                                const rtps_qos_t *qos,
                                rtps_reader_t **out);

/* Create the reader under a subscriber. Subscriber policies are inherited
 * before the topic and the reader's own values. */
RTPS_API int rtps_reader_create_on(rtps_subscriber_t *subscriber,
                                   rtps_topic_t *topic,
                                   const rtps_qos_t *qos,
                                   rtps_reader_t **out);
RTPS_API int rtps_reader_destroy(rtps_reader_t *reader);

RTPS_API int rtps_reader_set_data_callback(rtps_reader_t *reader,
                                           rtps_data_callback_t callback,
                                           void *user);

/*
 * Enable (or resize) the reader history. depth 0 keeps the current depth; a
 * reader created without a history starts with depth 1 when this is called.
 */
RTPS_API int rtps_reader_enable_history(rtps_reader_t *reader, uint32_t depth);

/* Number of samples currently in the history. */
RTPS_API size_t rtps_reader_size(const rtps_reader_t *reader);

/*
 * Copy matching samples into out without removing them and mark them READ.
 * filter may be NULL. Returns the number written, or a negative rtps_error.
 * The data pointers are owned by the reader.
 */
RTPS_API size_t rtps_reader_read(rtps_reader_t *reader,
                                 const rtps_sample_filter_t *filter,
                                 rtps_sample_t *out,
                                 size_t max);

/* Like read(), but removes the samples; release them with rtps_sample_release. */
RTPS_API size_t rtps_reader_take(rtps_reader_t *reader,
                                 const rtps_sample_filter_t *filter,
                                 rtps_sample_t *out,
                                 size_t max);

/* Release samples returned by rtps_reader_take(). */
RTPS_API void rtps_sample_release(rtps_sample_t *samples, size_t count);

/*
 * Loan path, for DDS-style code: identical to read() (borrowed views into the
 * history, no copy), with a matching return call that does nothing except make
 * the intent explicit. The views stay valid until the sample is taken, evicted
 * or the reader is destroyed.
 */
RTPS_API size_t rtps_reader_loan(rtps_reader_t *reader,
                                 const rtps_sample_filter_t *filter,
                                 rtps_sample_t *out,
                                 size_t max);
RTPS_API void rtps_reader_return_loan(rtps_reader_t *reader,
                                      rtps_sample_t *samples,
                                      size_t count);

/* Number of matching samples without consuming or marking them. */
RTPS_API size_t rtps_reader_match_count(rtps_reader_t *reader,
                                        const rtps_sample_filter_t *filter);

RTPS_API int rtps_reader_get_guid(const rtps_reader_t *reader, rtps_guid_t *out);
RTPS_API int rtps_reader_get_qos(const rtps_reader_t *reader, rtps_qos_t *out);
RTPS_API size_t rtps_reader_matched_count(const rtps_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_READER_H */
