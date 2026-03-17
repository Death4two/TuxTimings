/*
 * bench.c — Cache latency and DRAM bandwidth benchmark
 *
 * Latency : single-threaded random pointer chasing (one node per cache line).
 *           Median of LAT_SAMPLES independent timed traversals.
 *
 * Bandwidth: multi-threaded, one pthread per physical core, each thread
 *            working on its own contiguous buffer chunk.  A barrier
 *            synchronises all threads at the start and end of each timed
 *            pass so the wall-clock window covers the full parallel work.
 *            All buffers are clflushopt-evicted before each timed pass to
 *            defeat V-Cache / hardware prefetcher staging and ensure cold
 *            DRAM reads.
 */

#define _GNU_SOURCE
#include "bench.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include "tuxbench/tuxbench.h"

/* ── Timing ──────────────────────────────────────────────────────────── */

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ── Huge-page-aware allocator ───────────────────────────────────────── */
/*
 * Large buffers (≥2 MB) are mmap'd with MADV_HUGEPAGE so the kernel can
 * back them with 2 MB THP pages instead of 4 KB pages.
 *
 * Why this matters for bandwidth:
 *   A 512 MB buffer has 131,072 normal pages → 131,072 TLB misses per sweep.
 *   With 2 MB pages that drops to 256 TLB entries — the L1 TLB (typically
 *   64 entries) still misses but the L2 TLB (1 500–2 048 entries) covers it
 *   almost entirely.  Eliminating TLB pressure means the CPU spends its
 *   memory-access budget on bandwidth, not page-table walks.
 */
#define HUGEPAGE_THRESH (2UL * 1024 * 1024)

static void *bench_alloc(size_t bytes)
{
    if (bytes >= HUGEPAGE_THRESH) {
        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            madvise(p, bytes, MADV_HUGEPAGE);
            return p;
        }
    }
    return aligned_alloc(64, (bytes + 63) & ~(size_t)63);
}

static void bench_free(void *p, size_t bytes)
{
    if (bytes >= HUGEPAGE_THRESH)
        munmap(p, bytes);
    else
        free(p);
}

/* ── Latency: random pointer chasing ─────────────────────────────────── */

/*
 * Each node occupies one cache line (64 bytes). The traversal order is a
 * random permutation so hardware prefetchers cannot predict the next address.
 * Latency per access = elapsed / (passes × n_nodes).
 */
#define CACHELINE 64

typedef struct node {
    struct node *next;
    char pad[CACHELINE - sizeof(void *)];
} node_t;

/*
 * Why rand() + modulo is wrong here:
 *
 *   1. GLOBAL STATE / NOT THREAD-SAFE
 *      rand() reads and writes a single global seed variable.  On glibc it
 *      is protected by a hidden lock, which means:
 *        a) Any concurrent caller (another benchmark thread, a future change)
 *           will race on that lock — contention for a lock that buys nothing.
 *        b) The sequence cannot be replayed: saving the local state is
 *           impossible because the state lives in the C library's internals.
 *
 *   2. MODULO BIAS
 *      rand() returns values in [0, RAND_MAX] where RAND_MAX is typically
 *      2^31 - 1.  Computing rand() % n gives a uniform distribution only if
 *      (RAND_MAX + 1) is divisible by n.  Otherwise the values in
 *      [0, (RAND_MAX + 1) % n) appear once more often than the rest — a
 *      systematic bias that skews the pointer-chain permutation so a tiny
 *      subset of node orderings is slightly over-represented.
 *
 * Fix: xorshift64 with a caller-supplied state (zero allocation, no locks,
 * fully reproducible) + rejection sampling (arc4random_uniform approach) to
 * produce a perfectly uniform distribution over any range.
 */
static uint64_t xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >>  7;
    x ^= x << 17;
    return (*s = x);
}

