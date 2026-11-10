#include "json.h"

#include <nb/base/error.h>
#include <nb/base/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_JSON_MAX_FILE_BYTES (1024u * 1024u)

int nb_json_fail_code(nb_config_error_t *err, int code, const char *path, const char *reason)
{
    if (err) {
        err->code = code;
        snprintf(err->path, sizeof(err->path), "%s", path ? path : "");
        snprintf(err->reason, sizeof(err->reason), "%s", reason ? reason : "");
    }
    return code;
}

int nb_json_fail(nb_config_error_t *err, const char *path, const char *reason)
{
    return nb_json_fail_code(err, NB_ERR_INVALID, path, reason);
}

void nb_json_join(char *out, size_t cap, const char *parent, const char *key)
{
    if (parent && parent[0]) {
        snprintf(out, cap, "%s.%s", parent, key);
    } else {
        snprintf(out, cap, "%s", key);
    }
}

int nb_json_parse_string(const char *json, cJSON **root, nb_config_error_t *err)
{
    if (err) {
        memset(err, 0, sizeof(*err));
    }
    *root = NULL;
    if (!json) {
        return nb_json_fail(err, "", "null argument");
    }
    *root = cJSON_Parse(json);
    if (!*root || !cJSON_IsObject(*root)) {
        cJSON_Delete(*root);
        *root = NULL;
        return nb_json_fail(err, "", "malformed JSON object");
    }
    return NB_OK;
}

int nb_json_load_file(const char *path, char **out_text, nb_config_error_t *err)
{
    FILE *f;
    long size;
    char *buf;

    if (err) {
        memset(err, 0, sizeof(*err));
    }
    if (!out_text) {
        return nb_json_fail(err, "", "null argument");
    }
    *out_text = NULL;
    if (!path) {
        return nb_json_fail(err, "", "null argument");
    }
    f = fopen(path, "rb");
    if (!f) {
        return nb_json_fail_code(err, NB_ERR_IO, "", "cannot open config file");
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return nb_json_fail_code(err, NB_ERR_IO, "", "cannot seek config file");
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return nb_json_fail_code(err, NB_ERR_IO, "", "cannot size config file");
    }
    if ((unsigned long)size > NB_JSON_MAX_FILE_BYTES) {
        fclose(f);
        return nb_json_fail(err, "", "config file too large");
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return nb_json_fail_code(err, NB_ERR_NOMEM, "", "out of memory");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return nb_json_fail_code(err, NB_ERR_IO, "", "cannot read config file");
    }
    buf[size] = '\0';
    fclose(f);
    *out_text = buf;
    return NB_OK;
}

int nb_json_parse_file(const char *path, cJSON **root, nb_config_error_t *err)
{
    char *text = NULL;
    int rc = nb_json_load_file(path, &text, err);

    if (rc != NB_OK) {
        return rc;
    }
    rc = nb_json_parse_string(text, root, err);
    free(text);
    return rc;
}

int nb_json_object(const cJSON *parent, const char *key, const cJSON **out,
                   nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    char full[96];

    *out = NULL;
    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsObject(item)) {
        nb_json_join(full, sizeof(full), path, key);
        return nb_json_fail(err, full, "must be an object");
    }
    *out = item;
    return NB_OK;
}

int nb_json_string_field(const cJSON *obj, const char *key, const char **out,
                         nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        nb_json_join(full, sizeof(full), path, key);
        return nb_json_fail(err, full, "must be a string");
    }
    *out = item->valuestring;
    return NB_OK;
}

int nb_json_u32_field(const cJSON *obj, const char *key, uint32_t *out,
                      nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0 ||
        item->valuedouble > 4294967295.0 ||
        (double)(uint32_t)item->valuedouble != item->valuedouble) {
        nb_json_join(full, sizeof(full), path, key);
        return nb_json_fail(err, full, "must be a non-negative integer");
    }
    *out = (uint32_t)item->valuedouble;
    return NB_OK;
}

