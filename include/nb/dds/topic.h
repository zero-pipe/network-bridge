#ifndef NB_DDS_TOPIC_H
#define NB_DDS_TOPIC_H

/*
 * DDS spine: a topic is name + type name + QoS, the unit of matching.
 *
 * The application name is combined with the participant prefix for the wire
 * name (`prefix/name`). type_name is advisory: samples are opaque bytes and
 * both backends put the `dds::Message` wire type on the network so RTPS and
 * Cyclone can interoperate.
 */

#include <nb/dds/participant.h>
#include <nb/dds/qos.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NB_DDS_TOPIC_NAME_MAX 128
#define NB_DDS_TOPIC_TYPE_MAX 128

int nb_dds_topic_create(nb_dds_participant_t *participant,
                        const char *name,
                        const char *type_name,
                        const nb_dds_qos_t *qos,
                        nb_dds_topic_t **out);

/*
 * Create a topic that also announces XTypes TypeInformation.
 *
 * type_information is an opaque XCDR2 blob generated from the type's IDL by a
 * compatible tool (Cyclone DDS idlc, Fast DDS fastddsgen, ...); the bytes are
 * carried verbatim in SEDP (PID_TYPE_INFORMATION) so a peer with strict type
 * consistency can match on the type's equivalence hash. The blob is borrowed
 * and must outlive the topic (a static const array from the generator is
 * typical). type_name typically matches the IDL type the blob was generated
 * from. Passing NULL/0 is equivalent to nb_dds_topic_create (name-only match).
 */
int nb_dds_topic_create_with_type_info(nb_dds_participant_t *participant,
                                       const char *name,
                                       const char *type_name,
                                       const void *type_information,
                                       size_t type_information_len,
                                       const nb_dds_qos_t *qos,
                                       nb_dds_topic_t **out);
void nb_dds_topic_destroy(nb_dds_topic_t *topic);
const char *nb_dds_topic_name(const nb_dds_topic_t *topic);
const char *nb_dds_topic_type_name(const nb_dds_topic_t *topic);
const nb_dds_qos_t *nb_dds_topic_qos(const nb_dds_topic_t *topic);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_TOPIC_H */