static void shuffle(size_t *arr, size_t n, uint64_t *rng)
{
    for (size_t i = n - 1; i > 0; i--) {
        uint64_t range = (uint64_t)(i + 1);
        /*
         * Rejection sampling: 2^64 is not always divisible by `range`, so
         * the top (2^64 % range) raw values would be generated one extra
         * time.  Discard any draw that falls in that surplus bucket.
         * `threshold = (-range) % range` uses unsigned wrap-around to
         * compute (2^64 % range) without needing a 128-bit type.
         */
        uint64_t threshold = (-range) % range;
        uint64_t r;
        do { r = xorshift64(rng); } while (r < threshold);
        size_t j = (size_t)(r % range);
        size_t t = arr[i]; arr[i] = arr[j]; arr[j] = t;
    }
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

/* forward declaration — defined in the Cache flush section below */
static void flush_buffer(const void *ptr, size_t bytes);

/*
 * Collect `nsamples` independent timed measurements and return the median.
 *
 * Why median instead of minimum:
 *   Minimum can be spuriously low if the timing syscall itself gets
 *   scheduled immediately after a context switch that happened to warm
 *   the TLB — giving an unrealistically fast result.  Median is robust
 *   to both high outliers (OS interrupts) and low outliers (lucky runs),
 *   and closely matches what tools like mlc and membench report.
 *
 * DRAM uses nsamples=3: each sample already traverses millions of nodes
 * (~380 ms per sample), so 3 is sufficient and keeps total time ~1.5 s.
 * L1/L2/L3 use LAT_SAMPLES=9 since their per-sample duration is shorter.
 */
#define LAT_SAMPLES 9

/*
 * flush_each=1: clflushopt every node before each timed sample.
 * Required for DRAM latency — without it the warm-up pass (and previous
 * samples) leave nodes resident in L3/V-Cache, making what should be a DRAM
 * measurement look suspiciously fast.  For L1/L2/L3 the buffer already fits
 * inside the target cache level, so flushing would defeat the purpose.
 */
static double measure_latency_ns(size_t buf_bytes, long long min_accesses,
                                  int nsamples, int flush_each)
{
    size_t n = buf_bytes / sizeof(node_t);
    if (n < 64) return 0.0;

    size_t alloc_bytes = n * sizeof(node_t);
    node_t *nodes = bench_alloc(alloc_bytes);
    if (!nodes) return 0.0;

    size_t *perm = malloc(n * sizeof(size_t));
    if (!perm) { bench_free(nodes, alloc_bytes); return 0.0; }

    /* Seed: mix buffer address (unique per alloc) with wall time so two
     * back-to-back calls never produce the same permutation. */
    uint64_t rng = (uint64_t)(uintptr_t)nodes ^ (uint64_t)time(NULL);
    if (!rng) rng = 0xdeadbeefcafe0001ULL;

    for (size_t i = 0; i < n; i++) perm[i] = i;
    shuffle(perm, n, &rng);
    for (size_t i = 0; i < n; i++)
        nodes[perm[i]].next = &nodes[perm[(i + 1) % n]];
    free(perm);

    long long passes = (min_accesses + (long long)n - 1) / (long long)n;
    if (passes < 1) passes = 1;

    /* Warm up: fault in all pages and prime caches */
    volatile node_t *p = nodes;
    for (size_t i = 0; i < n; i++) p = p->next;

    double samples[LAT_SAMPLES]; /* LAT_SAMPLES is the max; nsamples <= LAT_SAMPLES */
    for (int s = 0; s < nsamples; s++) {
        /* For DRAM: evict every node so the traversal truly goes to DRAM,
         * not a cache level warmed by the previous sample. */
        if (flush_each)
            flush_buffer(nodes, alloc_bytes);
        long long t0 = now_ns();
        for (long long k = 0; k < passes; k++)
            for (size_t i = 0; i < n; i++) p = p->next;
        long long t1 = now_ns();
        samples[s] = (double)(t1 - t0) / ((double)passes * (double)n);
    }

    bench_free(nodes, alloc_bytes);

    qsort(samples, nsamples, sizeof(double), cmp_double);
    return samples[nsamples / 2]; /* median */
}

/* ── Cache flush helper ───────────────────────────────────────────────── */
/*
 * Evict every cache line of [ptr, ptr+bytes) from the entire cache
 * hierarchy (L1/L2/L3/V-Cache) using clflushopt, then sfence to wait
 * for all flushes to complete. This forces the next read to go to DRAM.
 *
 * clflushopt is non-blocking (unlike clflush) so we issue all flushes
 * first, then a single sfence rather than serialising each one.
 */
static void flush_buffer(const void *ptr, size_t bytes)
{
    const char *p   = (const char *)ptr;
    const char *end = p + bytes;
    for (; p < end; p += 64)
        _mm_clflushopt((void *)p);
    _mm_sfence();
}

/* ── Multi-threaded bandwidth ─────────────────────────────────────────── */

#define BW_MIN_PASSES  5          /* always run at least this many passes   */
#define BW_MAX_PASSES  64         /* safety cap to bound worst-case runtime */
#define BW_CV_TARGET   0.01       /* stop when stddev/mean < 1%             */
#define MAX_THREADS    256

typedef enum { OP_READ, OP_WRITE, OP_COPY, OP_EXIT } bw_op_t;

typedef struct {
    uint64_t          *buf_a;      /* read src / write dst / copy dst     */
    uint64_t          *buf_b;      /* copy src                            */
    size_t             n;          /* uint64_t elements for this thread   */
    int                cpu;        /* logical CPU to pin to               */
    bw_op_t            op;         /* operation to perform each pass      */
    pthread_barrier_t *bar_start;  /* barrier: main releases workers      */
    pthread_barrier_t *bar_end;    /* barrier: workers signal done        */
} bw_arg_t;

/*
 * Read inner loop: 8 independent AVX2 YMM accumulators, each covering 4
 * uint64_t (32 bytes).  One iteration loads 256 bytes (32 uint64_t) via 8
 * independent VPXOR chains, giving the out-of-order engine 8+ cache-miss
 * loads in flight simultaneously to saturate the memory-level parallelism
 * (MLP) needed to fill the DDR bus.
 *
 * Explicit AVX2 intrinsics are used instead of relying on auto-vectorisation
 * because the original 32-scalar-accumulator pattern can inhibit the
 * vectoriser — it sees 32 independent chains and may emit 32 separate scalar
 * loads rather than consolidating them into wider vector loads.  With 8 YMM
 * accumulators (256-bit each) the loop body is exactly 8 vmovdqu + 8 vpxor
 * instructions, which the CPU can issue at full bandwidth.
 */
/*
 * Software prefetch distance for DRAM streaming loops.
 *
 * Why NT stores saturate bandwidth but plain loads don't:
 *   _mm256_stream_si256 (NT store) writes to write-combining (WC) buffers
 *   that drain to DRAM asynchronously — the CPU never stalls waiting for a
 *   DRAM round-trip.  A plain _mm256_loadu_si256 generates a cache miss and
 *   the CPU must wait ~70–80 ns for DRAM before XOR can retire and the next
 *   load address becomes known.  8 in-flight YMM loads = 512 bytes in flight.
 *
 *   Little's Law: bytes_in_flight = bandwidth × latency
 *     70 GB/s × 80 ns ≈ 5,600 bytes = ~87 cache lines required to saturate.
 *   8 lines is ~11× short.
 *
 * Fix: _mm_prefetch (PREFETCHNTA) issued PF_DIST_U64 uint64_t ahead dispatches
 * DRAM requests long before they are needed, giving the memory controller time
 * to overlap many requests and fill the bus.  NTA hint (non-temporal): data is
 * brought to L1 without allocating in L2/L3, which is correct for single-pass
 * streaming over a buffer that far exceeds the L3.
 *
 * Distance calculation (DDR5 dual-channel, ~70–80 ns latency):
 *   Target in-flight: ~128 cache lines (8 192 bytes) — 1.5× the theoretical
 *   minimum to absorb burst latency variance.
 *   Read loop stride  = 32 uint64_t = 256 B = 4 CL  → 32 iters = 128 CL → PF_DIST = 1024
 *   Copy loop stride  = 16 uint64_t = 128 B = 2 CL  → 64 iters = 128 CL → PF_DIST = 1024
 */
#define PF_DIST_U64 1024  /* uint64_t ahead to prefetch (= 8 192 bytes = 128 CL) */

/* forward declarations — AVX-512 variants defined before do_write below */
#if defined(__GNUC__) || defined(__clang__)
static void do_read_avx512(const uint64_t *a, size_t n);
static void do_write_avx512(uint64_t *a, size_t n);
static void do_copy_avx512(uint64_t *dst, const uint64_t *src, size_t n);
#endif

__attribute__((optimize("O3,tree-vectorize")))
static void do_read(const uint64_t *a, size_t n)
{
#if (defined(__GNUC__) || defined(__clang__)) && defined(__AVX2__)
    if (__builtin_cpu_supports("avx512f")) {
        do_read_avx512(a, n);
        return;
    }
#endif
#if defined(__AVX2__)
    size_t n32 = n & ~(size_t)31; /* 32 uint64_t = 8 YMM per iteration */
    __m256i v0 = _mm256_setzero_si256();
    __m256i v1 = _mm256_setzero_si256();
    __m256i v2 = _mm256_setzero_si256();
    __m256i v3 = _mm256_setzero_si256();
    __m256i v4 = _mm256_setzero_si256();
    __m256i v5 = _mm256_setzero_si256();
    __m256i v6 = _mm256_setzero_si256();
    __m256i v7 = _mm256_setzero_si256();
    for (size_t i = 0; i < n32; i += 32) {
        /* 4 prefetch hints per iteration, each covering one 64-byte cache line,
         * issued PF_DIST_U64 elements (8 192 bytes) ahead of the current position.
         * This keeps ~128 DRAM requests outstanding so the memory bus stays full. */
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 +  0], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 +  8], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 16], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 24], _MM_HINT_NTA);
        v0 = _mm256_xor_si256(v0, _mm256_loadu_si256((const __m256i *)&a[i+ 0]));
        v1 = _mm256_xor_si256(v1, _mm256_loadu_si256((const __m256i *)&a[i+ 4]));
        v2 = _mm256_xor_si256(v2, _mm256_loadu_si256((const __m256i *)&a[i+ 8]));
        v3 = _mm256_xor_si256(v3, _mm256_loadu_si256((const __m256i *)&a[i+12]));
        v4 = _mm256_xor_si256(v4, _mm256_loadu_si256((const __m256i *)&a[i+16]));
        v5 = _mm256_xor_si256(v5, _mm256_loadu_si256((const __m256i *)&a[i+20]));
        v6 = _mm256_xor_si256(v6, _mm256_loadu_si256((const __m256i *)&a[i+24]));
        v7 = _mm256_xor_si256(v7, _mm256_loadu_si256((const __m256i *)&a[i+28]));
    }
    __m256i vsink = _mm256_xor_si256(
        _mm256_xor_si256(_mm256_xor_si256(v0, v1), _mm256_xor_si256(v2, v3)),
        _mm256_xor_si256(_mm256_xor_si256(v4, v5), _mm256_xor_si256(v6, v7)));
    __asm__ volatile("" : "+x"(vsink) :: "memory");
    /* scalar tail */
    uint64_t stail = 0;
    for (size_t i = n32; i < n; i++) stail ^= a[i];
    __asm__ volatile("" : "+r"(stail));