int nb_json_bool_field(const cJSON *obj, const char *key, bool *out,
                       nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (!cJSON_IsBool(item)) {
        nb_json_join(full, sizeof(full), path, key);
        return nb_json_fail(err, full, "must be a boolean");
    }
    *out = cJSON_IsTrue(item) ? true : false;
    return NB_OK;
}

/* A duration is a number of milliseconds or a string such as "250ms" / "2s". */
static int nb_json_parse_duration(const char *text, uint32_t *out_ms)
{
    char *end = NULL;
    unsigned long value;

    if (!text || !text[0]) {
        return NB_ERR_INVALID;
    }
    value = strtoul(text, &end, 10);
    if (end == text) {
        return NB_ERR_INVALID;
    }
    if (*end == '\0') {
        /* bare number = milliseconds */
    } else if (strcmp(end, "ms") == 0) {
        /* as written */
    } else if (strcmp(end, "s") == 0) {
        if (value > 4294967295ul / 1000ul) {
            return NB_ERR_INVALID;
        }
        value *= 1000ul;
    } else {
        return NB_ERR_INVALID;
    }
    if (value > 4294967295ul) {
        return NB_ERR_INVALID;
    }
    *out_ms = (uint32_t)value;
    return NB_OK;
}

int nb_json_duration_field(const cJSON *obj, const char *key, uint32_t *out,
                           nb_config_error_t *err, const char *path)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    char full[96];

    if (!item) {
        return NB_OK;
    }
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble < 0.0 || item->valuedouble > 4294967295.0 ||
            (double)(uint32_t)item->valuedouble != item->valuedouble) {
            nb_json_join(full, sizeof(full), path, key);
            return nb_json_fail(err, full, "must be a duration");
        }
        *out = (uint32_t)item->valuedouble;
        return NB_OK;
    }
    if (cJSON_IsString(item) && item->valuestring &&
        nb_json_parse_duration(item->valuestring, out) == NB_OK) {
        return NB_OK;
    }
    nb_json_join(full, sizeof(full), path, key);
    return nb_json_fail(err, full, "must be a duration like \"250ms\" or \"2s\"");
}

int nb_json_enum_field(const cJSON *obj, const char *key,
                       const char *const *names, size_t count, const int *values,
                       int *out, nb_config_error_t *err, const char *path)
{
    const char *text = NULL;
    char full[96];
    int rc = nb_json_string_field(obj, key, &text, err, path);
    size_t i;

    if (rc != NB_OK || !text) {
        return rc;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(text, names[i]) == 0) {
            *out = values[i];
            return NB_OK;
        }
    }
    nb_json_join(full, sizeof(full), path, key);
    return nb_json_fail(err, full, "invalid value");
}

int nb_json_host_port(const char *text, char *host, size_t cap, uint16_t *port)
{
    const char *colon = text ? strrchr(text, ':') : NULL;
    size_t len;
    unsigned long value;
    char *end = NULL;

    if (!colon || colon == text) {
        return NB_ERR_INVALID;
    }
    len = (size_t)(colon - text);
    if (len == 0 || len >= cap) {
        return NB_ERR_INVALID;
    }
    value = strtoul(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || value > 65535ul) {
        return NB_ERR_INVALID;
    }
    memcpy(host, text, len);
    host[len] = '\0';
    *port = (uint16_t)value;
    return NB_OK;
}

void nb_json_warn_unknown(const cJSON *obj, const char *const *known, size_t count,
                          const char *section)
{
    const cJSON *child;

    if (!obj || !cJSON_IsObject(obj)) {
        return;
    }
    cJSON_ArrayForEach(child, obj)
    {
        size_t i;
        int found = 0;
        char full[96];

        if (!child->string) {
            continue;
        }
        for (i = 0; i < count; i++) {
            if (known[i] && strcmp(known[i], child->string) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            nb_json_join(full, sizeof(full), section, child->string);
            nb_log_warn("unknown config key ignored: %s", full);
        }
    }
}
