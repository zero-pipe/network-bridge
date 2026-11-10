#ifndef NB_BASE_JSON_H
#define NB_BASE_JSON_H

/*
 * Shared cJSON readers for the RPC and DDS config parsers.
 *
 * Every reader takes an optional field: an absent key leaves *out untouched
 * and returns NB_OK. Type errors fill nb_config_error_t with parent.key or
 * parent[index].
 */

#include <nb/base/config_error.h>

#include "cJSON.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int nb_json_fail(nb_config_error_t *err, const char *path, const char *reason);
int nb_json_fail_code(nb_config_error_t *err, int code, const char *path, const char *reason);
void nb_json_join(char *out, size_t cap, const char *parent, const char *key);

/* Parse a file (max 1 MiB) and return the root object. */
int nb_json_parse_file(const char *path, cJSON **root, nb_config_error_t *err);
int nb_json_parse_string(const char *json, cJSON **root, nb_config_error_t *err);
/* Read a config file into a NUL-terminated heap buffer. */
int nb_json_load_file(const char *path, char **out_text, nb_config_error_t *err);

int nb_json_object(const cJSON *parent, const char *key, const cJSON **out,
                   nb_config_error_t *err, const char *path);
int nb_json_string_field(const cJSON *obj, const char *key, const char **out,
                         nb_config_error_t *err, const char *path);
int nb_json_u32_field(const cJSON *obj, const char *key, uint32_t *out,
                      nb_config_error_t *err, const char *path);
int nb_json_bool_field(const cJSON *obj, const char *key, bool *out,
                       nb_config_error_t *err, const char *path);
int nb_json_duration_field(const cJSON *obj, const char *key, uint32_t *out_ms,
                           nb_config_error_t *err, const char *path);
int nb_json_enum_field(const cJSON *obj, const char *key,
                       const char *const *names, size_t count, const int *values,
                       int *out, nb_config_error_t *err, const char *path);
/* "host:port" -> host buffer + port. */
int nb_json_host_port(const char *text, char *host, size_t cap, uint16_t *port);
/* Warn about keys not in the known list, prefixed with section. */
void nb_json_warn_unknown(const cJSON *obj, const char *const *known, size_t count,
                          const char *section);

#ifdef __cplusplus
}
#endif

#endif /* NB_BASE_JSON_H */
