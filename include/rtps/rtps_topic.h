#ifndef RTPS_TOPIC_H
#define RTPS_TOPIC_H

/*
 * Topic: a name plus a type name plus a QoS template, owned by a participant.
 *
 * Topics are a DCPS concept: RTPS itself has no topic entity and nothing about
 * a topic is announced on the wire. Writers and readers are created from a
 * topic, and their effective QoS is resolved as endpoint > topic > DDS default.
 * Within one participant a topic name maps to exactly one type; creating the
 * same name and type again returns the same handle (no extra reference count).
 *
 * Destroying a topic destroys the writers and readers created from it (the DDS
 * parent/child rule); destroying the participant destroys every topic.
 */

#include <stddef.h>
#include <stdint.h>

#include "rtps_qos.h"
#include "rtps_version.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTPS_TOPIC_NAME_MAX 128u
#define RTPS_TOPIC_TYPE_MAX 128u

typedef struct rtps_participant rtps_participant_t;
typedef struct rtps_topic rtps_topic_t;

typedef struct rtps_topic_desc {
    uint32_t struct_size;
    const char *name;
    const char *type_name;
    /* Topic-level defaults for the endpoints created from it. */
    rtps_qos_t qos;
} rtps_topic_desc_t;

RTPS_API void rtps_topic_desc_init(rtps_topic_desc_t *desc);

/*
 * Create or look up a topic. On success *out receives a handle; the same
 * (name, type) pair twice returns the same handle. A name already used with a
 * different type returns RTPS_ERR_PRECONDITION.
 */
RTPS_API int rtps_topic_create(rtps_participant_t *participant,
                               const rtps_topic_desc_t *desc,
                               rtps_topic_t **out);

/* Destroy a topic and every writer/reader created from it. */
RTPS_API int rtps_topic_destroy(rtps_topic_t *topic);

/* Look up an existing topic by name; *out is NULL when there is none. */
RTPS_API int rtps_topic_find(rtps_participant_t *participant,
                             const char *name,
                             rtps_topic_t **out);

RTPS_API const char *rtps_topic_name(const rtps_topic_t *topic);
RTPS_API const char *rtps_topic_type_name(const rtps_topic_t *topic);
/* Topic-level QoS as passed at creation. */
RTPS_API int rtps_topic_get_qos(const rtps_topic_t *topic, rtps_qos_t *out);
/* Current number of writers and readers created from this topic. */
RTPS_API int rtps_topic_endpoint_count(const rtps_topic_t *topic, size_t *writers, size_t *readers);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_TOPIC_H */