#else
    /* Fallback: 32 scalar accumulators */
    size_t n32 = n & ~(size_t)31;
    uint64_t s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,s12,s13,s14,s15,
             s16,s17,s18,s19,s20,s21,s22,s23,s24,s25,s26,s27,s28,s29,s30,s31,sink;
    s0=s1=s2=s3=s4=s5=s6=s7=s8=s9=s10=s11=s12=s13=s14=s15=
    s16=s17=s18=s19=s20=s21=s22=s23=s24=s25=s26=s27=s28=s29=s30=s31=0;
    for (size_t i = 0; i < n32; i += 32) {
        s0^=a[i];    s1^=a[i+1];  s2^=a[i+2];  s3^=a[i+3];
        s4^=a[i+4];  s5^=a[i+5];  s6^=a[i+6];  s7^=a[i+7];
        s8^=a[i+8];  s9^=a[i+9];  s10^=a[i+10]; s11^=a[i+11];
        s12^=a[i+12]; s13^=a[i+13]; s14^=a[i+14]; s15^=a[i+15];
        s16^=a[i+16]; s17^=a[i+17]; s18^=a[i+18]; s19^=a[i+19];
        s20^=a[i+20]; s21^=a[i+21]; s22^=a[i+22]; s23^=a[i+23];
        s24^=a[i+24]; s25^=a[i+25]; s26^=a[i+26]; s27^=a[i+27];
        s28^=a[i+28]; s29^=a[i+29]; s30^=a[i+30]; s31^=a[i+31];
    }
    sink = s0^s1^s2^s3^s4^s5^s6^s7^s8^s9^s10^s11^s12^s13^s14^s15^
           s16^s17^s18^s19^s20^s21^s22^s23^s24^s25^s26^s27^s28^s29^s30^s31;
    __asm__ volatile("" : "+r"(sink) :: "memory");
#endif
}

/* ── AVX-512 kernel variants (runtime-dispatched) ─────────────────────────
 *
 * __attribute__((target("avx512f"))) compiles each function with AVX-512
 * even when the TU is built with -mavx2 only.  __builtin_cpu_supports() in
 * the callers selects the right path at runtime so the binary still runs on
 * non-AVX-512 CPUs without SIGILL.
 *
 * Why AVX-512 NT stores close the gap to Windows tools:
 *   _mm512_stream_si512 writes one full 64-byte cache line per instruction,
 *   filling one WC buffer entry atomically.  With AVX2 (32-byte stores), two
 *   instructions are needed per WC buffer fill — the WC management circuitry
 *   fires twice as often, and the memory controller sees write requests at
 *   twice the frequency, making read/write mode switches more frequent.
 *   ZMM NT stores match DDR burst length exactly (64 bytes = 8 × 8-byte
 *   beat) so the controller can schedule write bursts with zero padding,
 *   recovering the last few percent of DRAM write and copy bandwidth.
 */
#if defined(__GNUC__) || defined(__clang__)

