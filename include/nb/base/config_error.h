#ifndef NB_BASE_CONFIG_ERROR_H
#define NB_BASE_CONFIG_ERROR_H

/* Shared structured JSON parse error for both config parsers. */

typedef struct nb_config_error {
    int code; /* NB_ERR_* */
    char path[96];
    char reason[128];
} nb_config_error_t;

#endif /* NB_BASE_CONFIG_ERROR_H */
