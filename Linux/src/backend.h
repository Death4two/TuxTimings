#ifndef BACKEND_H
#define BACKEND_H

#include "types.h"

/* Returns true if the ryzen_smu driver is loaded and accessible. */
int backend_is_supported(void);

/* Read all system data into out. Call every ~1 second for live refresh.
 * Static data (dmidecode, AGESA) is cached after first call. */
void backend_read_summary(system_summary_t *out);

#endif
