#ifndef RTPS_WAITSET_H
#define RTPS_WAITSET_H

/*
 * Wait set: block until one of the attached readers has a sample matching its
 * filter. Attaching a reader enables its history if it had none.
 *
 * rtps_waitset_wait() returns the number of ready readers written to out, 0 on
 * timeout, or a negative rtps_error. It must be called from the application
 * thread, never from a data callback.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_reader.h"
#include "rtps_types.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtps_participant rtps_participant_t;
typedef struct rtps_waitset rtps_waitset_t;

/* Block forever. */
#define RTPS_WAITSET_INFINITE (-1)

/* A wait set belongs to one participant; attach only its readers. */
RTPS_API int rtps_waitset_create(rtps_participant_t *participant,
                                 rtps_waitset_t **out);
RTPS_API void rtps_waitset_destroy(rtps_waitset_t *waitset);

RTPS_API int rtps_waitset_attach(rtps_waitset_t *waitset,
                                 rtps_reader_t *reader,
                                 const rtps_sample_filter_t *filter);
RTPS_API int rtps_waitset_detach(rtps_waitset_t *waitset, rtps_reader_t *reader);

/*
 * Wait until at least one attached reader matches, the timeout expires, or a
 * wake-up is posted. timeout_ms < 0 waits forever.
 */
RTPS_API int rtps_waitset_wait(rtps_waitset_t *waitset,
                               rtps_reader_t **out,
                               size_t max,
                               int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_WAITSET_H */
