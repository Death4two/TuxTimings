/*
 * pi_bench.c — Pi computation benchmark using the Chudnovsky algorithm
 *
 * Algorithm: Chudnovsky brothers series with binary splitting.
 *   Each term contributes ~14.18 decimal digits of precision.
 *   Binary splitting reduces N individual term computations to O(log N)
 *   large-integer multiplications, making it O(M(n) log(n)^2) overall
 *   where M(n) is the cost of an n-digit multiplication.
 *
 * Parallelisation: work queue with M = nthreads × 16 tasks.
 *   Each thread pulls tasks from the queue and processes them one at a time.
 *   Progress is updated after each completed task, giving smooth reporting
 *   throughout the run rather than a single jump at the end.
 *   After all threads finish, the coordinator merges the M task results in
 *   order and computes the final sqrt and division serially.
 *
 * Requires: libgmp (-lgmp)
 */

#define _GNU_SOURCE
#include "pi_bench.h"
#include <gmp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

/* 640320^3 / 24 — fits in 64-bit unsigned long on x86-64 */
#define C3_OVER_24  10939058860032000UL

/* Number of Chudnovsky terms needed for d decimal digits of pi.
 * The series converges at ~14.181647 digits/term; add a small margin. */
static long terms_needed(int digits)
{
    return (long)(digits / 14.0) + 16;
}

/* ── Serial binary splitting ──────────────────────────────────────────── */

/*
 * Compute P(a,b), Q(a,b), T(a,b) for the Chudnovsky binary splitting.
 *
 * For a single term at position k (b = a+1):
 *   k == 0:  P = 1,  Q = 1,  T = 13591409
 *   k  > 0:  P = -(6k-5)(2k-1)(6k-1)
 *             Q = k³ × C3_OVER_24
 *             T = P × (13591409 + 545140134k)
 *
 * Recursive combination (m = midpoint):
 *   P(a,b) = P(a,m) × P(m,b)
 *   Q(a,b) = Q(a,m) × Q(m,b)
 *   T(a,b) = T(a,m) × Q(m,b) + P(a,m) × T(m,b)
 *
 * Final result: pi = 426880 × sqrt(10005) × Q(0,N) / T(0,N)
 */
static void bs(mpz_t P, mpz_t Q, mpz_t T, long a, long b)
{
    if (b - a == 1) {
        if (a == 0) {
            mpz_set_ui(P, 1);
            mpz_set_ui(Q, 1);
            mpz_set_ui(T, 13591409);
        } else {
            /* P = -(6a-5)(2a-1)(6a-1) */
            mpz_set_si(P, -(6*a - 5));
            mpz_mul_si(P, P, 2*a - 1);
            mpz_mul_si(P, P, 6*a - 1);
            /* Q = a³ × C3_OVER_24 */
            mpz_set_ui(Q, (unsigned long)a);
            mpz_pow_ui(Q, Q, 3);
            mpz_mul_ui(Q, Q, C3_OVER_24);
            /* T = P × (13591409 + 545140134a) */
            mpz_set_si(T, 13591409L + 545140134L * a);
            mpz_mul(T, T, P);
        }
        return;
    }

    long m = (a + b) / 2;
    mpz_t Pm, Qm, Tm;
    mpz_inits(Pm, Qm, Tm, NULL);

    bs(P, Q, T, a, m);
    bs(Pm, Qm, Tm, m, b);

    /* T = T*Qm + P*Tm */
    mpz_t tmp;
    mpz_init(tmp);
    mpz_mul(T, T, Qm);
    mpz_mul(tmp, P, Tm);
    mpz_add(T, T, tmp);
    mpz_clear(tmp);

    /* P = P * Pm,  Q = Q * Qm */
    mpz_mul(P, P, Pm);
    mpz_mul(Q, Q, Qm);

    mpz_clears(Pm, Qm, Tm, NULL);
}

/* ── Segment merge ────────────────────────────────────────────────────── */

static void bs_merge(mpz_t P1, mpz_t Q1, mpz_t T1,
                     const mpz_t P2, const mpz_t Q2, const mpz_t T2)
{
    mpz_t tmp;
    mpz_init(tmp);
    /* T1 = T1*Q2 + P1*T2 */
    mpz_mul(T1, T1, Q2);
    mpz_mul(tmp, P1, T2);
    mpz_add(T1, T1, tmp);
    /* P1 = P1*P2,  Q1 = Q1*Q2 */
    mpz_mul(P1, P1, P2);
    mpz_mul(Q1, Q1, Q2);
    mpz_clear(tmp);
}

