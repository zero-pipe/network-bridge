/* Version symbol for the umbrella `nb` library.
 *
 * The real code lives in nb_rpc and nb_dds; this tiny object keeps `nb` a
 * linkable static archive for consumers that link the combined SDK. */
#include <nb/base/types.h>

const char *nb_version_string(void)
{
    return NB_VERSION_STRING;
}
