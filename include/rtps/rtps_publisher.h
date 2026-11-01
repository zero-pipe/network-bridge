#ifndef RTPS_PUBLISHER_H
#define RTPS_PUBLISHER_H

/*
 * Publisher: an optional grouping entity between a participant and its
 * writers, mirroring the DDS Publisher. It carries a QoS template (typically
 * partition and presentation) that its writers inherit.
 *
 * Publishers are a DCPS concept: RTPS has no publisher entity and nothing is
 * announced for it. Destroying a publisher destroys the writers created under
 * it (the DDS parent/child rule). A writer created with rtps_writer_create()
 * does not belong to a publisher; the two forms can be mixed.
 */

#include <stddef.h>

#include "rtps_qos.h"
#include "rtps_topic.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rtps_participant rtps_participant_t;
typedef struct rtps_publisher rtps_publisher_t;

/* qos may be NULL; policies are inherited by writers between the topic and
 * the writer's own values. */
RTPS_API int rtps_publisher_create(rtps_participant_t *participant,
                                   const rtps_qos_t *qos,
                                   rtps_publisher_t **out);
RTPS_API int rtps_publisher_destroy(rtps_publisher_t *publisher);
RTPS_API int rtps_publisher_get_qos(const rtps_publisher_t *publisher,
                                    rtps_qos_t *out);
RTPS_API size_t rtps_publisher_writer_count(const rtps_publisher_t *publisher);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_PUBLISHER_H */
