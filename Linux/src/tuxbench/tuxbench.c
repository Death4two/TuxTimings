// SPDX-License-Identifier: GPL-2.0
/*
 * tuxbench.c — kernel-mode memory latency and bandwidth benchmark
 *
 * Advantages over the userspace bench.c path:
 *
 *   wbinvd          — single instruction flushes the entire cache hierarchy
 *                     on every logical CPU (broadcast via IPI), including AMD
 *                     3D V-Cache.  Userspace must iterate clflushopt over
 *                     every cache line individually.
 *
 *   alloc_pages_node — guaranteed physically contiguous pages on the local
 *                      NUMA node.  mmap+MADV_HUGEPAGE is advisory; the kernel
 *                      may silently fall back to 4 KB pages under memory
 *                      pressure.
 *
 *   kthread_bind     — hard CPU affinity; CFS cannot migrate or preempt a
 *                      kthread running at SCHED_FIFO inside the kernel.
 *
 *   no syscall overhead — each bandwidth pass runs entirely in kernel space;
 *                         no user/kernel transitions between timed intervals.
 *
 * Interface:
 *   open /dev/tuxbench
 *   ioctl(fd, TUXBENCH_IOC_RUN, &req)   // fills req with results
 *   close fd
 *
 * The char device is created at module load; no udev rule needed (uses
 * device_create so it appears automatically under /dev).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/numa.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/sort.h>
#include <linux/topology.h>
#include <asm/special_insns.h>
#include <asm/fpu/api.h>
#include <asm/cpufeature.h>

#include "tuxbench.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TuxTimings");
MODULE_DESCRIPTION("Kernel-mode memory latency and bandwidth benchmark");
MODULE_VERSION("0.4");

/* AVX2 vector type — 4×u64 = 256 bits, unaligned-safe */
typedef unsigned long long v4u64 __attribute__((vector_size(32), aligned(1)));

static bool tb_avx2;

/* ── Constants ──────────────────────────────────────────────────────────── */

#define DEVICE_NAME     "tuxbench"
#define CLASS_NAME      "tuxbench"

/* Latency: one pointer-chase node per cache line */
#define CACHELINE       64
#define LAT_SAMPLES_MIN 5          /* minimum samples before CV² check */
#define LAT_SAMPLES_MAX 32         /* hard cap                         */

/* Bandwidth */
#define BW_PASSES_MAX   64

/* Buffer sizing — override via module params */
static ulong bw_buf_mb = 512;
module_param(bw_buf_mb, ulong, 0444);
MODULE_PARM_DESC(bw_buf_mb, "Bandwidth buffer size in MB per thread (default 512)");

/* ── Char device globals ─────────────────────────────────────────────── */

static dev_t         tb_dev;
static struct cdev   tb_cdev;
static struct class *tb_class;

/* ── Page allocation helpers ─────────────────────────────────────────── */

/*
 * Allocate bytes of virtually-contiguous, NUMA-local memory backed by
 * huge pages where possible.
 *
 * vmalloc_huge uses 2 MB pages on x86-64, eliminating TLB pressure for large
 * latency and bandwidth buffers.  Without huge pages a 48 MB L3 buffer needs
 * 12,288 × 4 KB page-table entries — far exceeding TLB capacity — and every
 * random pointer-chase access adds a page-walk penalty that inflates the
 * apparent latency by ~10 ns.  With 2 MB pages the same buffer needs 24
 * entries, which fit comfortably in the L2 TLB.
 *
 * Falls back to vmalloc_node if vmalloc_huge is unavailable or fails.
 */