__attribute__((target("avx512f")))
static void do_read_avx512(const uint64_t *a, size_t n)
{
    /*
     * 8 ZMM accumulators × 64 bytes = 512 bytes (8 cache lines) per iteration.
     * 8 prefetch hints cover the 8 cache lines we'll consume 16 iterations later.
     * Matching the write side's 8-WC-buffer burst size keeps the memory
     * controller's read scheduler equally loaded.
     */
    size_t n64 = n & ~(size_t)63; /* 64 uint64_t = 8 ZMM per iteration */
    __m512i v0 = _mm512_setzero_si512();
    __m512i v1 = _mm512_setzero_si512();
    __m512i v2 = _mm512_setzero_si512();
    __m512i v3 = _mm512_setzero_si512();
    __m512i v4 = _mm512_setzero_si512();
    __m512i v5 = _mm512_setzero_si512();
    __m512i v6 = _mm512_setzero_si512();
    __m512i v7 = _mm512_setzero_si512();
    for (size_t i = 0; i < n64; i += 64) {
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 +  0], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 +  8], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 16], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 24], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 32], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 40], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 48], _MM_HINT_NTA);
        _mm_prefetch((const char *)&a[i + PF_DIST_U64 + 56], _MM_HINT_NTA);
        v0 = _mm512_xor_si512(v0, _mm512_loadu_si512((const __m512i *)&a[i+ 0]));
        v1 = _mm512_xor_si512(v1, _mm512_loadu_si512((const __m512i *)&a[i+ 8]));
        v2 = _mm512_xor_si512(v2, _mm512_loadu_si512((const __m512i *)&a[i+16]));
        v3 = _mm512_xor_si512(v3, _mm512_loadu_si512((const __m512i *)&a[i+24]));
        v4 = _mm512_xor_si512(v4, _mm512_loadu_si512((const __m512i *)&a[i+32]));
        v5 = _mm512_xor_si512(v5, _mm512_loadu_si512((const __m512i *)&a[i+40]));
        v6 = _mm512_xor_si512(v6, _mm512_loadu_si512((const __m512i *)&a[i+48]));
        v7 = _mm512_xor_si512(v7, _mm512_loadu_si512((const __m512i *)&a[i+56]));
    }
    __m512i vsink = _mm512_xor_si512(
        _mm512_xor_si512(_mm512_xor_si512(v0, v1), _mm512_xor_si512(v2, v3)),
        _mm512_xor_si512(_mm512_xor_si512(v4, v5), _mm512_xor_si512(v6, v7)));
    __asm__ volatile("" : "+x"(vsink) :: "memory");
    uint64_t stail = 0;
    for (size_t i = n64; i < n; i++) stail ^= a[i];
    __asm__ volatile("" : "+r"(stail));
}

__attribute__((target("avx512f")))
static void do_write_avx512(uint64_t *a, size_t n)
{
    static const uint64_t PAT = 0xDEADBEEFCAFEBABEULL;
    __m512i vpat = _mm512_set1_epi64((long long)PAT);
    size_t n8 = n & ~(size_t)7; /* 8 uint64_t = 1 ZMM = 1 WC buffer fill */
    for (size_t i = 0; i < n8; i += 8)
        _mm512_stream_si512((__m512i *)&a[i], vpat);
    _mm_sfence();
    for (size_t i = n8; i < n; i++) a[i] = PAT;
    __asm__ volatile("" ::: "memory");
}

__attribute__((target("avx512f")))
static void do_copy_avx512(uint64_t *dst, const uint64_t *src, size_t n)
{
    /*
     * 8 ZMM loads + 8 ZMM NT stores = 512 bytes (8 cache lines) per iteration.
     *
     * AMD Zen has 8 write-combining (WC) buffers × 64 bytes = 512 bytes total.
     * Issuing 8 ZMM NT stores per iteration fills ALL 8 WC buffers in one shot.
     * They drain together as a single 512-byte burst to DRAM, giving the memory
     * controller the longest possible write-mode window before it must switch back
     * to read mode to service the next 8 prefetch requests.  Mode switches (tWTR
     * / tRTW) are thus minimised to one per 512-byte block rather than one per
     * 64-byte block — the largest batch achievable without spilling into a second
     * WC-buffer rotation.
     *
     * 8 matching prefetch hints cover the 8 cache lines of src we will consume
     * 16 iterations later (1024 uint64_t / 64 per iteration = 16 iterations).
     */
    size_t n64 = n & ~(size_t)63; /* 64 uint64_t = 8 ZMM per iteration */
    for (size_t i = 0; i < n64; i += 64) {
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 +  0], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 +  8], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 16], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 24], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 32], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 40], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 48], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 56], _MM_HINT_NTA);
        __m512i v0 = _mm512_loadu_si512((const __m512i *)&src[i+ 0]);
        __m512i v1 = _mm512_loadu_si512((const __m512i *)&src[i+ 8]);
        __m512i v2 = _mm512_loadu_si512((const __m512i *)&src[i+16]);
        __m512i v3 = _mm512_loadu_si512((const __m512i *)&src[i+24]);
        __m512i v4 = _mm512_loadu_si512((const __m512i *)&src[i+32]);
        __m512i v5 = _mm512_loadu_si512((const __m512i *)&src[i+40]);
        __m512i v6 = _mm512_loadu_si512((const __m512i *)&src[i+48]);
        __m512i v7 = _mm512_loadu_si512((const __m512i *)&src[i+56]);
        _mm512_stream_si512((__m512i *)&dst[i+ 0], v0);
        _mm512_stream_si512((__m512i *)&dst[i+ 8], v1);
        _mm512_stream_si512((__m512i *)&dst[i+16], v2);
        _mm512_stream_si512((__m512i *)&dst[i+24], v3);
        _mm512_stream_si512((__m512i *)&dst[i+32], v4);
        _mm512_stream_si512((__m512i *)&dst[i+40], v5);
        _mm512_stream_si512((__m512i *)&dst[i+48], v6);
        _mm512_stream_si512((__m512i *)&dst[i+56], v7);
    }
    _mm_sfence();
    for (size_t i = n64; i < n; i++) dst[i] = src[i];
    __asm__ volatile("" ::: "memory");
}

#endif /* __GNUC__ || __clang__ */

