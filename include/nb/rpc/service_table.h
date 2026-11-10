#ifndef NB_SERVICE_TABLE_H
#define NB_SERVICE_TABLE_H

/*
 * Service/method declaration table (shared contract).
 *
 * Mirrors fc_service_table from fastcall-c: service/method names and numeric
 * IDs live in a header shared by both ends; client and server install the same
 * table and therefore agree on names and numbering. Numeric IDs are currently
 * used for declaration validation and metadata (routing still uses the
 * "service.method" string).
 *
 * Typical use (in a header shared by both ends):
 *
 *     NB_SERVICE_DEFINE(order_table, 1u, "OrderService",
 *         NB_SERVICE_METHOD(1u, "get"),
 *         NB_SERVICE_METHOD(2u, "put"));
 *
 * Then either set &order_table in nb_node_config_t::service_table, or call
 * nb_service_table_install(&order_table) before creating a node.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Declaration structs ---- */
typedef struct nb_method_descriptor {
    uint16_t id;
    const char *name;
} nb_method_descriptor_t;

typedef struct nb_service_descriptor {
    uint16_t id;
    const char *name;
    const nb_method_descriptor_t *methods;
    size_t method_count;
} nb_service_descriptor_t;

typedef struct nb_service_table {
    const nb_service_descriptor_t *services;
    size_t service_count;
} nb_service_table_t;

/* ---- Install and query ---- */
/*
 * Install a declaration table. May be called several times before creating
 * nodes (multiple tables coexist); installing the same declaration again is
 * idempotent. A service/method with the same name but a different ID is
 * rejected with NB_ERR_INVALID. The table is borrowed by pointer and must have
 * static lifetime (as produced by NB_SERVICE_DEFINE).
 */
int nb_service_table_install(const nb_service_table_t *table);

/* Clear all installed declarations. Mainly for tests. */
void nb_service_table_reset(void);

/* True when any declaration table is installed (fast hot-path check). */
bool nb_service_table_is_active(void);

/* Look up the numeric ID of service.method; returns NB_OK when found. Any NULL
 * argument returns NB_ERR_INVALID. */
int nb_service_table_find_id(const char *service, const char *method, uint16_t *out_id);

/* ---- Declaration macros ---- */
/* Declare one method row; only valid inside NB_SERVICE_DEFINE(). */
#define NB_SERVICE_METHOD(id, name) { (uint16_t)(id), (name) }

/* Declare a single-service table; expands to <table>_methods, <table>_service,
 * and <table>. */
#define NB_SERVICE_DEFINE(table, service_id, service_name, ...)                                \
    static const nb_method_descriptor_t table##_methods[] = { __VA_ARGS__ };                   \
    static const nb_service_descriptor_t table##_service = { (uint16_t)(service_id),           \
                                                          (service_name),                   \
                                                          table##_methods,                  \
                                                          sizeof(table##_methods) /         \
                                                              sizeof(table##_methods[0]) }; \
    static const nb_service_table_t table = { &table##_service, 1u }

#ifdef __cplusplus
}
#endif

#endif /* NB_SERVICE_TABLE_H */
