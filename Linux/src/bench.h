#ifndef BENCH_H
#define BENCH_H

typedef struct {
    double lat_l1_ns;
    double lat_l2_ns;
    double lat_l3_ns;
    double lat_dram_ns;
    double bw_read_mbs;
    double bw_write_mbs;
    double bw_copy_mbs;
} bench_results_t;

/* Run all benchmarks — blocks for ~2–4 seconds. Call from a background thread. */
void bench_run(bench_results_t *out);


#endif /* BENCH_H */