__attribute__((optimize("O3,tree-vectorize")))
static void do_write(uint64_t *a, size_t n)
{
#if (defined(__GNUC__) || defined(__clang__)) && defined(__AVX2__)
    if (__builtin_cpu_supports("avx512f")) {
        do_write_avx512(a, n);
        return;
    }
#endif

    static const uint64_t PAT = 0xDEADBEEFCAFEBABEULL;

#if defined(__AVX2__)
    __m256i vpat = _mm256_set1_epi64x((long long)PAT);
    size_t  n32  = n & ~(size_t)31; /* 32 uint64_t = 8 YMM = 4 cache lines */

    for (size_t i = 0; i < n32; i += 32) {
        _mm256_stream_si256((__m256i *)&a[i+ 0], vpat);
        _mm256_stream_si256((__m256i *)&a[i+ 4], vpat);
        _mm256_stream_si256((__m256i *)&a[i+ 8], vpat);
        _mm256_stream_si256((__m256i *)&a[i+12], vpat);
        _mm256_stream_si256((__m256i *)&a[i+16], vpat);
        _mm256_stream_si256((__m256i *)&a[i+20], vpat);
        _mm256_stream_si256((__m256i *)&a[i+24], vpat);
        _mm256_stream_si256((__m256i *)&a[i+28], vpat);
    }
    _mm_sfence();
    for (size_t i = n32; i < n; i++)
        a[i] = PAT;
#else
    for (size_t i = 0; i < n; i++)
        a[i] = PAT;
#endif

    __asm__ volatile("" ::: "memory");
}

/*
 * Copy inner loop: 8× unrolled AVX2 — 8 loads then 8 NT stores per
 * iteration = 512 bytes (8 cache lines of src + 8 of dst).
 *
 * Why 8× instead of 4×, and why it helps on dual-CCD (e.g. 9950X3D):
 *
 *   The DDR memory controller must periodically switch between READ mode
 *   (serving src prefetch/load misses) and WRITE mode (draining NT-store
 *   write-combining buffers to dst).  Each switch costs tWTR / tRTW (~7.5 ns
 *   on DDR5).  With 4× unroll, 4 NT stores fill 2 cache lines per iteration;
 *   AMD's 8 WC buffers (Intel: 12) drain immediately when each 64-byte line
 *   is complete, forcing a read→write→read mode switch approximately every
 *   iteration.  At ~70 GB/s bandwidth, one iteration = ~3 ns → nearly every
 *   cycle is a mode-switch penalty.
 *
 *   With 8× unroll:
 *   - 8 NT stores = 4 complete 64-byte cache lines per iteration
 *   - The WC buffers accumulate a full burst of 4 writes before draining
 *   - The memory controller can stay in WRITE mode for 4 CL (~3.5 ns) before
 *     needing to switch back to READ for the next prefetch batch
 *   - Mode switches drop to ~half the frequency → measurable BW gain
 *
 *   On dual-CCD CPUs (9950X3D): each CCD has independent L3/V-Cache slices
 *   but shares the same DDR channels through the I/O die.  All 16 physical
 *   cores participate; each gets its own chunk of the DRAM buffer.  The 8×
 *   unroll reduces contention on the shared memory controller request queues
 *   by issuing larger, more coherent bursts from each core's thread.
 *
 *   Prefetch: 4 hints per iteration covering 4 cache lines at PF_DIST_U64
 *   ahead, matching the 4 CL of src loaded this iteration.  dst is NT-stored
 *   so needs no prefetch.
 */
__attribute__((optimize("O3,tree-vectorize")))
static void do_copy(uint64_t *dst, const uint64_t *src, size_t n)
{
#if (defined(__GNUC__) || defined(__clang__)) && defined(__AVX2__)
    if (__builtin_cpu_supports("avx512f")) {
        do_copy_avx512(dst, src, n);
        return;
    }
#endif
#if defined(__AVX2__)
    size_t n32 = n & ~(size_t)31; /* 32 uint64_t = 8 YMM per iteration */

    for (size_t i = 0; i < n32; i += 32) {
        /* 4 prefetch hints = 4 cache lines = one full iteration's worth of src,
         * issued PF_DIST_U64 elements ahead so DRAM latency is hidden. */
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 +  0], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 +  8], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 16], _MM_HINT_NTA);
        _mm_prefetch((const char *)&src[i + PF_DIST_U64 + 24], _MM_HINT_NTA);
        /* 8 loads first — all hit in L1 due to prefetch from 32 iters ago */
        __m256i v0 = _mm256_loadu_si256((const __m256i *)&src[i+ 0]);
        __m256i v1 = _mm256_loadu_si256((const __m256i *)&src[i+ 4]);
        __m256i v2 = _mm256_loadu_si256((const __m256i *)&src[i+ 8]);
        __m256i v3 = _mm256_loadu_si256((const __m256i *)&src[i+12]);
        __m256i v4 = _mm256_loadu_si256((const __m256i *)&src[i+16]);
        __m256i v5 = _mm256_loadu_si256((const __m256i *)&src[i+20]);
        __m256i v6 = _mm256_loadu_si256((const __m256i *)&src[i+24]);
        __m256i v7 = _mm256_loadu_si256((const __m256i *)&src[i+28]);
        /* 8 NT stores — 4 full cache lines → WC buffers drain in one burst */
        _mm256_stream_si256((__m256i *)&dst[i+ 0], v0);
        _mm256_stream_si256((__m256i *)&dst[i+ 4], v1);
        _mm256_stream_si256((__m256i *)&dst[i+ 8], v2);
        _mm256_stream_si256((__m256i *)&dst[i+12], v3);
        _mm256_stream_si256((__m256i *)&dst[i+16], v4);
        _mm256_stream_si256((__m256i *)&dst[i+20], v5);
        _mm256_stream_si256((__m256i *)&dst[i+24], v6);
        _mm256_stream_si256((__m256i *)&dst[i+28], v7);
    }
    _mm_sfence();
    for (size_t i = n32; i < n; i++)
        dst[i] = src[i];
#else
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
#endif

    __asm__ volatile("" ::: "memory");
}

/*
 * Worker thread: pins itself to a specific logical CPU, then loops waiting
 * on bar_start.  Main sets op before releasing bar_start.  After the work,
 * the worker hits bar_end to signal completion (except OP_EXIT — it returns
 * directly so main can pthread_join without another barrier wait).
 */
