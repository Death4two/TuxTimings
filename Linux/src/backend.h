#ifndef BACKEND_H
#define BACKEND_H

#include "types.h"

/* Returns true if the ryzen_smu driver is loaded and accessible. */
int backend_is_supported(void);

/* Read all system data into out. Call every ~1 second for live refresh.
 * Static data (dmidecode, AGESA) is cached after first call. */
void backend_read_summary(system_summary_t *out);

/* Unload any kernel modules that were loaded by backend_read_summary().
 * Call once on application exit. */
void backend_cleanup(void);
void backend_set_tuxbench_loaded(void);

/* Returns a malloc'd string with a raw PM table + AOD sysfs debug dump.
 * Caller must free(). Returns NULL on failure. */
char *backend_read_debug_dump(void);

#endif
