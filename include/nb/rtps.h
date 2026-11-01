#ifndef NB_RTPS_H
#define NB_RTPS_H

/*
 * RTPS transport provider backed by the in-tree RTPS implementation.
 *
 * Register the provider before creating any node, then use a binding URI:
 *   rtps://<domain>                           multicast discovery, ephemeral ports
 *   rtps://<domain>:<spdp-port>               unicast only, fixed discovery port
 *   rtps://<domain>:<port>/<host>:<port>,...  explicit unicast peers
 *
 * A port or peer list disables multicast, because both pin discovery to
 * explicit endpoints.
 *
 * Like the Cyclone provider, the plain "rtps://<domain>" form needs no address
 * configuration: the participant binds 0.0.0.0 and announces the first
 * non-loopback IPv4 address of the host, falling back to loopback when there is
 * none. Set NB_RTPS_ADVERTISE_HOST to pick a specific interface.
 *
 * Topics are derived from the full "Service.method" route the same way as the
 * Cyclone provider:
 *   <Service>_<method>_req  request topic
 *   <Service>_<method>_res  response topic
 *   <Service>_<method>_evt  one-way event / pub-sub topic
 *
 * QoS is taken from nb_binding_config_t.qos (or the inherited transport.qos);
 * reliability, durability, history depth and partition are mapped onto the
 * participant's endpoint policies.
 */

#include <nb/transport_provider.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotently register the built-in "rtps" provider. */
int nb_rtps_transport_register(void);

/* Static provider descriptor, for callers that prefer explicit registration. */
const nb_transport_provider_t *nb_rtps_transport_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_RTPS_H */