static void *bw_worker(void *varg)
{
    bw_arg_t *a = varg;

    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(a->cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    for (;;) {
        pthread_barrier_wait(a->bar_start);

        switch (a->op) {
        case OP_READ:
            do_read(a->buf_a, a->n);
            break;
        case OP_WRITE:
            do_write(a->buf_a, a->n);
            break;
        case OP_COPY:
            do_copy(a->buf_a, a->buf_b, a->n);
            break;
        case OP_EXIT:
            return NULL;
        }

        pthread_barrier_wait(a->bar_end);
    }
}

/*
 * Cache eviction sweep: fill the shared caches with unrelated data so that
 * measurement-buffer lines evicted by clflushopt cannot be re-fetched from
 * an intermediate buffer in the memory subsystem.
 *
 * Stride is 32 bytes (half a cache line) rather than 64:
 *   - Both the low and high 32-byte halves of each 64-byte line are written,
 *     producing a dirty (modified) state.  A dirty line must be written back
 *     before it can be replaced, creating stronger pressure on L2/L3 fill
 *     buffers than a read-only sweep.
 *   - Two store operations per cache line also give the hardware prefetcher
 *     more address patterns to saturate, which helps ensure the eviction
 *     buffer occupies every way of set-associative caches (including the
 *     extra ways in AMD 3D V-Cache).
 */
static void evict_with_sweep(uint8_t *ev, size_t bytes)
{
    volatile uint8_t *p = ev;
    for (size_t i = 0; i < bytes; i += 32) {
        (void)p[i];   /* read: bring the cache line in */
        p[i] = 0;     /* write: mark dirty, forces write-back on eviction */
    }
    __asm__ volatile("" ::: "memory");
}

/*
 * Run bandwidth passes until the coefficient of variation (CV = σ/μ) of the
 * collected samples drops below BW_CV_TARGET (1%), then return the median.
 *
 * Why CV instead of a fixed pass count:
 *   On a quiet system, 3–5 passes usually converge.  Under background load
 *   (system updates, browser, etc.) variance stays high and more passes are
 *   needed.  A fixed count of 5 under-samples noisy systems (wide spread,
 *   unreliable median) and over-samples quiet ones (wasted time).  Stopping
 *   on CV < 1% gives consistent accuracy regardless of system noise.
 *
 *   CV² is compared against BW_CV_TARGET² to avoid a sqrt() in the hot path.
 *
 * stream_mult=1 for read/write, 2 for copy (STREAM convention: src read +
 * dst write = 2× bytes of memory traffic).
 */
static double run_bw_passes(bw_arg_t *args, int nthreads,
                             pthread_barrier_t *bar_start,
                             pthread_barrier_t *bar_end,
                             bw_op_t op, size_t total_bytes, int stream_mult,
                             uint8_t *evict_buf, size_t evict_bytes)
{
    double samples[BW_MAX_PASSES];
    int    npass = 0;

    for (npass = 0; npass < BW_MAX_PASSES; npass++) {
        /* Flush all chunks outside the timed window so every access is cold */
        for (int t = 0; t < nthreads; t++) {
            flush_buffer(args[t].buf_a, args[t].n * sizeof(uint64_t));
            if (op == OP_COPY)
                flush_buffer(args[t].buf_b, args[t].n * sizeof(uint64_t));
        }

        /* Thrash shared caches with unrelated data before timing. */
        if (evict_buf && evict_bytes)
            evict_with_sweep(evict_buf, evict_bytes);

        for (int t = 0; t < nthreads; t++) args[t].op = op;

        /* bar_start releases all workers simultaneously; main records t0
         * immediately after so the timed window starts as close to the
         * workers' first instruction as possible. */
        pthread_barrier_wait(bar_start);
        long long t0 = now_ns();

        pthread_barrier_wait(bar_end);
        long long t1 = now_ns();

        samples[npass] = (double)((size_t)stream_mult * total_bytes) /
                         ((double)(t1 - t0) * 1e-9) / 1e6;

        /* Check convergence after the minimum number of passes */
        if (npass + 1 >= BW_MIN_PASSES) {
            int    n    = npass + 1;
            double mean = 0.0;
            for (int i = 0; i < n; i++) mean += samples[i];
            mean /= n;

            double var = 0.0;
            for (int i = 0; i < n; i++) {
                double d = samples[i] - mean;
                var += d * d;
            }
            /*
             * Sample variance (Bessel's correction: divide by n-1, not n).
             * Population variance (÷n) underestimates the true spread by the
             * factor n/(n-1).  At BW_MIN_PASSES=5 that is a 25% underestimate,
             * meaning the check could declare convergence when the real CV is
             * 1.25% — 25% over the 1% target.  The unbiased estimator (÷(n-1))
             * gives the correct answer.
             * CV² < target² avoids sqrt(); target=1% → target²=0.0001.
             */
            double cv2 = (var / (n - 1)) / (mean * mean);
            if (cv2 < (BW_CV_TARGET * BW_CV_TARGET))
                break;
        }
    }

    /*
     * When the loop exhausts all BW_MAX_PASSES without converging, the for-loop
     * post-increment leaves npass == BW_MAX_PASSES, but only BW_MAX_PASSES
     * samples were collected (indices 0..BW_MAX_PASSES-1).  Using npass+1 here
     * would read samples[BW_MAX_PASSES] — one past the end of the array (UB)
     * and a wrong median index.
     */
    int n = (npass < BW_MAX_PASSES) ? npass + 1 : BW_MAX_PASSES;
    qsort(samples, n, sizeof(double), cmp_double);
    return samples[n / 2];
}

/* ── Physical core enumeration ────────────────────────────────────────── */

typedef struct {
    int pkg;
    int core;
} core_key_t;

/*
 * Build a list of logical CPUs, one per physical core, by walking
 * sysfs CPU topology (physical_package_id/core_id).
 *
 * This avoids SMT siblings running separate worker threads which can
 * otherwise contend for per-core resources (ROB, store queue, etc.) and
 * slightly depress the measured DRAM bandwidth.
 *
 * Fallback: if topology files are missing or unreadable, we just use the
 * online logical CPU count from sysconf().
 */
static int build_cpu_list(int *cpus, int max_cpus)
{
    core_key_t seen[1024];
    int        nseen = 0;
    int        ncpus = 0;

    /*
     * Use _SC_NPROCESSORS_CONF (configured CPUs) rather than a hardcoded
     * 1024.  On systems with CPU hotplug or cgroup CPU restrictions the
     * numbering can have gaps (e.g. cpu0, cpu1, cpu4, cpu5 …).  Using
     * `break` on the first missing entry would stop early and miss the
     * remaining CPUs; `continue` skips the gap and keeps scanning.
     */
    long max_possible = sysconf(_SC_NPROCESSORS_CONF);
    if (max_possible <= 0) max_possible = 1024;

    for (int cpu = 0; cpu < (int)max_possible && ncpus < max_cpus; cpu++) {
        char path[160];
        FILE *f;
        int   core_id = -1, pkg_id = -1;

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/core_id", cpu);
        f = fopen(path, "r");
        if (!f) {
            /* CPU may be offline or hotplugged out — skip this slot. */
            continue;
        }
        if (fscanf(f, "%d", &core_id) != 1) {
            fclose(f);
            continue;
        }
        fclose(f);

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                 cpu);
        f = fopen(path, "r");
        if (!f || fscanf(f, "%d", &pkg_id) != 1) {
            if (f) fclose(f);
            continue;
        }
        fclose(f);

        int duplicate = 0;
        for (int i = 0; i < nseen; i++) {
            if (seen[i].pkg == pkg_id && seen[i].core == core_id) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;

        if (nseen < (int)(sizeof(seen) / sizeof(seen[0]))) {
            seen[nseen].pkg  = pkg_id;
            seen[nseen].core = core_id;
            nseen++;
        }

        cpus[ncpus++] = cpu;
    }

    if (ncpus == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n <= 0 || n > max_cpus) n = 1;
        for (int i = 0; i < (int)n; i++)
            cpus[i] = i;
        ncpus = (int)n;
    }

    return ncpus;
}