static void *tb_alloc_node(size_t bytes, int node)
{
    void *p;

    /* vmalloc_huge: added in 5.15, uses PMD-level (2 MB) mappings */
    p = vmalloc_huge(bytes, GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
    if (!p)
        p = vmalloc_node(bytes, node);
    if (p)
        memset(p, 0, bytes);
    return p;
}

static void tb_free(void *p, size_t bytes)
{
    (void)bytes;
    vfree(p);
}

/* ── Full cache flush ─────────────────────────────────────────────────── */

/*
 * wbinvd_on_all_cpus() writes back and invalidates all cache lines on every
 * logical CPU via IPI.  This is the kernel equivalent of iterating clflushopt
 * over every line manually — but it flushes in one shot including 3D V-Cache.
 */
static void tb_flush_all(void)
{
    wbinvd_on_all_cpus();
}

/* ── PRNG (xorshift64, local state) ──────────────────────────────────── */

static u64 xorshift64(u64 *s)
{
    u64 x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*s = x);
}

static void tb_shuffle(size_t *arr, size_t n, u64 *rng)
{
    size_t i;
    for (i = n - 1; i > 0; i--) {
        u64 range = (u64)(i + 1);
        u64 threshold = (-range) % range;
        u64 r;
        do { r = xorshift64(rng); } while (r < threshold);
        size_t j = (size_t)(r % range);
        size_t tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

/* ── Latency measurement ─────────────────────────────────────────────── */

typedef struct tb_node {
    struct tb_node *next;
    char pad[CACHELINE - sizeof(void *)];
} tb_node_t;

static int cmp_u64(const void *a, const void *b)
{
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return (x > y) - (x < y);
}

/*
 * Measure pointer-chase latency for a buffer of buf_bytes.
 *
 * min_iters: minimum total pointer hops per sample (like userspace bench.c).
 *   L1: 200000000, L2: 50000000, L3: 5000000, DRAM: 1000000
 *   More iters = later samples hit warm cache = measures cache-resident latency.
 *
 * flush_each: if non-zero, call wbinvd before every sample (cold access).
 *   Use 1 only for DRAM (to ensure data is never cached).
 *   L1/L2/L3 use 0 — data warms up in the cache after the first traversal,
 *   and the median of LAT_SAMPLES then reflects the true cache-resident latency,
 *   matching what the userspace benchmark reports.
 *
 * Returns latency in picoseconds.
 */
static u64 tb_measure_latency_ps(size_t buf_bytes, long long min_iters,
                                  int flush_each, int node)
{
    size_t n_nodes = buf_bytes / sizeof(tb_node_t);
    tb_node_t *nodes;
    size_t *indices;
    u64 samples[LAT_SAMPLES_MAX];
    u64 rng;
    int s, n;

    if (n_nodes < 2)
        return 0;

    nodes = tb_alloc_node(buf_bytes, node);
    if (!nodes)
        return 0;

    indices = kvmalloc_array(n_nodes, sizeof(size_t), GFP_KERNEL);
    if (!indices) {
        tb_free(nodes, buf_bytes);
        return 0;
    }

    rng = (u64)(uintptr_t)nodes ^ (u64)ktime_get_ns();

    sched_set_fifo(current);

    /* Build the random permutation once.  All samples chase the same chain
     * so DRAM row-hit patterns are identical across samples, eliminating
     * the inter-sample variance caused by different access patterns. */
    {
        size_t i;
        for (i = 0; i < n_nodes; i++) indices[i] = i;
        tb_shuffle(indices, n_nodes, &rng);
        for (i = 0; i < n_nodes - 1; i++)
            nodes[indices[i]].next = &nodes[indices[i + 1]];
        nodes[indices[n_nodes - 1]].next = &nodes[indices[0]];
    }

    /* Warm-up pass: one full traversal before any timing begins.
     * Ensures the chain is resident in the target cache level so that
     * sample[0] does not include cold-miss (DRAM-speed) accesses. */
    {
        tb_node_t *p = &nodes[0];
        size_t j;
        for (j = 0; j < n_nodes; j++)
            p = p->next;
        WRITE_ONCE(nodes[0].pad[0], (char)(uintptr_t)p);
    }

    n = 0;
    for (s = 0; s < LAT_SAMPLES_MAX; s++) {
        tb_node_t *p;
        u64 t0, t1, iters;
        long long elapsed_ns;

        if (flush_each)
            tb_flush_all();

        iters = ((u64)min_iters + n_nodes - 1) / n_nodes;
        if (iters < 1) iters = 1;

        p = &nodes[0];
        migrate_disable();
        t0 = ktime_get_ns();
        {
            u64 k;
            for (k = 0; k < iters; k++) {
                size_t j;
                for (j = 0; j < n_nodes; j++)
                    p = p->next;
            }
        }
        t1 = ktime_get_ns();
        migrate_enable();

        /* prevent optimising away the pointer chase */
        WRITE_ONCE(nodes[0].pad[0], (char)(uintptr_t)p);

        elapsed_ns = (long long)(t1 - t0);
        samples[s] = (u64)elapsed_ns * 1000ULL / (iters * (u64)n_nodes);
        n++;

        if (n < LAT_SAMPLES_MIN) continue;

        /* CV² < 0.0001  ↔  var*10000 < mean²  (same criterion as bandwidth) */
        {
            s64 isum = 0;
            u64 imean, ivar, imean2;
            int i;

            for (i = 0; i < n; i++) isum += (s64)samples[i];
            imean = (u64)(isum / n);

            ivar = 0;
            for (i = 0; i < n; i++) {
                s64 d = (s64)samples[i] - (s64)imean;
                ivar += (u64)(d * d);
            }
            if (n > 1) ivar /= (u64)(n - 1);

            imean2 = imean * imean;
            if (ivar * 10000ULL < imean2)
                break;
        }
    }

    sched_set_normal(current, 0);

    kvfree(indices);
    tb_free(nodes, buf_bytes);

    sort(samples, n, sizeof(u64), cmp_u64, NULL);
    return samples[n / 2];
}

/* ── Cache sizing via sysfs ───────────────────────────────────────────── */

/*
 * Read a decimal number from the start of a sysfs buffer, stopping at the
 * first non-digit character (handles "32768K\n", "32768\n", etc.).
 */
static unsigned long tb_parse_leading_ulong(const char *buf)
{
    unsigned long val = 0;
    while (*buf >= '0' && *buf <= '9') {
        val = val * 10 + (*buf - '0');
        buf++;
    }
    return val;
}

/*
 * Read a sysfs file into buf.  Returns number of bytes read, or 0 on error.
 */
static int tb_sysfs_read(const char *path, char *buf, size_t bufsz)
{
    struct file *f;
    loff_t pos = 0;
    ssize_t n;

    f = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(f))
        return 0;
    n = kernel_read(f, buf, bufsz - 1, &pos);
    filp_close(f, NULL);
    if (n > 0)
        buf[n] = '\0';
    return (n > 0) ? (int)n : 0;
}

/*
 * Read a sysfs cache size file (e.g. index0/size).
 * sysfs reports in kB with optional "K" suffix: "32768K\n" or "32768\n".
 * Returns size in bytes, or 0 on failure.
 */
static size_t tb_read_cache_size(int index)
{
    char path[128], buf[32] = {0};
    unsigned long kb;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu0/cache/index%d/size", index);
    if (!tb_sysfs_read(path, buf, sizeof(buf)))
        return 0;
    kb = tb_parse_leading_ulong(buf);
    return kb * 1024;
}

/*
 * Sum L3 sizes across all unique cache instances (one per CCD on dual-CCD CPUs).
 * Mirrors the dram_buf_bytes() logic in userspace bench.c.
 */
static size_t tb_total_l3(void)
{
    size_t total = 0;
    int cpu;
    int max_cpus = num_possible_cpus();

    for (cpu = 0; cpu < max_cpus; cpu++) {
        char path[128], buf[64] = {0};
        unsigned long kb;
        int first_cpu;

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index3/size", cpu);
        if (!tb_sysfs_read(path, buf, sizeof(buf)))
            continue;
        kb = tb_parse_leading_ulong(buf);
        if (kb == 0)
            continue;

        /* shared_cpu_list — only count first CPU in each shared group */
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index3/shared_cpu_list", cpu);
        if (tb_sysfs_read(path, buf, sizeof(buf))) {
            /* parse first number: "0-15\n" or "0,8\n" → 0 */
            first_cpu = (int)tb_parse_leading_ulong(buf);
            if (first_cpu != cpu)
                continue;
        }

        total += kb * 1024;
    }
    return total;
}

static void tb_detect_lat_sizes(size_t *l1, size_t *l2, size_t *l3)
{
    /*
     * index0=L1d, index2=L2, index3=L3.
     * For L3 use the size reported for cpu0 (one CCD), not the total across
     * all CCDs.  On a 9950X3D each CCD has 96 MB L3; summing both gives 192 MB
     * which would produce a buffer that spills into DRAM, measuring DRAM
     * latency instead of L3.  Halving one CCD's L3 (48 MB) keeps the access
     * pattern comfortably inside the L3/V-Cache hierarchy.
     */
    size_t sl1d = tb_read_cache_size(0);
    size_t sl2  = tb_read_cache_size(2);
    size_t sl3  = tb_read_cache_size(3); /* one CCD only */

    if (sl1d == 0) sl1d = 32  * 1024;
    if (sl2  == 0) sl2  = 512 * 1024;
    if (sl3  == 0) sl3  = 32  * 1024 * 1024;

    *l1 = (sl1d * 3) / 4;
    *l2 = sl2 / 2;
    *l3 = sl3 / 2;
    if (*l2 <= *l1) *l2 = *l1 * 4;
    if (*l3 <= *l2) *l3 = *l2 * 4;
}

/* ── Per-thread bandwidth state ──────────────────────────────────────── */

struct bw_thread {
    /* inputs */
    u64    *buf_a;        /* read / write / copy dst   */
    u64    *buf_b;        /* copy src                  */
    size_t  n64;          /* elements                  */
    int     op;           /* 0=read 1=write 2=copy     */

    /* synchronisation */
    struct completion  ready;   /* thread signals it has started */
    struct completion  go;      /* coordinator signals start     */
    struct completion  done;    /* thread signals completion     */

    /* output */
    u64 elapsed_ns;
};

/* ── Bandwidth kernels using movnti (non-temporal stores) ────────────── */

/*
 * movnti bypasses the cache and writes directly to DRAM write-combining
 * buffers — the kernel equivalent of _mm256_stream_si256 in userspace.
 * Without NT stores, regular cached stores trigger write-allocate (RFO):
 * the CPU reads the cache line from DRAM first, then modifies it in cache.
 * The timed section then finishes before data is ever written back to DRAM,
 * giving a falsely high "write bandwidth" that reflects cache speed.
 *
 * 8× unroll fills all AMD WC buffers per iteration (8 × 64B = 512B).
 */

/*
 * Streaming read prefetch strategy:
 *
 * prefetchnta (locality=0) uses the non-temporal path — avoids polluting
 * L1/L2 with the large benchmark buffer, and on AMD sustains higher
 * streaming throughput than prefetcht0.
 *
 * Three staggered distances keep the memory request queue full:
 *   PF_A = 64 lines (~early fill)
 *   PF_B = 96 lines (~mid)
 *   PF_C = 128 lines (~far, matches Little's Law headroom)
 *
 * Each iteration covers 8 u64s = 1 cache line, so distances are in
 * units of (N * 8) u64 offsets.
 */

static void do_read_kernel(u64 *a, size_t n)
{
    u64 sink;

    if (tb_avx2) {
        /*
         * AVX2 path: 8 independent 256-bit accumulators, 256 bytes per
         * iteration.  kernel_fpu_begin/end saves/restores YMM state.
         * GCC vector extensions compile to vmovdqu + vpxor without
         * requiring immintrin.h.
         */
        const char *p    = (const char *)a;
        const char *stop = p + ((n * sizeof(u64)) & ~255UL);
        v4u64 s0 = {}, s1 = {}, s2 = {}, s3 = {},
              s4 = {}, s5 = {}, s6 = {}, s7 = {};

        kernel_fpu_begin();
        for (; p < stop; p += 256) {
            s0 ^= *(const v4u64 *)(p +   0);
            s1 ^= *(const v4u64 *)(p +  32);
            s2 ^= *(const v4u64 *)(p +  64);
            s3 ^= *(const v4u64 *)(p +  96);
            s4 ^= *(const v4u64 *)(p + 128);
            s5 ^= *(const v4u64 *)(p + 160);
            s6 ^= *(const v4u64 *)(p + 192);
            s7 ^= *(const v4u64 *)(p + 224);
        }
        s0 ^= s1; s2 ^= s3; s4 ^= s5; s6 ^= s7;
        s0 ^= s2; s4 ^= s6; s0 ^= s4;
        sink = s0[0] ^ s0[1] ^ s0[2] ^ s0[3];
        kernel_fpu_end();
    } else {
        /* Scalar fallback for CPUs without AVX2 */
        u64 s0 = 0, s1 = 0, s2 = 0, s3 = 0,
            s4 = 0, s5 = 0, s6 = 0, s7 = 0;
        size_t i;
        for (i = 0; i + 7 < n; i += 8) {
            s0 ^= a[i+0]; s1 ^= a[i+1]; s2 ^= a[i+2]; s3 ^= a[i+3];
            s4 ^= a[i+4]; s5 ^= a[i+5]; s6 ^= a[i+6]; s7 ^= a[i+7];
        }
        sink = s0 ^ s1 ^ s2 ^ s3 ^ s4 ^ s5 ^ s6 ^ s7;
    }

    WRITE_ONCE(*(u64 *)a, sink);
}

static void do_write_kernel(u64 *a, size_t n)
{
    size_t i;
    const u64 zero = 0;

    for (i = 0; i + 7 < n; i += 8) {
        asm volatile("movnti %1, %0" : "=m"(a[i+0]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+1]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+2]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+3]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+4]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+5]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+6]) : "r"(zero));
        asm volatile("movnti %1, %0" : "=m"(a[i+7]) : "r"(zero));
    }
    asm volatile("sfence" ::: "memory");
}

