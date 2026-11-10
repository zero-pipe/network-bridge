#ifndef NB_DDS_WRITER_H
#define NB_DDS_WRITER_H

/*
 * DDS spine: writer (publication).
 *
 * write() borrows the payload for the call, write_owned() takes ownership of
 * an nb_owned_payload_t and releases it. Both publish the application bytes
 * verbatim: the DDS spine no longer wraps samples in the RPC envelope.
 */

#include <nb/dds/topic.h>

#ifdef __cplusplus
extern "C" {
#endif

int nb_dds_writer_create(nb_dds_topic_t *topic,
                         const nb_dds_qos_t *qos,
                         nb_dds_writer_t **out);
void nb_dds_writer_destroy(nb_dds_writer_t *writer);

int nb_dds_writer_write(nb_dds_writer_t *writer, const void *data, size_t size);
int nb_dds_writer_write_payload(nb_dds_writer_t *writer, const nb_payload_t *payload);
int nb_dds_writer_write_owned(nb_dds_writer_t *writer, nb_owned_payload_t *payload);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_WRITER_H */
