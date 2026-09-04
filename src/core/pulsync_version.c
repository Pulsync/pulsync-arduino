/**
 * pulsync_version — implementation. See pulsync_version.h.
 */

#include "pulsync_version.h"

#include <string.h>

#define PULSYNC_VERSION_MAX 32

/* Runtime override buffer. Empty first byte => no override set. */
static char s_version_override[PULSYNC_VERSION_MAX] = {0};

void pulsync_version_set(const char *version) {
    if (!version || version[0] == '\0') {
        s_version_override[0] = '\0';
        return;
    }
    strncpy(s_version_override, version, PULSYNC_VERSION_MAX - 1);
    s_version_override[PULSYNC_VERSION_MAX - 1] = '\0';
}

const char *pulsync_version_get(void) {
    if (s_version_override[0] != '\0') {
        return s_version_override;
    }
#ifdef PULSYNC_FW_VERSION
    return PULSYNC_FW_VERSION;
#else
    return "0.0.0";
#endif
}