static void do_copy_kernel(u64 *dst, const u64 *src, size_t n)
{
    size_t bytes = n * sizeof(u64);
    asm volatile("rep movsb"
                 : "+D"(dst), "+S"(src), "+c"(bytes)
                 :: "memory");
}

/* ── kthread worker ──────────────────────────────────────────────────── */

static int bw_thread_fn(void *arg)
{
    struct bw_thread *t = arg;
    u64 t0, t1;

    /* signal ready, wait for coordinator go */
    complete(&t->ready);
    wait_for_completion(&t->go);

    t0 = ktime_get_ns();
    switch (t->op) {
    case 0: do_read_kernel (t->buf_a,           t->n64); break;
    case 1: do_write_kernel(t->buf_a,           t->n64); break;
    case 2: do_copy_kernel (t->buf_a, t->buf_b, t->n64); break;
    }
    t1 = ktime_get_ns();

    t->elapsed_ns = t1 - t0;
    complete(&t->done);
    return 0;
}

/* ── Bandwidth measurement ───────────────────────────────────────────── */

/*
 * Run one bandwidth pass (op: 0=read 1=write 2=copy) across all online
 * physical cores.  Returns bandwidth in KB/s.
 *
 * Strategy:
 *   - one kthread per online CPU (pinned via kthread_bind)
 *   - coordinator calls wbinvd_on_all_cpus() before each pass
 *   - all threads started simultaneously via completion; elapsed = max(elapsed)
 *   - repeat until CV² < BW_CV2_TARGET or BW_PASSES_MAX
 */
