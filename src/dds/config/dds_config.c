/*
 * DDS spine JSON configuration.
 *
 * Parses name plus the `dds` block; an `rpc` block in the same document is
 * ignored silently. See docs/api/config.md.
 */
#include <nb/dds/config.h>

#include <nb/base/log.h>

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_KEYS(array) array, sizeof(array) / sizeof((array)[0])

struct nb_dds_config_doc {
    nb_dds_config_t dds;
    nb_dds_peer_config_t peers[NB_DDS_CONFIG_MAX_PEERS];
};

typedef nb_dds_config_doc_t dds_doc_t;

void nb_dds_config_default(nb_dds_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->backend = NB_DDS_BACKEND_RTPS;
    config->domain = 0;
    snprintf(config->name, sizeof(config->name), "%s", "nb-dds");
    nb_dds_qos_init(&config->qos);
    config->rtps.bind_host[0] = '\0';
    config->rtps.multicast_group[0] = '\0';
    config->rtps.announce_interval_ms = 0;
    config->rtps.lease_ms = 0;
}

static int cfg_parse_qos(const cJSON *obj, nb_dds_qos_t *qos, nb_config_error_t *err,
                         const char *path)
{
    static const char *const known[] = {
        "reliability", "durability", "history", "depth", "deadline", "lifespan",
        "latency_budget", "min_separation", "liveliness", "lease_duration",
        "ownership", "partition", "max_samples", "max_instances",
        "max_samples_per_instance", "autodispose_unregistered",
        "presentation_scope", "coherent_access", "ordered_access"};
    static const char *const reliability_names[] = {"best_effort", "reliable"};
    static const int reliability_values[] = {NB_DDS_RELIABILITY_BEST_EFFORT,
                                             NB_DDS_RELIABILITY_RELIABLE};
    static const char *const durability_names[] = {"volatile", "transient_local"};
    static const int durability_values[] = {NB_DDS_DURABILITY_VOLATILE,
                                            NB_DDS_DURABILITY_TRANSIENT_LOCAL};
    static const char *const history_names[] = {"keep_last", "keep_all"};
    static const int history_values[] = {NB_DDS_HISTORY_KEEP_LAST, NB_DDS_HISTORY_KEEP_ALL};
    static const char *const liveliness_names[] = {"automatic", "manual_by_participant",
                                                   "manual_by_topic"};
    static const int liveliness_values[] = {NB_DDS_LIVELINESS_AUTOMATIC,
                                            NB_DDS_LIVELINESS_MANUAL_BY_PARTICIPANT,
                                            NB_DDS_LIVELINESS_MANUAL_BY_TOPIC};
    static const char *const ownership_names[] = {"shared", "exclusive"};
    static const int ownership_values[] = {NB_DDS_OWNERSHIP_SHARED, NB_DDS_OWNERSHIP_EXCLUSIVE};
    static const char *const presentation_names[] = {"instance", "topic", "group"};
    static const int presentation_values[] = {NB_DDS_PRESENTATION_INSTANCE,
                                              NB_DDS_PRESENTATION_TOPIC,
                                              NB_DDS_PRESENTATION_GROUP};
    const char *text = NULL;
    uint32_t value = 0;
    int ivalue = 0;
    int rc;

    nb_json_warn_unknown(obj, CFG_KEYS(known), path);
    nb_dds_qos_init(qos);

    rc = nb_json_enum_field(obj, "reliability", CFG_KEYS(reliability_names),
                            reliability_values, &ivalue, err, path);
    if (rc == NB_OK && (cJSON_GetObjectItemCaseSensitive(obj, "reliability") != NULL)) {
        nb_dds_qos_set_reliability(qos, (nb_dds_reliability_t)ivalue);
    }
    ivalue = 0;
    if (rc == NB_OK) {
        rc = nb_json_enum_field(obj, "durability", CFG_KEYS(durability_names),
                                durability_values, &ivalue, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "durability")) {
            nb_dds_qos_set_durability(qos, (nb_dds_durability_t)ivalue);
        }
    }
    if (rc == NB_OK) {
        int kind = 0;
        uint32_t depth = 0;

        rc = nb_json_enum_field(obj, "history", CFG_KEYS(history_names), history_values,
                                &kind, err, path);
        if (rc == NB_OK) {
            rc = nb_json_u32_field(obj, "depth", &depth, err, path);
        }
        if (rc == NB_OK &&
            (cJSON_GetObjectItemCaseSensitive(obj, "history") ||
             cJSON_GetObjectItemCaseSensitive(obj, "depth"))) {
            nb_dds_qos_set_history(qos, (nb_dds_history_t)kind, depth);
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "deadline", &value, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "deadline")) {
            nb_dds_qos_set_deadline_ms(qos, value);
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "lifespan", &value, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "lifespan")) {
            nb_dds_qos_set_lifespan_ms(qos, value);
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "latency_budget", &value, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "latency_budget")) {
            nb_dds_qos_set_latency_budget_ms(qos, value);
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "min_separation", &value, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "min_separation")) {
            nb_dds_qos_set_min_separation_ms(qos, value);
        }
    }
    if (rc == NB_OK) {
        int kind = 0;
        uint32_t lease = 0;

        rc = nb_json_enum_field(obj, "liveliness", CFG_KEYS(liveliness_names),
                                liveliness_values, &kind, err, path);
        if (rc == NB_OK) {
            rc = nb_json_duration_field(obj, "lease_duration", &lease, err, path);
        }
        if (rc == NB_OK &&
            (cJSON_GetObjectItemCaseSensitive(obj, "liveliness") ||
             cJSON_GetObjectItemCaseSensitive(obj, "lease_duration"))) {
            nb_dds_qos_set_liveliness(qos, (nb_dds_liveliness_t)kind, lease);
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_enum_field(obj, "ownership", CFG_KEYS(ownership_names), ownership_values,
                                &ivalue, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "ownership")) {
            nb_dds_qos_set_ownership(qos, (nb_dds_ownership_t)ivalue);
        }
    }
    if (rc == NB_OK) {
        rc = nb_json_string_field(obj, "partition", &text, err, path);
        if (rc == NB_OK && text) {
            if (strlen(text) >= NB_DDS_QOS_PARTITION_MAX) {
                char full[96];

                nb_json_join(full, sizeof(full), path, "partition");
                return nb_json_fail(err, full, "too long");
            }
            nb_dds_qos_set_partition(qos, text);
        }
    }
    if (rc == NB_OK) {
        uint32_t max_samples = 0;
        uint32_t max_instances = 0;
        uint32_t max_per_instance = 0;

        rc = nb_json_u32_field(obj, "max_samples", &max_samples, err, path);
        if (rc == NB_OK) {
            rc = nb_json_u32_field(obj, "max_instances", &max_instances, err, path);
        }
        if (rc == NB_OK) {
            rc = nb_json_u32_field(obj, "max_samples_per_instance", &max_per_instance, err,
                                   path);
        }
        if (rc == NB_OK && (cJSON_GetObjectItemCaseSensitive(obj, "max_samples") ||
                            cJSON_GetObjectItemCaseSensitive(obj, "max_instances") ||
                            cJSON_GetObjectItemCaseSensitive(obj, "max_samples_per_instance"))) {
            nb_dds_qos_set_resource_limits(qos, max_samples, max_instances, max_per_instance);
        }
    }
    if (rc == NB_OK) {
        bool enabled = false;

        rc = nb_json_bool_field(obj, "autodispose_unregistered", &enabled, err, path);
        if (rc == NB_OK && cJSON_GetObjectItemCaseSensitive(obj, "autodispose_unregistered")) {
            nb_dds_qos_set_autodispose(qos, enabled);
        }
    }
    if (rc == NB_OK) {
        int scope = 0;
        bool coherent = false;
        bool ordered = false;

        rc = nb_json_enum_field(obj, "presentation_scope", CFG_KEYS(presentation_names),
                                presentation_values, &scope, err, path);
        if (rc == NB_OK) {
            rc = nb_json_bool_field(obj, "coherent_access", &coherent, err, path);
        }
        if (rc == NB_OK) {
            rc = nb_json_bool_field(obj, "ordered_access", &ordered, err, path);
        }
        if (rc == NB_OK && (cJSON_GetObjectItemCaseSensitive(obj, "presentation_scope") ||
                            cJSON_GetObjectItemCaseSensitive(obj, "coherent_access") ||
                            cJSON_GetObjectItemCaseSensitive(obj, "ordered_access"))) {
            nb_dds_qos_set_presentation(qos, (nb_dds_presentation_scope_t)scope, coherent,
                                        ordered);
        }
    }
    if (rc != NB_OK) {
        return rc;
    }
    if (nb_dds_qos_validate(qos) != NB_OK) {
        return nb_json_fail(err, path, nb_dds_qos_validate_reason(qos));
    }
    return NB_OK;
}

