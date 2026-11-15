/* v-perf-pnl-cut-shift.c — analytical probe, stream D (performance audit).
 *
 * Measures the element-at-a-time tail-shift loops used by pnl_cut()
 * (pnl.c:377-385), gc_repnl_get_sequence() (gc-get.c:759-768) and
 * list_remove_first()/rkl_push() (rkl.c) against the equivalent
 * memmove() that a vectorizing replacement would use.
 *
 * These loops shift 4-byte elements one per iteration. Modern cores move
 * 32-64 bytes per cycle with AVX2 memmove; the scalar loop moves 4 bytes
 * per iteration with a load+store dependency chain (~1 elem/cycle best
 * case, load-to-use latency bound ~5 cycles/iteration if not unrolled).
 *
 * Build: cc -O2 -std=gnu11 -o /tmp/probe3 v-perf-pnl-cut-shift.c
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef uint32_t pgno_t;

/* pnl_cut() replica (descending PNL): remove [pos, pos+span) shifting the tail */
__attribute__((noinline)) static void cut_scalar(pgno_t *pnl, size_t len, size_t pos, size_t span) {
  size_t nlen = len - span;
  for (size_t i = pos; i <= nlen; ++i)
    pnl[i] = pnl[i + span];
}

/* gc_repnl_get_sequence() replica: shift everything after target down by num */
__attribute__((noinline)) static void cut_seq_scalar(pgno_t *pnl, size_t len, size_t target_idx, size_t num) {
  pgno_t *t = pnl + target_idx;
  const pgno_t *const end = pnl + len;
  for (; ++t <= end;)
    t[-(ptrdiff_t)num] = *t;
}

__attribute__((noinline)) static void cut_memmove(pgno_t *pnl, size_t len, size_t pos, size_t span) {
  memmove(pnl + pos, pnl + pos + span, (len - pos - span + 1) * sizeof(pgno_t));
}

static uint64_t nsec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static pgno_t buf[1 << 17];

static void bench(const char *what, size_t len, size_t pos, size_t span) {
  const int iters = 20000;
  volatile size_t sink = 0;
  uint64_t t0, t1;

  for (size_t i = 0; i < len + 2; ++i)
    buf[i] = (pgno_t)(len - i);
  t0 = nsec();
  for (int i = 0; i < iters; ++i)
    cut_scalar(buf, len, pos, span);
  t1 = nsec();
  const double sc = (double)(t1 - t0) / iters;
  sink ^= buf[0];

  for (size_t i = 0; i < len + 2; ++i)
    buf[i] = (pgno_t)(len - i);
  t0 = nsec();
  for (int i = 0; i < iters; ++i)
    cut_memmove(buf, len, pos, span);
  t1 = nsec();
  const double mm = (double)(t1 - t0) / iters;
  sink ^= buf[0];

  printf("%-34s len=%6zu pos=%6zu span=%zu: scalar %8.1f ns | memmove %7.1f ns | x%.1f\n", what, len, pos, span, sc, mm,
         sc / mm);
  (void)sink;
}

int main(void) {
  bench("cut@tail (best case)", 100000, 99995, 4);
  bench("cut@middle", 100000, 50000, 4);
  bench("crop-tail-seq (cut@1, span=2)", 100000, 1, 2);
  bench("repnl seq@head (num=4)", 100000, 3, 4);
  bench("small repnl cut@1", 2048, 1, 2);
  bench("small repnl cut@mid", 2048, 1000, 2);
  return 0;
}