static u64 tb_measure_bw(int op, int node)
{
    cpumask_var_t phys_mask;
    const cpumask_t *node_mask;
    bool phys_mask_alloc = false;
    int ncpus;
    size_t buf_bytes;
    u64 samples[BW_PASSES_MAX];
    int npass, n;
    int cpu_idx;
    int cpu;
    struct bw_thread *threads;
    struct task_struct **tasks;
    u64 **bufs_a, **bufs_b;
    u64 result = 0;

    /*
     * One thread per physical core — skip HT siblings.
     * topology_sibling_cpumask() gives the HT set; keep only the
     * lowest-numbered sibling per core.  Matches userspace build_cpu_list()
     * and avoids overcounting bytes_total when SMT is enabled.
     */
    if (zalloc_cpumask_var(&phys_mask, GFP_KERNEL)) {
        phys_mask_alloc = true;
        for_each_cpu(cpu, cpumask_of_node(node)) {
            const cpumask_t *siblings = topology_sibling_cpumask(cpu);
            if (cpu == cpumask_first(siblings))
                cpumask_set_cpu(cpu, phys_mask);
        }
        node_mask = phys_mask;
    } else {
        node_mask = cpumask_of_node(node);
    }

    ncpus = cpumask_weight(node_mask);
    if (ncpus <= 0)
        ncpus = num_online_cpus();

    buf_bytes = (size_t)bw_buf_mb << 20;

    threads = kcalloc(ncpus, sizeof(*threads), GFP_KERNEL);
    tasks   = kcalloc(ncpus, sizeof(*tasks),   GFP_KERNEL);
    bufs_a  = kcalloc(ncpus, sizeof(*bufs_a),  GFP_KERNEL);
    bufs_b  = kcalloc(ncpus, sizeof(*bufs_b),  GFP_KERNEL);

    if (!threads || !tasks || !bufs_a || !bufs_b)
        goto cleanup_meta;

    /* allocate per-thread buffers */
    cpu_idx = 0;
    for_each_cpu(cpu, node_mask) {
        if (cpu_idx >= ncpus) break;
        bufs_a[cpu_idx] = tb_alloc_node(buf_bytes, node);
        if (!bufs_a[cpu_idx]) goto cleanup_bufs;
        memset(bufs_a[cpu_idx], 0, buf_bytes);
        if (op == 2) {
            bufs_b[cpu_idx] = tb_alloc_node(buf_bytes, node);
            if (!bufs_b[cpu_idx]) goto cleanup_bufs;
            memset(bufs_b[cpu_idx], 1, buf_bytes);
        }
        cpu_idx++;
    }

    for (npass = 0; npass < BW_PASSES_MAX; npass++) {
        u64 max_elapsed = 0;
        size_t bytes_total;
        u64 bw_kbs;
        int i;

        /* global cache flush before each pass */
        wbinvd_on_all_cpus();

        /* launch threads */
        cpu_idx = 0;
        for_each_cpu(cpu, node_mask) {
            struct bw_thread *t;
            struct task_struct *task;
            if (cpu_idx >= ncpus) break;

            t = &threads[cpu_idx];
            t->buf_a         = bufs_a[cpu_idx];
            t->buf_b         = (op == 2) ? bufs_b[cpu_idx] : NULL;
            t->n64           = buf_bytes / sizeof(u64);
            t->op            = op;
            init_completion(&t->ready);
            init_completion(&t->go);
            init_completion(&t->done);

            task = kthread_create(bw_thread_fn, t, "tuxbench/%d", cpu);
            if (IS_ERR(task)) goto cleanup_threads;
            tasks[cpu_idx] = task;
            kthread_bind(task, cpu);
            sched_set_fifo(task);
            wake_up_process(task);
            cpu_idx++;
        }

        /* wait for all threads ready, then release */
        for (i = 0; i < cpu_idx; i++)
            wait_for_completion(&threads[i].ready);
        for (i = 0; i < cpu_idx; i++)
            complete(&threads[i].go);

        /* wait for completion, find max elapsed */
        for (i = 0; i < cpu_idx; i++) {
            wait_for_completion(&threads[i].done);
            if (threads[i].elapsed_ns > max_elapsed)
                max_elapsed = threads[i].elapsed_ns;
        }

        if (max_elapsed == 0) continue;

        /*
         * bw_kbs = (bytes_total / 1024) * 1e9 / elapsed_ns
         *
         * Dividing bytes→KB first keeps the intermediate value well within
         * u64 range:
         *   32 cores × 512 MB × 2 (copy) = 32 GB → 33,554,432 KB
         *   33,554,432 × 1,000,000,000 = 3.36e16 < u64_max (1.84e19) ✓
         */
        bytes_total = (size_t)ncpus * buf_bytes * (op == 2 ? 2 : 1);
        {
            u64 kb_total = (u64)bytes_total / 1024ULL;
            u64 elapsed  = max_elapsed ? max_elapsed : 1;
            bw_kbs = kb_total * 1000000000ULL / elapsed;
        }

        samples[npass] = bw_kbs;

        if (npass < 4) continue; /* need at least 5 samples */

        /*
         * Integer CV² check: CV² < 0.0001  ↔  var*10000 < mean²
         * Using s64 to handle (sample - mean) differences safely.
         * mean and samples are in KB/s (~70,000,000); mean² ~ 4.9e15 — fits u64.
         * var/n ~ (700,000)²/64 ~ 7.6e9; *10000 ~ 7.6e13 — fits u64.
         */
        {
            s64 isum = 0;
            u64 imean, ivar, imean2;
            int nn = npass + 1;

            for (i = 0; i < nn; i++) isum += (s64)samples[i];
            imean = (u64)(isum / nn);

            ivar = 0;
            for (i = 0; i < nn; i++) {
                s64 d = (s64)samples[i] - (s64)imean;
                ivar += (u64)(d * d);
            }
            /* Bessel: divide by n-1 = npass */
            if (npass > 0) ivar /= (u64)npass;

            imean2 = imean * imean;
            /* CV² < 0.0001  →  ivar * 10000 < imean² */
            if (ivar * 10000ULL < imean2)
                break;
        }

        continue;

cleanup_threads:
        /* stop any threads already started this pass */
        for (i = 0; i < cpu_idx; i++) {
            if (tasks[i] && !IS_ERR(tasks[i])) {
                complete(&threads[i].go); /* unblock */
                wait_for_completion(&threads[i].done);
            }
        }
        goto cleanup_bufs;
    }

    n = (npass < BW_PASSES_MAX) ? npass + 1 : BW_PASSES_MAX;
    {
        u64 tmp[BW_PASSES_MAX];
        memcpy(tmp, samples, n * sizeof(u64));
        sort(tmp, n, sizeof(u64), cmp_u64, NULL);
        result = tmp[n / 2];
    }

cleanup_bufs:
    for (cpu_idx = 0; cpu_idx < ncpus; cpu_idx++) {
        if (bufs_a[cpu_idx]) tb_free(bufs_a[cpu_idx], buf_bytes);
        if (bufs_b[cpu_idx]) tb_free(bufs_b[cpu_idx], buf_bytes);
    }
cleanup_meta:
    kfree(threads);
    kfree(tasks);
    kfree(bufs_a);
    kfree(bufs_b);
    if (phys_mask_alloc)
        free_cpumask_var(phys_mask);
    return result;
}

