#ifndef PM_TABLE_H
#define PM_TABLE_H

#include "types.h"

/* Read PM table binary and apply family-specific index mappings to fill metrics.
 * version: PM table version from /sys/kernel/ryzen_smu_drv/pm_table_version
 * table:   array of floats from pm_table binary
 * count:   number of floats
 * codename_index: from /sys/kernel/ryzen_smu_drv/codename
 */
void pm_table_read(uint32_t version, const float *table, int count,
                   int codename_index, smu_metrics_t *out);

#endif