/* ── Work queue ───────────────────────────────────────────────────────── */

typedef struct {
    long  a, b;
    mpz_t P, Q, T;
} bs_task_t;

typedef struct {
    bs_task_t  *tasks;
    int         M;           /* total number of tasks      */
    long        N;           /* total Chudnovsky terms     */
    _Atomic int next_task;   /* index of next unclaimed task */
} bs_queue_t;

static void *bs_queue_worker(void *arg)
{
    bs_queue_t *q = arg;
    int t;

    while ((t = atomic_fetch_add(&q->next_task, 1)) < q->M) {
        bs_task_t *task = &q->tasks[t];
        task->a = (long)t       * q->N / q->M;
        task->b = (long)(t + 1) * q->N / q->M;
        if (task->b > q->N) task->b = q->N;

        bs(task->P, task->Q, task->T, task->a, task->b);
    }
    return NULL;
}

/* ── Parallel merge (tree reduction) ─────────────────────────────────── */

typedef struct {
    bs_task_t          *tasks;
    _Atomic int         next_pair;
    int                 M;          /* current number of valid tasks */
    int                 shutdown;
    pthread_barrier_t  *bar_start;
    pthread_barrier_t  *bar_end;
} merge_ctx_t;

static void merge_pair(bs_task_t *dst, bs_task_t *a, bs_task_t *b)
{
    (void)a;
    /* dst = merge(dst,b). We keep all mpz_t initialized; do not clear here. */
    bs_merge(dst->P, dst->Q, dst->T, b->P, b->Q, b->T);
    mpz_set_ui(b->P, 0);
    mpz_set_ui(b->Q, 0);
    mpz_set_ui(b->T, 0);
}

static void *merge_worker(void *arg)
{
    merge_ctx_t *ctx = arg;
    for (;;) {
        pthread_barrier_wait(ctx->bar_start);
        if (ctx->shutdown || ctx->M <= 1) {
            pthread_barrier_wait(ctx->bar_end);
            break;
        }

        int pairs = ctx->M / 2;
        int i;
        while ((i = atomic_fetch_add(&ctx->next_pair, 1)) < pairs) {
            int left  = 2 * i;
            int right = left + 1;
            merge_pair(&ctx->tasks[left], &ctx->tasks[left], &ctx->tasks[right]);
        }

        pthread_barrier_wait(ctx->bar_end);
    }
    return NULL;
}

/* ── Public entry point ───────────────────────────────────────────────── */

