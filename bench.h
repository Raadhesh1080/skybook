#ifndef BENCH_H
#define BENCH_H

/* Runs the full benchmark suite with n flight records.
 * Returns 0 if every correctness check passed. Must be called while no
 * database is open (it creates and drops its own "skybench" database). */
int run_benchmark(long n);

#endif
