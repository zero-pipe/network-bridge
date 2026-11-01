#ifndef RTPS_SUBSCRIBER_H
#define RTPS_SUBSCRIBER_H

/*
 * Subscriber: an optional grouping entity between a participant and its
 * readers, mirroring the DDS Subscriber. It carries a QoS template (typically
 * partition and presentation) that its readers inherit.
 *
 * Subscribers are a DCPS concept: RTPS has no subscriber entity and nothing is
 * announced for it. Destroying a subscriber destroys the readers created under
 * it (the DDS parent/child rule). A reader created with rtps_reader_create()
 * does not belong to a subscriber; the two forms can be mixed.
 */

#include <stddef.h>

#include "rtps_qos.h"
#include "rtps_topic.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtps_participant rtps_participant_t;
typedef struct rtps_subscriber rtps_subscriber_t;

RTPS_API int rtps_subscriber_create(rtps_participant_t *participant,
                                    const rtps_qos_t *qos,
                                    rtps_subscriber_t **out);
RTPS_API int rtps_subscriber_destroy(rtps_subscriber_t *subscriber);
RTPS_API int rtps_subscriber_get_qos(const rtps_subscriber_t *subscriber,
                                     rtps_qos_t *out);
RTPS_API size_t rtps_subscriber_reader_count(const rtps_subscriber_t *subscriber);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_SUBSCRIBER_H */