static int cfg_parse_peers(dds_doc_t *doc, const cJSON *rtps, nb_config_error_t *err)
{
    const cJSON *peers = cJSON_GetObjectItemCaseSensitive(rtps, "peers");
    const cJSON *item;
    size_t index = 0;

    if (!peers) {
        return NB_OK;
    }
    if (!cJSON_IsArray(peers)) {
        return nb_json_fail(err, "dds.rtps.peers", "must be an array");
    }
    cJSON_ArrayForEach(item, peers)
    {
        char path[96];
        nb_dds_peer_config_t *peer;

        snprintf(path, sizeof(path), "dds.rtps.peers[%lu]", (unsigned long)index++);
        if (!cJSON_IsString(item) || !item->valuestring) {
            return nb_json_fail(err, path, "must be a string");
        }
        if (doc->dds.rtps.peer_count >= NB_DDS_CONFIG_MAX_PEERS) {
            return nb_json_fail(err, "dds.rtps.peers", "too many peers");
        }
        peer = &doc->peers[doc->dds.rtps.peer_count];
        if (nb_json_host_port(item->valuestring, peer->host, sizeof(peer->host), &peer->port) !=
            NB_OK) {
            return nb_json_fail(err, path, "must be host:port");
        }
        doc->dds.rtps.peer_count++;
    }
    doc->dds.rtps.peers = doc->peers;
    return NB_OK;
}