/* ── Detect DRAM buffer size ─────────────────────────────────────────── */
/*
 * Sum the L3 size across all unique cache instances (one per CCD on
 * multi-CCD CPUs) then use 4× that total, with a 512 MB floor.
 *
 * The separate eviction buffer in bench_run() uses 2×this value so the
 * shared caches are fully churned between passes even on 3D V-Cache parts.
 */
static size_t dram_buf_bytes(void)
{
    size_t total_l3 = 0;

    /*
     * Use _SC_NPROCESSORS_CONF as the bound and `continue` on missing entries
     * for the same reason as build_cpu_list: on dual-CCD systems (e.g. 9950X3D)
     * or systems with offline CPUs, CPU numbers can have gaps.  A `break` on
     * the first missing sysfs entry would miss all CCDs beyond the gap and
     * drastically under-count total L3, producing a buffer too small to force
     * DRAM accesses and producing inflated (cache-hitting) bandwidth results.
     */
    long max_possible = sysconf(_SC_NPROCESSORS_CONF);
    if (max_possible <= 0) max_possible = 1024;

    for (int cpu = 0; cpu < (int)max_possible; cpu++) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index3/size", cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[32] = {0};
        size_t sz = 0;
        if (fgets(buf, sizeof(buf), f))
            sz = strtoul(buf, NULL, 10) * 1024;
        fclose(f);

        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cache/index3/shared_cpu_list", cpu);
        f = fopen(path, "r");
        if (f) {
            char list[64] = {0};
            int first_cpu = -1;
            if (fgets(list, sizeof(list), f))
                first_cpu = atoi(list);
            fclose(f);
            if (first_cpu == cpu)
                total_l3 += sz;
        } else {
            total_l3 += sz;
        }
    }

    size_t target = total_l3 * 4;
    if (target < 512UL * 1024 * 1024) target = 512UL * 1024 * 1024;
    return target;
}

/* ── Dynamic latency buffer sizing ───────────────────────────────────── */

/*
 * Read the size reported by sysfs for a specific cache level on cpu0.
 * sysfs reports in kB (e.g. "32K" → strtoul gives 32 → ×1024 = 32768).
 * index mapping: 0=L1D, 1=L1I, 2=L2, 3=L3.
 * Returns 0 if the file is absent or unparseable.
 */
static size_t read_cache_size(int cpu, int index)
{
    char path[160], buf[32] = {0};
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cache/index%d/size", cpu, index);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    fgets(buf, sizeof(buf), f);
    fclose(f);
    return strtoul(buf, NULL, 10) * 1024UL;
}

/*
 * Choose latency-benchmark buffer sizes based on the actual cache topology.
 *
 * Each buffer must land in exactly one level:
 *   L1  buffer = 3/4 × L1D  → clearly inside L1, clear of the edge
 *   L2  buffer = 1/2 × L2   → inside L2, well above L1
 *   L3  buffer = 1/2 × per-CCD L3 → inside L3, well above L2
 *
 * Per-CCD L3 is read from cpu0's index3 entry; on multi-CCD CPUs each CCD
 * has its own L3 slice so cpu0's entry gives the relevant slice size.
 *
 * Sanity clamps ensure each level's buffer strictly exceeds the previous
 * (e.g. if sysfs is absent and fallbacks happen to collide).
 *
 * Fallbacks match the previous hardcoded constants so behaviour on systems
 * without sysfs (containers, unusual kernels) is unchanged.
 */
static void detect_lat_buf_sizes(size_t *l1, size_t *l2, size_t *l3)
{
    size_t s_l1 = read_cache_size(0, 0); /* index0 = L1D */
    size_t s_l2 = read_cache_size(0, 2); /* index2 = L2  */
    size_t s_l3 = read_cache_size(0, 3); /* index3 = L3 (per-CCD slice) */

    *l1 = s_l1 ? (s_l1 * 3 / 4)  : (24UL  * 1024);
    *l2 = s_l2 ? (s_l2 / 2)      : (512UL * 1024);
    *l3 = s_l3 ? (s_l3 / 2)      : (16UL  * 1024 * 1024);

    /* Guarantee strict ordering: each buffer must exceed the level below */
    if (*l2 <= *l1) *l2 = *l1 * 4;
    if (*l3 <= *l2) *l3 = *l2 * 8;
}

/* ── Public ──────────────────────────────────────────────────────────── */

/*
 * Try to run via the tuxbench kernel module (/dev/tuxbench).
 * Returns 1 on success (results filled), 0 if module not loaded or ioctl failed.
 *
 * The kernel path gives:
 *   - wbinvd_on_all_cpus()    — single-instruction full cache flush (incl. 3D V-Cache)
 *   - alloc_pages_node()      — guaranteed physically contiguous NUMA-local pages
 *   - kthread_bind()          — hard CPU pin, SCHED_FIFO RT priority
 *
 * The module is loaded at startup by backend_read_summary() and unloaded on exit
 * by backend_cleanup().
 */
