#ifndef RTPS_RTPS_H
#define RTPS_RTPS_H

/*
 * Public API of the in-tree RTPS/DDS stack.
 *
 * Layering:
 *
 *   application
 *     rtps_* API and entities            this interface
 *   engine
 *     SPDP/SEDP, reliability, fragments, QoS enforcement, TCP
 *   ntk
 *     threads, sockets, locks, time, atomics
 *
 * The API layer and the protocol engine are separate: nothing under src/rtps
 * is required to use this header, and the wire format is unaffected by it.
 *
 * Minimal use:
 *
 *   rtps_participant_config_t pc; rtps_participant_t *pp;
 *   rtps_topic_desc_t td; rtps_topic_t *topic;
 *   rtps_writer_t *w; rtps_reader_t *r;
 *
 *   rtps_participant_config_init(&pc);
 *   pc.domain_id = 0;
 *   rtps_participant_create(&pc, &pp);
 *   rtps_participant_start(pp);
 *
 *   rtps_topic_desc_init(&td);
 *   td.name = "example/hello"; td.type_name = "example::Hello";
 *   rtps_topic_create(pp, &td, &topic);
 *   rtps_writer_create(topic, NULL, &w);
 *   rtps_reader_create(topic, NULL, &r);
 *   rtps_reader_set_data_callback(r, on_data, NULL);
 *   rtps_writer_write(w, cdr_body, cdr_body_len);
 *
 * All handles are opaque and owned by their parent: destroying a participant
 * destroys its topics, writers and readers.
 */

#include "rtps_version.h"
#include "rtps_error.h"
#include "rtps_types.h"
#include "rtps_qos.h"
#include "rtps_status.h"
#include "rtps_discovery.h"
#include "rtps_metrics.h"
#include "rtps_participant.h"
#include "rtps_topic.h"
#include "rtps_publisher.h"
#include "rtps_subscriber.h"
#include "rtps_writer.h"
#include "rtps_reader.h"
#include "rtps_waitset.h"

#endif /* RTPS_RTPS_H */
