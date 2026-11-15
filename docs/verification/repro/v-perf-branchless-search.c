/* v-perf-branchless-search.c — analytical probe, stream D (performance audit).
 *
 * Question: do the BINARY_BRANCHLESS_SEARCH_CYCLE macros of tree-search.c
 * actually compile branchless (single cmov per probe + predictable loop-exit),
 * and how do they compare against a classic branchy binary search for the
 * two page-level regimes:
 *   - branch pages: ~16-256 keys (fan-out)
 *   - dupfix/leaf pages: up to thousands of keys
 * under (a) uniform-random lookup keys (predictor-hostile for branchy) and
 * (b) repeated lookup of the same small key-set (predictor-friendly).
 *
 * Build: cc -O2 -std=gnu11 -o /tmp/probe2 v-perf-branchless-search.c
 * Inspect: objdump -d /tmp/probe2 --disassembler=symbol
 */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLEAR_VALUE_PROPAGATION(VAR) __asm__ __volatile__("" : "+r"(VAR))
#define BEGIN(IT, LOWER, SIZE)                                                                                         \
  intptr_t adjust = SIZE - 1, half = adjust >> 1;                                                                      \
  adjust &= 1;                                                                                                         \
  IT = LOWER + half
#define END(IT, CMP, LOWER, SIZE)                                                                                      \
  CMP >>= CHAR_BIT * sizeof(CMP) - 1;                                                                                  \
  CLEAR_VALUE_PROPAGATION(CMP);                                                                                        \
  LOWER += (half + 1) & CMP;                                                                                           \
  SIZE = half + (CMP & adjust)

/* replica of SEARCH_BRANCH(uint64): lo=1 start, early-exit on exact */
__attribute__((noinline)) static size_t search_branchless(const uint64_t *keys, const size_t nkeys,
                                                          const uint64_t key) {
  intptr_t lo = 1, scope = nkeys - lo, it, cmp;
  if (nkeys > 1)
    do {
      BEGIN(it, lo, scope);
      cmp = (keys[it] != key) ? ((keys[it] < key) ? -1 : 1) : 0;
      if (cmp == 0)
        return (size_t)it;
      END(it, cmp, lo, scope);
    } while (scope > 0);
  return (size_t)(lo - 1);
}

/* same, but with the CLEAR_VALUE_PROPAGATION barrier applied to the
 * comparator result BEFORE the exact-match test, mirroring the real macro
 * (the real code's comparator result goes through the barrier in END) */
__attribute__((noinline)) static size_t search_branchless_bar(const uint64_t *keys, const size_t nkeys,
                                                              const uint64_t key) {
  intptr_t lo = 1, scope = nkeys - lo, it, cmp;
  if (nkeys > 1)
    do {
      BEGIN(it, lo, scope);
      cmp = (keys[it] != key) ? ((keys[it] < key) ? -1 : 1) : 0;
      CLEAR_VALUE_PROPAGATION(cmp);
      if (cmp == 0)
        return (size_t)it;
      END(it, cmp, lo, scope);
    } while (scope > 0);
  return (size_t)(lo - 1);
}

/* replica of the SEARCH_IMPL cmov bsearch from sort.h (pnl_search) */
__attribute__((noinline)) static const uint64_t *bsearch_cmov(const uint64_t *it, size_t length, uint64_t item) {
  const uint64_t *const begin = it;
  do {
    const uint64_t *const middle = it + (length >> 1);
    length = (length + 1) >> 1;
    const bool flag = (middle[0] < item);
    CLEAR_VALUE_PROPAGATION(length);
    it = flag ? middle : it;
  } while (length > 2);
  it += (length > 1) && (it[0] < item);
  it += (length > 0) && (it[0] < item);
  (void)begin;
  return it;
}

/* classic branchy binary search (lower bound) */
__attribute__((noinline)) static size_t search_classic(const uint64_t *keys, size_t nkeys, uint64_t key) {
  size_t lo = 0, hi = nkeys;
  while (lo < hi) {
    size_t mid = (lo + hi) >> 1;
    if (keys[mid] < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

static uint64_t nsec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t rng_state = 42;
static uint64_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

static void run(const char *what, const uint64_t *keys, size_t nkeys, const uint64_t *queries, int nq, int iters) {
  uint64_t t0, t1;
  volatile size_t sink = 0;
  size_t acc = 0;

  t0 = nsec();
  for (int i = 0; i < iters; ++i)
    for (int q = 0; q < nq; ++q)
      acc ^= search_branchless(keys, nkeys, queries[q]);
  t1 = nsec();
  const double ns_bl = (double)(t1 - t0) / ((double)iters * nq);
  sink ^= acc;
  acc = 0;

  t0 = nsec();
  for (int i = 0; i < iters; ++i)
    for (int q = 0; q < nq; ++q)
      acc ^= search_branchless_bar(keys, nkeys, queries[q]);
  t1 = nsec();
  const double ns_blb = (double)(t1 - t0) / ((double)iters * nq);
  sink ^= acc;
  acc = 0;

  t0 = nsec();
  for (int i = 0; i < iters; ++i)
    for (int q = 0; q < nq; ++q)
      acc ^= search_classic(keys, nkeys, queries[q]);
  t1 = nsec();
  const double ns_cl = (double)(t1 - t0) / ((double)iters * nq);
  sink ^= acc;

  t0 = nsec();
  for (int i = 0; i < iters; ++i)
    for (int q = 0; q < nq; ++q)
      acc ^= (size_t)(bsearch_cmov(keys, nkeys, queries[q]) - keys);
  t1 = nsec();
  const double ns_cm = (double)(t1 - t0) / ((double)iters * nq);
  sink ^= acc;

  printf("%-22s n=%6zu: branchless %6.1f | +barrier %6.1f | classic %6.1f | cmov-lb %6.1f ns\n", what, nkeys, ns_bl,
         ns_blb, ns_cl, ns_cm);
  (void)sink;
}

int main(void) {
  enum { MAXN = 8192, NQ = 1 << 20 };
  static uint64_t keys[MAXN];
  static uint64_t *queries = NULL;
  queries = malloc((size_t)NQ * sizeof(uint64_t));
  for (size_t i = 0; i < MAXN; ++i)
    keys[i] = 0x4000000000000000u + i * 16;

  /* uniform random queries, unique stream (no repetition => the branch
   * predictor cannot learn the pattern across iterations) */
  for (int q = 0; q < NQ; ++q)
    queries[q] = keys[rng() % MAXN] + (rng() % 8);

  /* skewed: repeat 16 hot keys inside the stream (predictor-friendly) */
  for (int q = 0; q < NQ; ++q)
    if ((q & 3) == 0)
      queries[q] = keys[(q % 16) * (MAXN / 16)] + (q % 3);

  run("branch fanout", keys, 16, queries, NQ, 2);
  run("branch fanout big", keys, 128, queries, NQ, 2);
  run("leaf small", keys, 1024, queries, NQ, 2);
  run("leaf max", keys, MAXN, queries, NQ, 2);
  run("skewed/hot mixed", keys, 1024, queries, NQ, 2);
  free(queries);
  return 0;
}
