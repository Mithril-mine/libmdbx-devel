/* v-perf-pnl-merge-cmov.c — analytical probe, stream D (performance audit).
 *
 * Question 1: does the pnl_merge_inner() CMOV variant actually compile
 * branchless with gcc >= 7 (the in-source comment claims gcc>=7 emits
 * "cmp+jmp with return into the loop body (WTF?)" for the ternary form,
 * which is why the pointer-arithmetic form is used for gcc)?
 *
 * Question 2: is the branchless merge actually faster than a branchy merge
 * for the GC-reclaim merge pattern (two interleaved descending lists)?
 *
 * Build:  cc -O2 -std=gnu11 -o /tmp/probe1 v-perf-pnl-merge-cmov.c
 * Inspect: objdump -d /tmp/probe1 | awk '/<merge_ternary>|<merge_ptr>|<merge_branchy>/,/ret/'
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MDBX_HAVE_CMOV 1
#define MDBX_DISABLE_VALIDATION 1
#define ORDERED(first, last) ((first) > (last))

typedef uint32_t pgno_t;

/* --- variant A: ternary form (what clang>=13/lcc get) --- */
__attribute__((noinline)) static size_t merge_ternary(pgno_t *__restrict dst, const pgno_t *__restrict a,
                                                      const pgno_t *__restrict b,
                                                      const pgno_t *__restrict const b_detent) {
  do {
    const bool flag = ORDERED(*b, *a);
#if defined(__LCC__) || __clang_major__ >= 13
    *dst = flag ? *a-- : *b--;
#else
    /* gcc path: pointer arithmetic instead of ternary on the deref */
    *dst = flag ? *a : *b;
    b += (ptrdiff_t)flag - 1;
    a -= flag;
#endif
    --dst;
  } while (b > b_detent);
  return 0;
}

/* --- variant B: branchy merge (what a naive implementation would be) --- */
__attribute__((noinline)) static size_t merge_branchy(pgno_t *__restrict dst, const pgno_t *__restrict a,
                                                      const pgno_t *__restrict b,
                                                      const pgno_t *__restrict const b_detent) {
  while (ORDERED(*b, *a))
    *dst-- = *a--;
  *dst-- = *b--;
  while (b > b_detent) {
    while (ORDERED(*b, *a))
      *dst-- = *a--;
    *dst-- = *b--;
  }
  return 0;
}

static uint64_t nsec(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

#define N_A 2048
#define N_B 2048
static pgno_t list_a[N_A], list_b[N_B], out[N_A + N_B + 1];

static uint64_t rng_state = 12345;
static uint64_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* Build two strictly descending, MUTUALLY COLLISION-FREE lists whose merge
 * order is a uniformly random interleaving (each round: one element from a
 * random list). This is the predictor-hostile case; a run-based interleave
 * would let the branchy variant win via prediction. Parity separation (odd
 * decrements for a, even for b) removes ties so the reference merge and the
 * tested forms must agree exactly. */
static void build_random_interleaved(void) {
  size_t ia = 0, ib = 0;
  pgno_t pool[N_A + N_B];
  static size_t owner[N_A + N_B];
  pgno_t va = 1u << 30, vb = (1u << 30) - 1;
  rng_state = 12345;
  while (ia < N_A || ib < N_B) {
    const bool take_a = (ib >= N_B) || (ia < N_A && (rng() & 1));
    if (take_a) {
      va -= 1u + 2u * (pgno_t)(rng() % 4); /* odd decrement => a stays even */
      pool[ia + ib] = va;
      owner[ia + ib] = 0;
      ia += 1;
    } else {
      vb -= 2u * (1u + (pgno_t)(rng() % 4)); /* even decrement => b stays odd */
      pool[ia + ib] = vb;
      owner[ia + ib] = 1;
      ib += 1;
    }
  }
  ia = ib = 0;
  for (size_t k = 0; k < N_A + N_B; ++k) {
    if (owner[k] == 0)
      list_a[ia++] = pool[k];
    else
      list_b[ib++] = pool[k];
  }
}

int main(void) {
  build_random_interleaved();

  /* sanity: both lists strictly descending */
  for (size_t i = 1; i < N_A; ++i)
    if (list_a[i - 1] <= list_a[i]) {
      printf("SANITY FAIL: list_a not descending at %zu\n", i);
      return 1;
    }
  for (size_t i = 1; i < N_B; ++i)
    if (list_b[i - 1] <= list_b[i]) {
      printf("SANITY FAIL: list_b not descending at %zu\n", i);
      return 1;
    }

  /* Faithful layout (mirrors pnl_merge): a IS the prefix of the output
   * buffer, out[0] is the P_INVALID-style detent consumed when a's frontier
   * is exhausted, out[1..N_A] holds a, and the merged output overwrites
   * out[1 .. N_A+N_B]. list_a keeps the pristine copy for restoration. */
  static pgno_t list_a_copy[N_A];
  memcpy(list_a_copy, list_a, sizeof(list_a));

  const int iters = 400000;
  uint64_t t0, t1;
  volatile size_t sink = 0;

  t0 = nsec();
  for (int i = 0; i < iters; ++i) {
    memcpy(out + 1, list_a_copy, sizeof(list_a));
    out[0] = (pgno_t)0xFFFFFFFF; /* dst[0]=P_INVALID detent as pnl_merge does */
    merge_ternary(out + N_A + N_B, out + N_A, list_b + N_B - 1, list_b - 1);
    sink ^= out[N_A + N_B];
  }
  t1 = nsec();
  printf("merge_ptr   : %lu ns/iter (%.2f ns per merged element, balanced %zu+%zu random interleave)\n",
         (unsigned long)((t1 - t0) / iters), (double)(t1 - t0) / iters / (N_A + N_B), (size_t)N_A, (size_t)N_B);

  /* verify merge_ptr output against an independent backward reference merge
   * with the same write window (*dst before --dst => first element at
   * out[N_A + N_B]); a-exhaustion takes b, mirroring the sentinel. */
  static pgno_t refout[N_A + N_B + 1];
  size_t ra = N_A - 1, rb = N_B - 1, wi = N_A + N_B;
  while (rb + 1 > 0) {
    if (ra + 1 > 0 && list_a_copy[ra] < list_b[rb])
      refout[wi--] = list_a_copy[ra--];
    else
      refout[wi--] = list_b[rb--];
  }
  const size_t written = N_A + N_B - wi; /* elements written by the merge */
  const size_t a_left = ra + 1;          /* a-elements left in place below the window */
  if (memcmp(out + wi + 1, refout + wi + 1, written * sizeof(pgno_t)) != 0) {
    printf("SANITY FAIL: merge output mismatch (written=%zu, a_left=%zu)\n", written, a_left);
    return 1;
  }
  printf("sanity: merge output correct (written %zu elems, %zu a-elements consumed in-loop, %zu left in place)\n",
         written, written - (size_t)N_B, a_left);

  t0 = nsec();
  for (int i = 0; i < iters; ++i) {
    memcpy(out + 1, list_a_copy, sizeof(list_a));
    out[0] = (pgno_t)0xFFFFFFFF; /* detent for the a-exhaustion case */
    merge_branchy(out + N_A + N_B, out + N_A, list_b + N_B - 1, list_b - 1);
    sink ^= out[N_A + N_B];
  }
  t1 = nsec();
  printf("merge_branchy: %lu ns/iter (%.2f ns per merged element)\n", (unsigned long)((t1 - t0) / iters),
         (double)(t1 - t0) / iters / (N_A + N_B));
  (void)sink;
  return 0;
}