static int bench_run_kernel(bench_results_t *out)
{
    int fd = open("/dev/tuxbench", O_RDWR);
    if (fd < 0)
        return 0;

    struct tuxbench_req req = {
        .flags = TUXBENCH_FL_LAT | TUXBENCH_FL_BW,
    };

    if (ioctl(fd, TUXBENCH_IOC_RUN, &req) != 0) {
        close(fd);
        return 0;
    }
    close(fd);

    /* convert picoseconds → nanoseconds (double) */
    out->lat_l1_ns   = (double)req.lat_l1_ps   / 1000.0;
    out->lat_l2_ns   = (double)req.lat_l2_ps   / 1000.0;
    out->lat_l3_ns   = (double)req.lat_l3_ps   / 1000.0;
    out->lat_dram_ns = (double)req.lat_dram_ps  / 1000.0;

    /* convert KB/s → MB/s */
    out->bw_read_mbs  = (double)req.bw_read_kbs  / 1024.0;
    out->bw_write_mbs = (double)req.bw_write_kbs / 1024.0;
    out->bw_copy_mbs  = (double)req.bw_copy_kbs  / 1024.0;

    return 1;
}


void bench_run(bench_results_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Prefer kernel module path: better flush, guaranteed huge pages, hard CPU pin */
    if (bench_run_kernel(out))
        return;

    /* --- Latency (single-threaded random pointer chasing) ---
     *
     * Buffer sizes are derived from sysfs cache topology so the benchmark
     * remains accurate across CPU generations (large L2 on Zen 4 / Raptor
     * Lake, 3D V-Cache L3, etc.).
     *
     * min_accesses chosen so each test runs ~200–500 ms per sample.
     * DRAM uses dram_buf_bytes() (≥4×total L3, ≥512 MB) and flush_each=1
     * so every sample truly goes to DRAM rather than cache residue from the
     * previous sample.  Only 3 samples because each one already traverses
     * millions of nodes (~380 ms). */
    size_t lat_l1, lat_l2, lat_l3;
    detect_lat_buf_sizes(&lat_l1, &lat_l2, &lat_l3);
    size_t lat_dram = dram_buf_bytes();

    out->lat_l1_ns   = measure_latency_ns(lat_l1,   200000000LL, LAT_SAMPLES, 0);
    out->lat_l2_ns   = measure_latency_ns(lat_l2,    50000000LL, LAT_SAMPLES, 0);
    out->lat_l3_ns   = measure_latency_ns(lat_l3,     5000000LL, LAT_SAMPLES, 0);
    out->lat_dram_ns = measure_latency_ns(lat_dram,   1000000LL, 3,           1);

    /* --- Bandwidth (multi-threaded, one thread per physical core) ---
     *
     * Each thread works on its own contiguous chunk of a shared buffer
     * that exceeds the total L3 (including 3D V-Cache).  Threads are
     * pinned to specific logical CPUs and synchronised with barriers so
     * the wall-clock timing window covers all parallel work. */
    int    cpu_list[MAX_THREADS];
    int    nthreads = build_cpu_list(cpu_list, MAX_THREADS);
    size_t dram_sz  = dram_buf_bytes();

    uint64_t *buf_a = bench_alloc(dram_sz);
    uint64_t *buf_b = bench_alloc(dram_sz);
    /* Eviction buffer: 2× dram_sz so we churn well beyond total L3. */
    uint8_t  *evict  = bench_alloc(dram_sz * 2);
    if (!buf_a || !buf_b || !evict) {
        bench_free(buf_a, dram_sz);
        bench_free(buf_b, dram_sz);
        bench_free(evict, dram_sz * 2);
        return;
    }

    /* Touch all pages so THP faults and TLB entries are warm before
     * the timed passes begin. */
    memset(buf_a, 0xAB, dram_sz);
    memset(buf_b, 0xCD, dram_sz);
    memset(evict, 0xEF, dram_sz * 2);

    pthread_barrier_t bar_start, bar_end;
    /* nthreads workers + 1 main */
    pthread_barrier_init(&bar_start, NULL, (unsigned)(nthreads + 1));
    pthread_barrier_init(&bar_end,   NULL, (unsigned)(nthreads + 1));

    bw_arg_t  args[MAX_THREADS];
    pthread_t tids[MAX_THREADS];

    size_t total_n  = dram_sz / sizeof(uint64_t);
    size_t chunk_n  = total_n / (size_t)nthreads;

    for (int t = 0; t < nthreads; t++) {
        size_t off     = (size_t)t * chunk_n;
        /* Last thread takes any remainder so no bytes are skipped */
        size_t this_n  = (t == nthreads - 1) ? total_n - off : chunk_n;
        args[t].buf_a     = buf_a + off;
        args[t].buf_b     = buf_b + off;
        args[t].n         = this_n;
        args[t].cpu       = cpu_list[t];
        args[t].op        = OP_READ;
        args[t].bar_start = &bar_start;
        args[t].bar_end   = &bar_end;
        pthread_create(&tids[t], NULL, bw_worker, &args[t]);
    }

    out->bw_read_mbs  = run_bw_passes(args, nthreads, &bar_start, &bar_end,
                                       OP_READ,  dram_sz, 1,
                                       evict, dram_sz * 2);
    out->bw_write_mbs = run_bw_passes(args, nthreads, &bar_start, &bar_end,
                                       OP_WRITE, dram_sz, 1,
                                       evict, dram_sz * 2);
    out->bw_copy_mbs  = run_bw_passes(args, nthreads, &bar_start, &bar_end,
                                       OP_COPY,  dram_sz, 2,
                                       evict, dram_sz * 2);

    /* Signal threads to exit — they return directly without hitting bar_end */
    for (int t = 0; t < nthreads; t++) args[t].op = OP_EXIT;
    pthread_barrier_wait(&bar_start);
    for (int t = 0; t < nthreads; t++) pthread_join(tids[t], NULL);

    pthread_barrier_destroy(&bar_start);
    pthread_barrier_destroy(&bar_end);

    bench_free(buf_a, dram_sz);
    bench_free(buf_b, dram_sz);
    bench_free(evict, dram_sz * 2);
}