static int cfg_copy_string(char *dst, size_t cap, const char *src, nb_config_error_t *err,
                           const char *path)
{
    if (!src) {
        return NB_OK;
    }
    if (strlen(src) >= cap) {
        return nb_json_fail(err, path, "too long");
    }
    snprintf(dst, cap, "%s", src);
    return NB_OK;
}

static int cfg_parse_rtps(dds_doc_t *doc, const cJSON *dds, nb_config_error_t *err)
{
    static const char *const known[] = {
        "bind_host", "advertise_host", "multicast_group", "spdp_port", "unicast_port",
        "announce_interval", "lease", "data_heartbeat", "data_history_depth",
        "tcp", "tcp_port", "standard_ports", "participant_id", "peers"};
    const cJSON *obj = NULL;
    const char *text = NULL;
    uint32_t value = 0;
    bool flag = false;
    int rc = nb_json_object(dds, "rtps", &obj, err, "dds");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "dds.rtps");
    doc->dds.has_rtps = true;
    rc = nb_json_string_field(obj, "bind_host", &text, err, "dds.rtps");
    if (rc == NB_OK) {
        rc = cfg_copy_string(doc->dds.rtps.bind_host, sizeof(doc->dds.rtps.bind_host), text, err,
                             "dds.rtps.bind_host");
    }
    text = NULL;
    if (rc == NB_OK) {
        rc = nb_json_string_field(obj, "advertise_host", &text, err, "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = cfg_copy_string(doc->dds.rtps.advertise_host,
                             sizeof(doc->dds.rtps.advertise_host), text, err,
                             "dds.rtps.advertise_host");
    }
    text = NULL;
    if (rc == NB_OK) {
        rc = nb_json_string_field(obj, "multicast_group", &text, err, "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = cfg_copy_string(doc->dds.rtps.multicast_group,
                             sizeof(doc->dds.rtps.multicast_group), text, err,
                             "dds.rtps.multicast_group");
    }
    if (rc == NB_OK) {
        value = doc->dds.rtps.spdp_port;
        rc = nb_json_u32_field(obj, "spdp_port", &value, err, "dds.rtps");
        doc->dds.rtps.spdp_port = (uint16_t)value;
    }
    if (rc == NB_OK) {
        value = doc->dds.rtps.unicast_port;
        rc = nb_json_u32_field(obj, "unicast_port", &value, err, "dds.rtps");
        doc->dds.rtps.unicast_port = (uint16_t)value;
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "announce_interval",
                                    &doc->dds.rtps.announce_interval_ms, err, "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "lease", &doc->dds.rtps.lease_ms, err, "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = nb_json_duration_field(obj, "data_heartbeat",
                                    &doc->dds.rtps.data_heartbeat_ms, err, "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "data_history_depth",
                               &doc->dds.rtps.data_history_depth, err, "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = nb_json_bool_field(obj, "tcp", &flag, err, "dds.rtps");
        doc->dds.rtps.enable_tcp = flag;
    }
    if (rc == NB_OK) {
        value = doc->dds.rtps.tcp_port;
        rc = nb_json_u32_field(obj, "tcp_port", &value, err, "dds.rtps");
        doc->dds.rtps.tcp_port = (uint16_t)value;
    }
    if (rc == NB_OK) {
        rc = nb_json_bool_field(obj, "standard_ports", &flag, err, "dds.rtps");
        doc->dds.rtps.standard_ports = flag;
    }
    if (rc == NB_OK) {
        rc = nb_json_u32_field(obj, "participant_id", &doc->dds.rtps.participant_id, err,
                               "dds.rtps");
    }
    if (rc == NB_OK) {
        rc = cfg_parse_peers(doc, obj, err);
    }
    return rc;
}

