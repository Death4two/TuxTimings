#ifndef INTEL_H
#define INTEL_H

#include "types.h"

/*
 * intel.h — Intel DDR5 timing and CPU info reader
 *
 * Supports 12th–15th gen Intel CPUs (Alder Lake, Raptor Lake, Arrow Lake).
 * Reads timings from MCHBAR via /dev/mem. Requires root.
 */

/*
 * Detect whether the current CPU is Intel.
 * Reads /proc/cpuinfo for vendor_id, family, model, microcode.
 * Populates vendor, intel_gen, cpu_model, codename, microcode_version,
 * and processor_name in out_cpu.
 * Returns 1 if Intel detected, 0 otherwise.
 */
int intel_detect_cpu(cpu_info_t *out_cpu);

/*
 * Read Intel DDR5 timings via MCHBAR MMIO.
 * Maps /dev/mem at the MCHBAR physical base (read from PCI config space).
 * gen selects register layout: INTEL_GEN_12_14 vs INTEL_GEN_15.
 * Silently returns zeroed timings if /dev/mem mmap fails.
 */
void intel_read_timings(intel_gen_t gen, dram_timings_t *out_dram);

/*
 * Read Intel power/voltage metrics.
 * Uses RAPL powercap sysfs for package power.
 * SA/VDDQ voltages have no standard Linux interface — left at 0.
 */
void intel_read_voltages(smu_metrics_t *out_metrics);

/*
 * Unmap MCHBAR region if mapped. Call on exit or when Intel support
 * is no longer needed.
 */
void intel_cleanup(void);

#endif /* INTEL_H */
