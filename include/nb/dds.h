#ifndef NB_DDS_H
#define NB_DDS_H

/*
 * DDS spine: participant / topic / writer / reader over a selectable backend.
 *
 *   <nb/dds/participant.h>  domain, backend selection, status, discovery
 *   <nb/dds/topic.h>        name + type name + QoS
 *   <nb/dds/writer.h>       publications
 *   <nb/dds/reader.h>       subscriptions, history, wait sets
 *   <nb/dds/qos.h>          DDS policy set
 *   <nb/dds/backend.h>      backend SPI (rtps / cyclone)
 *
 * The spine is independent of the RPC node: it owns its DDS domain and lets
 * the backend run discovery (SPDP/SEDP). RPC links (tcp/udp/shm) never appear
 * here, and DDS topics never appear in the RPC spine.
 */

#include <nb/base.h>
#include <nb/dds/qos.h>
#include <nb/dds/sample.h>
#include <nb/dds/status.h>
#include <nb/dds/discovery_types.h>
#include <nb/dds/participant.h>
#include <nb/dds/topic.h>
#include <nb/dds/writer.h>
#include <nb/dds/reader.h>
#include <nb/dds/backend.h>

#endif /* NB_DDS_H */