void pi_bench_run(int n_digits, pi_results_t *out)
{
    struct timespec t0, t1;
    long N = terms_needed(n_digits);

    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1)  nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    /*
     * 16 tasks per thread: each thread processes tasks one at a time from the
     * queue, so tasks complete continuously and progress stays smooth.
     * More tasks = smoother progress but more overhead in the merge phase.
     */
    int M = nthreads * 16;
    if (M > (int)N) M = (int)N;

    /* GMP floating-point precision: log2(10) ≈ 3.322 bits per decimal digit */
    mp_bitcnt_t prec = (mp_bitcnt_t)((double)n_digits * 3.322 + 128);

    out->n_digits = n_digits;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    bs_task_t *tasks = calloc((size_t)M, sizeof(bs_task_t));
    pthread_t *tids  = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!tasks || !tids) { free(tasks); free(tids); return; }

    /* Initialize GMP integers once (safe for swaps/sets in parallel merge). */
    for (int i = 0; i < M; i++) {
        mpz_inits(tasks[i].P, tasks[i].Q, tasks[i].T, NULL);
        mpz_set_ui(tasks[i].P, 0);
        mpz_set_ui(tasks[i].Q, 0);
        mpz_set_ui(tasks[i].T, 0);
    }

    bs_queue_t q;
    q.tasks = tasks;
    q.M     = M;
    q.N     = N;
    atomic_init(&q.next_task, 0);

    int nstarted = 0;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, bs_queue_worker, &q) == 0)
            nstarted++;
    }
    for (int i = 0; i < nstarted; i++)
        pthread_join(tids[i], NULL);

    /* Merge all M task results with a parallel tree reduction. */
    if (M > 1 && nthreads > 1) {
        pthread_barrier_t bar_start, bar_end;
        pthread_barrier_init(&bar_start, NULL, (unsigned)(nthreads + 1));
        pthread_barrier_init(&bar_end,   NULL, (unsigned)(nthreads + 1));

        merge_ctx_t mctx;
        mctx.tasks     = tasks;
        mctx.M         = M;
        mctx.shutdown  = 0;
        mctx.bar_start = &bar_start;
        mctx.bar_end   = &bar_end;
        atomic_init(&mctx.next_pair, 0);

        pthread_t *mtids = calloc((size_t)nthreads, sizeof(pthread_t));
        int mnstarted = 0;
        if (mtids) {
            for (int i = 0; i < nthreads; i++) {
                if (pthread_create(&mtids[i], NULL, merge_worker, &mctx) == 0)
                    mnstarted++;
            }
        }

        while (mctx.M > 1) {
            atomic_store(&mctx.next_pair, 0);
            pthread_barrier_wait(&bar_start);
            pthread_barrier_wait(&bar_end);

            int pairs = mctx.M / 2;
            int newM  = pairs + (mctx.M & 1);

            /* Compact: move merged results from even slots [0,2,4,...] into [0..pairs-1]. */
            for (int i = 0; i < pairs; i++) {
                int src = 2 * i;
                if (i != src) {
                    mpz_swap(tasks[i].P, tasks[src].P);
                    mpz_swap(tasks[i].Q, tasks[src].Q);
                    mpz_swap(tasks[i].T, tasks[src].T);
                }
            }
            /* If odd: move last element into the new tail slot (index = pairs). */
            if (mctx.M & 1) {
                int last = mctx.M - 1;
                int tail = pairs;
                if (tail != last) {
                    mpz_swap(tasks[tail].P, tasks[last].P);
                    mpz_swap(tasks[tail].Q, tasks[last].Q);
                    mpz_swap(tasks[tail].T, tasks[last].T);
                }
            }

            mctx.M = newM;
        }

        /* Shut down merge workers */
        mctx.shutdown = 1;
        pthread_barrier_wait(&bar_start);
        pthread_barrier_wait(&bar_end);
        for (int i = 0; i < mnstarted; i++)
            pthread_join(mtids[i], NULL);
        free(mtids);
        pthread_barrier_destroy(&bar_start);
        pthread_barrier_destroy(&bar_end);
    } else {
        /* Fallback: serial merge */
        for (int i = 1; i < M; i++) {
            bs_merge(tasks[0].P, tasks[0].Q, tasks[0].T,
                     tasks[i].P, tasks[i].Q, tasks[i].T);
            mpz_set_ui(tasks[i].P, 0);
            mpz_set_ui(tasks[i].Q, 0);
            mpz_set_ui(tasks[i].T, 0);
        }
    }

    /*
     * pi = 426880 × sqrt(10005) × Q / T
     * (derived from 12/640320^(3/2) = 426880/sqrt(10005)/640320^3)
     */
    mpf_set_default_prec(prec);
    mpf_t sqrt_part, fQ, fT;
    mpf_inits(sqrt_part, fQ, fT, NULL);

    mpf_set_ui(sqrt_part, 10005);
    mpf_sqrt(sqrt_part, sqrt_part);
    mpf_mul_ui(sqrt_part, sqrt_part, 426880);

    mpf_set_z(fQ, tasks[0].Q);
    mpf_set_z(fT, tasks[0].T);
    mpf_mul(fQ, fQ, sqrt_part);   /* fQ = Q × 426880 × sqrt(10005) */
    mpf_div(fQ, fQ, fT);          /* fQ = pi                        */

    clock_gettime(CLOCK_MONOTONIC, &t1);

    out->time_sec       = (t1.tv_sec - t0.tv_sec)
                        + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    out->digits_per_sec = (double)n_digits / out->time_sec;

    mpf_clears(sqrt_part, fQ, fT, NULL);
    for (int i = 0; i < M; i++)
        mpz_clears(tasks[i].P, tasks[i].Q, tasks[i].T, NULL);
    free(tasks);
    free(tids);
}