static int cfg_parse_cyclone(dds_doc_t *doc, const cJSON *dds, nb_config_error_t *err)
{
    static const char *const known[] = {"config_xml"};
    const cJSON *obj = NULL;
    const char *text = NULL;
    int rc = nb_json_object(dds, "cyclone", &obj, err, "dds");

    if (rc != NB_OK || !obj) {
        return rc;
    }
    nb_json_warn_unknown(obj, CFG_KEYS(known), "dds.cyclone");
    doc->dds.has_cyclone = true;
    rc = nb_json_string_field(obj, "config_xml", &text, err, "dds.cyclone");
    if (rc == NB_OK) {
        rc = cfg_copy_string(doc->dds.cyclone_config_xml, sizeof(doc->dds.cyclone_config_xml),
                             text, err, "dds.cyclone.config_xml");
    }
    return rc;
}

static int cfg_parse_dds(dds_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    static const char *const known[] = {"backend", "domain", "partition", "prefix", "name",
                                        "qos", "rtps", "cyclone"};
    static const char *const backend_names[] = {"rtps", "cyclone"};
    static const int backend_values[] = {NB_DDS_BACKEND_RTPS, NB_DDS_BACKEND_CYCLONE};
    const cJSON *dds = NULL;
    const cJSON *qos = NULL;
    const char *text = NULL;
    int backend = NB_DDS_BACKEND_RTPS;
    int rc = nb_json_object(root, "dds", &dds, err, "");

    if (rc != NB_OK || !dds) {
        return rc;
    }
    nb_json_warn_unknown(dds, CFG_KEYS(known), "dds");
    rc = nb_json_enum_field(dds, "backend", CFG_KEYS(backend_names), backend_values, &backend,
                            err, "dds");
    if (rc == NB_OK) {
        doc->dds.backend = (nb_dds_backend_t)backend;
    }
    if (rc == NB_OK) {
        rc = nb_json_u32_field(dds, "domain", &doc->dds.domain, err, "dds");
    }
    if (rc == NB_OK) {
        rc = nb_json_string_field(dds, "partition", &text, err, "dds");
    }
    if (rc == NB_OK && text) {
        rc = cfg_copy_string(doc->dds.partition, sizeof(doc->dds.partition), text, err,
                             "dds.partition");
    }
    text = NULL;
    if (rc == NB_OK) {
        rc = nb_json_string_field(dds, "prefix", &text, err, "dds");
    }
    if (rc == NB_OK && text) {
        rc = cfg_copy_string(doc->dds.prefix, sizeof(doc->dds.prefix), text, err, "dds.prefix");
    }
    text = NULL;
    if (rc == NB_OK) {
        rc = nb_json_string_field(dds, "name", &text, err, "dds");
    }
    if (rc == NB_OK && text) {
        rc = cfg_copy_string(doc->dds.name, sizeof(doc->dds.name), text, err, "dds.name");
    }
    if (rc == NB_OK) {
        rc = nb_json_object(dds, "qos", &qos, err, "dds");
    }
    if (rc == NB_OK && qos) {
        rc = cfg_parse_qos(qos, &doc->dds.qos, err, "dds.qos");
    }
    if (rc == NB_OK) {
        rc = cfg_parse_rtps(doc, dds, err);
    }
    if (rc == NB_OK) {
        rc = cfg_parse_cyclone(doc, dds, err);
    }
    return rc;
}

