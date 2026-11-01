#ifndef RTPS_WRITER_H
#define RTPS_WRITER_H

/*
 * DataWriter: publishes samples on one topic. Created from a topic, so the
 * topic/type pair is fixed for its lifetime.
 *
 * Payloads are opaque CDR. rtps_writer_write() takes the CDR body and adds the
 * standard 4-octet CDR_LE encapsulation header; rtps_writer_write_serialized()
 * takes bytes that already include it. Callbacks deliver the body without the
 * header; because RTPS pads submessages to 4 octets, a received body may carry
 * up to three trailing padding octets that a decoder must ignore.
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

typedef struct rtps_writer rtps_writer_t;
typedef struct rtps_publisher rtps_publisher_t;

/* One payload for rtps_writer_write_batch(). */
typedef struct rtps_payload {
    const void *data;
    size_t size;
} rtps_payload_t;

/*
 * Create a writer on the topic. qos may be NULL; policies it does not set are
 * inherited from the topic. The writer is announced through SEDP immediately
 * (or at participant start if it does not run yet).
 */
RTPS_API int rtps_writer_create(rtps_topic_t *topic,
                                const rtps_qos_t *qos,
                                rtps_writer_t **out);

/* Create the writer under a publisher. Publisher policies are inherited before
 * the topic and the writer's own values. */
RTPS_API int rtps_writer_create_on(rtps_publisher_t *publisher,
                                   rtps_topic_t *topic,
                                   const rtps_qos_t *qos,
                                   rtps_writer_t **out);
RTPS_API int rtps_writer_destroy(rtps_writer_t *writer);

/* Publish a CDR body (without encapsulation header). */
RTPS_API int rtps_writer_write(rtps_writer_t *writer, const void *data, size_t size);
/* Publish a payload that already starts with the 4-octet encapsulation header. */
RTPS_API int rtps_writer_write_serialized(rtps_writer_t *writer,
                                          const void *payload,
                                          size_t size);

/* Write several bodies in order; stops at the first error. */
RTPS_API int rtps_writer_write_batch(rtps_writer_t *writer,
                                     const rtps_payload_t *payloads,
                                     size_t count);

/*
 * Publish a keyed sample or a state change. state is ALIVE (data with the
 * instance handle), NOT_ALIVE_DISPOSED or NOT_ALIVE_NO_WRITERS. data may be
 * NULL for a state change: the engine sends an empty key-only sample and the
 * handle travels in the inline QoS.
 */
RTPS_API int rtps_writer_write_instance(rtps_writer_t *writer,
                                        const rtps_instance_handle_t *handle,
                                        rtps_instance_state_t state,
                                        const void *data,
                                        size_t size);

/* The assigned entity GUID. */
RTPS_API int rtps_writer_get_guid(const rtps_writer_t *writer, rtps_guid_t *out);
/* Effective QoS: topic values overridden by the writer's explicit policies. */
RTPS_API int rtps_writer_get_qos(const rtps_writer_t *writer, rtps_qos_t *out);
/* Number of remote readers currently matched. */
RTPS_API size_t rtps_writer_matched_count(const rtps_writer_t *writer);

#ifdef __cplusplus
}
#endif

#endif /* RTPS_WRITER_H */
