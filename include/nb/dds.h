#ifndef NB_DDS_H
#define NB_DDS_H

/*
 * Cyclone DDS transport provider.
 *
 * Register the provider before creating any node, then use a DDS binding URI of
 * the form "dds://<domain>". The host component selects the DDS domain; the
 * port is optional and ignored ("dds://<domain>:<n>" is accepted for
 * compatibility). DDS/other non-network schemes may omit the port; tcp/udp
 * URIs still require one.
 *
 * Topics are derived from the full "Service.method" route, so two services
 * that share a method name stay on distinct topics:
 *   <Service>_<method>_req  request topic
 *   <Service>_<method>_res  response topic
 *   <Service>_<method>_evt  one-way event / pub-sub topic
 * Characters outside [A-Za-z0-9_] are replaced with '_'.
 *
 * QoS is taken from nb_binding_config_t.qos (or the inherited transport.qos)
 * and mapped to Cyclone DDS reader/writer policies; see qos.h.
 */

#include <nb/transport_provider.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Idempotently register the built-in "dds" provider. */
int nb_cyclone_dds_register(void);

/* Static provider descriptor, for callers that prefer explicit registration. */
const nb_transport_provider_t *nb_cyclone_dds_provider(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_DDS_H */
