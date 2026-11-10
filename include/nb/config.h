#ifndef NB_CONFIG_H
#define NB_CONFIG_H

/*
 * Convenience umbrella for both configuration parsers. The spines stay
 * independent: include <nb/rpc/config.h> or <nb/dds/config.h> directly if an
 * application only uses one.
 */

#include <nb/rpc/config.h>
#include <nb/dds/config.h>

#endif /* NB_CONFIG_H */