/* ── DRAM buffer size ────────────────────────────────────────────────── */

static size_t tb_dram_buf_bytes(void)
{
    size_t total_l3 = tb_total_l3();
    size_t target;

    if (total_l3 == 0)
        total_l3 = 32UL * 1024 * 1024;

    target = total_l3 * 4;
    if (target < 512UL * 1024 * 1024)
        target = 512UL * 1024 * 1024;
    return target;
}

/* ── ioctl handler ────────────────────────────────────────────────────── */

static long tb_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct tuxbench_req req;
    int node;

    if (cmd != TUXBENCH_IOC_RUN)
        return -ENOTTY;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    node = numa_node_id();
    /* zero only the result fields, preserving flags */
    req.lat_l1_ps = req.lat_l2_ps = req.lat_l3_ps = req.lat_dram_ps = 0;
    req.bw_read_kbs = req.bw_write_kbs = req.bw_copy_kbs = 0;

    if (req.flags & TUXBENCH_FL_LAT) {
        size_t l1, l2, l3;
        size_t dram_buf;

        tb_detect_lat_sizes(&l1, &l2, &l3);
        dram_buf = tb_dram_buf_bytes();

        req.lat_l1_ps   = tb_measure_latency_ps(l1,       200000000LL, 0, node);
        req.lat_l2_ps   = tb_measure_latency_ps(l2,        50000000LL, 0, node);
        req.lat_l3_ps   = tb_measure_latency_ps(l3,        20000000LL, 0, node);
        req.lat_dram_ps = tb_measure_latency_ps(dram_buf,   1000000LL, 1, node);
    }

    if (req.flags & TUXBENCH_FL_BW) {
        req.bw_read_kbs  = tb_measure_bw(0, node);
        req.bw_write_kbs = tb_measure_bw(1, node);
        req.bw_copy_kbs  = tb_measure_bw(2, node);
    }

    if (copy_to_user((void __user *)arg, &req, sizeof(req)))
        return -EFAULT;

    return 0;
}

