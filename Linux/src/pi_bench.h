#ifndef PI_BENCH_H
#define PI_BENCH_H

typedef struct {
    double time_sec;
    double digits_per_sec;
    int    n_digits;
} pi_results_t;

/*
 * Compute pi to n_digits decimal digits using the Chudnovsky algorithm
 * with binary splitting, parallelised across all online logical CPUs.
 *
 * Requires: libgmp (-lgmp)
 */
void pi_bench_run(int n_digits, pi_results_t *out);

#endif /* PI_BENCH_H */