static int cfg_parse_name(dds_doc_t *doc, const cJSON *root, nb_config_error_t *err)
{
    const char *name = NULL;
    int rc = nb_json_string_field(root, "name", &name, err, "");

    if (rc != NB_OK) {
        return rc;
    }
    if (name) {
        rc = cfg_copy_string(doc->dds.name, sizeof(doc->dds.name), name, err, "name");
    }
    return rc;
}

int nb_dds_config_parse(const char *json, nb_dds_config_doc_t **out, nb_config_error_t *err)
{
    static const char *const top_keys[] = {"name", "threads", "limits", "rpc", "dds",
                                           "metrics", "log", "$schema"};
    cJSON *root = NULL;
    dds_doc_t *doc;
    int rc;

    if (!out) {
        return nb_json_fail(err, "", "null argument");
    }
    *out = NULL;
    rc = nb_json_parse_string(json, &root, err);
    if (rc != NB_OK) {
        return rc;
    }
    doc = (dds_doc_t *)calloc(1, sizeof(*doc));
    if (!doc) {
        cJSON_Delete(root);
        return nb_json_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    nb_dds_config_default(&doc->dds);
    nb_json_warn_unknown(root, CFG_KEYS(top_keys), "");

    rc = cfg_parse_name(doc, root, err);
    if (rc == NB_OK) {
        rc = cfg_parse_dds(doc, root, err);
    }
    if (rc != NB_OK) {
        goto done;
    }
    if (nb_dds_qos_validate(&doc->dds.qos) != NB_OK) {
        rc = nb_json_fail(err, "dds.qos", nb_dds_qos_validate_reason(&doc->dds.qos));
        goto done;
    }
    *out = doc;
    doc = NULL;
    rc = NB_OK;

done:
    cJSON_Delete(root);
    free(doc);
    return rc;
}

int nb_dds_config_parse_file(const char *path,
                             nb_dds_config_doc_t **out,
                             nb_config_error_t *err)
{
    char *text = NULL;
    int rc;

    if (!out) {
        return nb_json_fail(err, "", "null argument");
    }
    *out = NULL;
    rc = nb_json_load_file(path, &text, err);
    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_dds_config_parse(text, out, err);
    free(text);
    return rc;
}

void nb_dds_config_doc_destroy(nb_dds_config_doc_t *doc)
{
    free(doc);
}

const nb_dds_config_t *nb_dds_config_get(const nb_dds_config_doc_t *doc)
{
    return doc ? &doc->dds : NULL;
}

int nb_dds_participant_create_from_config(const nb_dds_config_doc_t *doc,
                                          nb_dds_participant_t **out)
{
    nb_dds_participant_options_t options;
    nb_dds_rtps_options_t rtps;
    nb_dds_cyclone_options_t cyclone;
    const char *peer_strings[NB_DDS_CONFIG_MAX_PEERS];
    char peer_text[NB_DDS_CONFIG_MAX_PEERS][NB_HOST_MAX + 8];
    const nb_dds_config_t *cfg;

    if (!doc || !out) {
        return NB_ERR_INVALID;
    }
    cfg = &doc->dds;
    memset(&options, 0, sizeof(options));
    options.backend = cfg->backend;
    options.domain = cfg->domain;
    options.partition = cfg->partition[0] ? cfg->partition : NULL;
    options.prefix = cfg->prefix[0] ? cfg->prefix : NULL;
    options.name = cfg->name;
    options.default_qos = &cfg->qos;
    if (cfg->has_rtps || cfg->backend == NB_DDS_BACKEND_RTPS) {
        memset(&rtps, 0, sizeof(rtps));
        rtps.struct_size = sizeof(rtps);
        rtps.bind_host = cfg->rtps.bind_host[0] ? cfg->rtps.bind_host : NULL;
        rtps.advertise_host = cfg->rtps.advertise_host[0] ? cfg->rtps.advertise_host : NULL;
        rtps.multicast_group =
            cfg->rtps.multicast_group[0] ? cfg->rtps.multicast_group : NULL;
        rtps.spdp_port = cfg->rtps.spdp_port;
        rtps.unicast_port = cfg->rtps.unicast_port;
        rtps.announce_interval_ms = cfg->rtps.announce_interval_ms;
        rtps.lease_ms = cfg->rtps.lease_ms;
        rtps.data_heartbeat_ms = cfg->rtps.data_heartbeat_ms;
        rtps.data_history_depth = cfg->rtps.data_history_depth;
        rtps.enable_tcp = cfg->rtps.enable_tcp ? 1 : 0;
        rtps.tcp_port = cfg->rtps.tcp_port;
        rtps.standard_ports = cfg->rtps.standard_ports ? 1 : 0;
        rtps.participant_id = cfg->rtps.participant_id;
        rtps.peers = NULL;
        rtps.peer_count = 0;
        if (cfg->rtps.peer_count > 0) {
            size_t i;

            for (i = 0; i < cfg->rtps.peer_count; i++) {
                snprintf(peer_text[i], sizeof(peer_text[i]), "%s:%u",
                         cfg->rtps.peers[i].host, (unsigned)cfg->rtps.peers[i].port);
                peer_strings[i] = peer_text[i];
            }
            rtps.peers = peer_strings;
            rtps.peer_count = cfg->rtps.peer_count;
        }
        options.rtps = &rtps;
    }
    if (cfg->has_cyclone || cfg->backend == NB_DDS_BACKEND_CYCLONE) {
        memset(&cyclone, 0, sizeof(cyclone));
        cyclone.struct_size = sizeof(cyclone);
        cyclone.config_xml =
            cfg->cyclone_config_xml[0] ? cfg->cyclone_config_xml : NULL;
        options.cyclone = &cyclone;
    }
    return nb_dds_participant_create(&options, out);
}