/* ── File operations ─────────────────────────────────────────────────── */

static int tb_open(struct inode *i, struct file *f)  { return 0; }
static int tb_release(struct inode *i, struct file *f) { return 0; }

static const struct file_operations tb_fops = {
    .owner          = THIS_MODULE,
    .open           = tb_open,
    .release        = tb_release,
    .unlocked_ioctl = tb_ioctl,
};

/* ── Module init / exit ──────────────────────────────────────────────── */

static int __init tuxbench_init(void)
{
    int ret;
    struct device *dev;

    ret = alloc_chrdev_region(&tb_dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("tuxbench: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&tb_cdev, &tb_fops);
    ret = cdev_add(&tb_cdev, tb_dev, 1);
    if (ret < 0) {
        pr_err("tuxbench: cdev_add failed: %d\n", ret);
        goto err_region;
    }

    tb_class = class_create(CLASS_NAME);
    if (IS_ERR(tb_class)) {
        ret = PTR_ERR(tb_class);
        pr_err("tuxbench: class_create failed: %d\n", ret);
        goto err_cdev;
    }

    dev = device_create(tb_class, NULL, tb_dev, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        pr_err("tuxbench: device_create failed: %d\n", ret);
        goto err_class;
    }

    tb_avx2 = boot_cpu_has(X86_FEATURE_AVX2);
    pr_info("tuxbench: ready at /dev/%s (major %d)\n",
            DEVICE_NAME, MAJOR(tb_dev));
    pr_info("tuxbench: read path = %s\n",
            tb_avx2 ? "AVX2 (8x256-bit accumulators)" : "scalar (8x64-bit accumulators)");
    return 0;

err_class:
    class_destroy(tb_class);
err_cdev:
    cdev_del(&tb_cdev);
err_region:
    unregister_chrdev_region(tb_dev, 1);
    return ret;
}

static void __exit tuxbench_exit(void)
{
    device_destroy(tb_class, tb_dev);
    class_destroy(tb_class);
    cdev_del(&tb_cdev);
    unregister_chrdev_region(tb_dev, 1);
    pr_info("tuxbench: unloaded\n");
}

module_init(tuxbench_init);
module_exit(tuxbench_exit);
