/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tuxbench.h — shared ABI between the tuxbench kernel module and bench.c
 *
 * Userspace opens /dev/tuxbench and calls ioctl(fd, TUXBENCH_IOC_RUN, &req).
 * The kernel module runs the benchmark with guaranteed huge pages, hard CPU
 * pinning (kthread_bind), and wbinvd for full cache hierarchy flush, then
 * copies results back.
 *
 * If the module is not loaded, bench.c falls back to its userspace path.
 */

#ifndef TUXBENCH_H
#define TUXBENCH_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Flags for tuxbench_req.flags */
#define TUXBENCH_FL_LAT   (1U << 0)  /* measure cache/DRAM latency   */
#define TUXBENCH_FL_BW    (1U << 1)  /* measure read/write/copy BW   */

struct tuxbench_req {
    __u32 flags;          /* TUXBENCH_FL_LAT | TUXBENCH_FL_BW    */
    __u32 pad;

    /* results — filled by kernel on return */
    __u64 lat_l1_ps;      /* L1  latency, picoseconds             */
    __u64 lat_l2_ps;      /* L2  latency, picoseconds             */
    __u64 lat_l3_ps;      /* L3  latency, picoseconds             */
    __u64 lat_dram_ps;    /* DRAM latency, picoseconds            */
    __u64 bw_read_kbs;    /* read  bandwidth, KB/s                */
    __u64 bw_write_kbs;   /* write bandwidth, KB/s                */
    __u64 bw_copy_kbs;    /* copy  bandwidth, KB/s                */
};

#define TUXBENCH_MAGIC   'T'
#define TUXBENCH_IOC_RUN _IOWR(TUXBENCH_MAGIC, 1, struct tuxbench_req)

#endif /* TUXBENCH_H */
