#ifndef DRAM_H
#define DRAM_H

#include "types.h"

/* Read DRAM timings based on codename index.
 * 23 = Granite Ridge DDR5
 * 4,9,10,12,18,19 = DDR4 desktop/HEDT families
 */
void dram_read_timings(int codename_index, dram_timings_t *out);

#endif
