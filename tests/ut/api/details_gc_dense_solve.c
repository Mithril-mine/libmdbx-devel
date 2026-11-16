/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/// V-004 regression test (issue reference: see the PR description).
///
/// Contract: the dense-solver's binary search predicate must stay BOUNDED in
/// work. solve_recursive() uses a full recursive descent as its binary-search
/// predicate, so the work is multiplicative across the histogram levels; on
/// adversarial/malformed shapes the FALSE outcome takes an exploding number of
/// invocations (measured: 71,895,998 calls, ~50 seconds, inside one
/// gc_rerere call - the ctx->loop bound never applies and the commit thread
/// holds the writer mutex).
///
/// The fix puts a shared work budget into the recursion; on exhaustion the
/// predicate returns false ("infeasible"), which is fail-safe: the caller
/// falls back to the legacy reservation paths, and the solution self-check
/// still guards whatever is produced.
///
/// This is a verbatim-extraction test in the details_scan4seq.c style: the
/// solver below (consume_stack, consume_remaining, solve_recursive and the
/// gc_dense_solve core) is copied from src/gc-put.c @ the fix commit, with
/// the txn/env-derived values hoisted into stand-in constants and a call
/// counter added. Keep the extraction in sync when refactoring the solver.

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t pgno_t;
#define PAGEHDRSZ 20u
#define MIN_PAGENO 3
#define ASSERT(c) assert(c)
#define unlikely(c) __builtin_expect(!!(c), 0)

/* stand-ins for the txn/env-derived values (4K pages) */
static const size_t tst_ps = 4096;
static const size_t tst_maxgc_large1page = (4096u - PAGEHDRSZ) / sizeof(pgno_t) - 1;

/* the work counter: the contract under test */
static unsigned long long tst_calls;

/* --- dense histogram (verbatim shape, src/gc.h) --- */
typedef struct gc_dense_histogram {
  unsigned end;
  pgno_t array[31];
} gc_dense_histogram_t;

/* --- VERBATIM from src/gc-put.c @ the fix commit: keep in sync --- */
typedef struct sr_state {
  size_t left_slots;
  size_t left_volume;
  gc_dense_histogram_t hist;
} sr_state_t;

/* Пытается отъесть n кусков длиной len, двигаясь по гистограмме от больших элементов к меньшим. */
static bool consume_stack(sr_state_t *const st, const size_t len, size_t n) {
  ASSERT(len > 1 && n > 0);
  while (st->hist.end >= len) {
    if (st->hist.array[st->hist.end - 1] < 1)
      st->hist.end -= 1;
    else {
      if (st->hist.end > len)
        st->hist.array[st->hist.end - len - 1] += 1;
      st->hist.array[st->hist.end - 1] -= 1;
      if (--n == 0)
        return true;
    }
  }
  return false;
}

typedef struct sr_context {
  /* расход страниц / ёмкость кусков */
  pgno_t first_page, other_pages;
  /* длина последовательностей смежных страниц, при нарезке на куски соответствующей длины, имеющихся
   * слотов/идентификаторов хватит для размещения возвращаемых страниц. Нарезать куски большего размера, есть смысл
   * только если недостаточно последовательностей такой длины (с учетом более длинных, в том числе кратно длиннее). */
  pgno_t factor;
  /* результирующее решение */
  gc_dense_histogram_t *solution;
  /* общий на всю рекурсию остаток бюджета работы (см. GC_DENSE_SOLVE_BUDGET) */
  size_t *budget;
} sr_context_t;

/* Пытается покрыть остаток объёма и слотов, кусками длиной не более factor,
 * двигаясь по гистограмме от больших элементов к меньшим. */
static bool consume_remaining(const sr_context_t *const ct, sr_state_t *const st, size_t len) {
  pgno_t *const solution = ct->solution->array;
  while (len > ct->factor)
    solution[--len] = 0;
  solution[len - 1] = 0;
  if (unlikely(0 >= (int)st->left_volume))
    goto done;

  size_t per_chunk = ct->first_page + ct->other_pages * (len - 1);
  while (st->hist.end > 0 && st->left_slots > 0) {
    if (st->hist.array[st->hist.end - 1]) {
      solution[len - 1] += 1;
      if (st->hist.end > len)
        st->hist.array[st->hist.end - len - 1] += 1;
      st->hist.array[st->hist.end - 1] -= 1;
      st->left_slots -= 1;
      st->left_volume -= (pgno_t)per_chunk;
      if (0 >= (int)st->left_volume) {
      done:
        while (--len)
          solution[len - 1] = 0;
        return true;
      }
    } else {
      st->hist.end -= 1;
      if (len > st->hist.end) {
        ASSERT(len == st->hist.end + 1);
        len = st->hist.end;
        per_chunk -= ct->other_pages;
        solution[len - 1] = 0;
      }
    }
  }
  return false;
}

/* Поиск оптимального решения путем жадного бинарного деления и рекурсивного спуска по уже посчитанной гистограмме. */
static bool solve_recursive(const sr_context_t *const ct, sr_state_t *const st, size_t len) {
  if (unlikely(0 == *ct->budget))
    return false; /* budget exhausted: treat as infeasible (see GC_DENSE_SOLVE_BUDGET in src/gc-put.c) */
  *ct->budget -= 1;
  ++tst_calls; /* the counting seam (not in the source) */
  ASSERT(st->left_slots >= 1);
  size_t per_chunk = ct->first_page + ct->other_pages * (len - 1);
  if (len > ct->factor && st->left_slots > 1 && st->left_volume > per_chunk) {
    size_t lo = 0, hi = st->left_slots - 1, n = lo;
    do {
      sr_state_t local = *st;
      if (n) {
        if (!consume_stack(&local, len, n)) {
          hi = n - 1;
          n = (hi + lo) / 2;
          continue;
        }
        ASSERT(local.left_slots > n);
        local.left_slots -= n;
        local.left_volume = (pgno_t)(local.left_volume > n * per_chunk) ? local.left_volume - n * per_chunk : 0;
      }
      if (!solve_recursive(ct, &local, len - 1)) {
        lo = n + 1;
      } else if (n > lo && n < hi) {
        hi = n;
      } else {
        ct->solution->array[len - 1] = (pgno_t)n;
        *st = local;
        return true;
      }
      n = (hi + lo + 1) / 2;
    } while (hi >= lo);
    return false;
  }

  return consume_remaining(ct, st, len);
}

/* --- the gc_dense_solve core with the txn/env values hoisted (see provenance) --- */
static size_t tst_bytes2pgno(size_t bytes) { return bytes / tst_ps; }
static size_t tst_largechunk_npages(size_t bytes) { return tst_bytes2pgno(PAGEHDRSZ - 1 + bytes) + 1; }
static size_t tst_gc_chunk_pages(const size_t chunk) { return tst_largechunk_npages((chunk + 1) * sizeof(pgno_t)); }

/* returns 0 with *found set, -1 on violated preconditions, -2 on the self-check failure */
static int tst_dense_solve(size_t left_slots, size_t left_volume, const gc_dense_histogram_t *hist,
                           gc_dense_histogram_t *const solution, bool *found) {
  sr_state_t st = {.left_slots = left_slots, .left_volume = left_volume, .hist = *hist};
  ASSERT(left_slots > 0 && left_volume > 0);
  if (!st.left_slots || !st.left_volume)
    return -1;

  size_t budget = 262144; /* GC_DENSE_SOLVE_BUDGET (src/gc-put.c) */
  const sr_context_t ct = {.factor = (pgno_t)tst_gc_chunk_pages((st.left_volume + st.left_slots - 1) / st.left_slots),
                           .first_page = /* на первой странице */ tst_maxgc_large1page +
                                         /* сама страница также будет израсходована */ 1,
                           .other_pages = /* на второй и последующих страницах */ tst_ps / sizeof(pgno_t) +
                                          /* каждая страница также будет израсходована */ 1,
                           .solution = solution,
                           .budget = &budget};

  memset(solution, 0, sizeof(*solution));
  tst_calls = 0;
  if (solve_recursive(&ct, &st, st.hist.end)) {
    const pgno_t *end = solution->array + 31;
    while (end > solution->array && end[-1] == 0)
      --end;
    solution->end = (unsigned)(end - solution->array);

    /* проверяем решение */
    size_t items = 0, volume = 0;
    for (size_t i = 0, chunk = ct.first_page; i < solution->end; ++i) {
      items += solution->array[i];
      volume += solution->array[i] * chunk;
      chunk += ct.other_pages;
    }
    if (volume < (size_t)left_volume || items > left_slots)
      return -2;
    *found = true;
    return 0;
  }
  *found = false;
  return 0;
}

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok, tst_cases;

static void tst_report(const char *what, bool ok, const char *fmt, ...) {
  if (ok) {
    ++tst_ok;
    return;
  }
  ++tst_failed;
  fprintf(stderr, "  FAIL: %s", what);
  if (fmt && fmt[0]) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, ": ");
    vfprintf(stderr, fmt, args);
    va_end(args);
  }
  fprintf(stderr, "\n");
}

/* The work cap asserted by the contract. Loose against the solver's own
 * budget (see src/gc-put.c), tight against the unfixed explosion. */
static const unsigned long long tst_work_cap = 1000000ull;

static void tst_solve(const char *name, size_t left_slots, size_t left_volume, const gc_dense_histogram_t *hist,
                      bool expect_found) {
  ++tst_cases;
  gc_dense_histogram_t solution;
  bool found = false;
  const int rc = tst_dense_solve(left_slots, left_volume, hist, &solution, &found);
  char what[96];
  snprintf(what, sizeof(what), "%s: rc", name);
  tst_report(what, rc == 0, "rc %d", rc);
  snprintf(what, sizeof(what), "%s: outcome", name);
  tst_report(what, rc == 0 && found == expect_found, "found %d, want %d", (int)found, (int)expect_found);
  snprintf(what, sizeof(what), "%s: bounded work", name);
  tst_report(what, tst_calls <= tst_work_cap, "calls %llu > cap %llu", tst_calls, tst_work_cap);
  if (found) {
    /* the self-check equivalents: a solution, if returned, must be sound */
    size_t items = 0;
    for (unsigned i = 0; i < solution.end; ++i)
      items += solution.array[i];
    snprintf(what, sizeof(what), "%s: solution sane", name);
    tst_report(what, items >= 1 && items <= left_slots && solution.end <= 31, "items %zu", (size_t)items);
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* 1) the wave-campaign counterexample: hist {2x42, 7x143, 17x87, 18x17,
   * 24x9}, 1021 slots, volume 3557022. Pre-fix this explores ~72 million
   * predicate invocations (~50 seconds at -O2) before returning FALSE -
   * inside a single gc_rerere call, holding the writer mutex. The outcome
   * itself is FALSE both before and after the fix; the contract under test
   * is the bounded work. */
  {
    gc_dense_histogram_t h = {0};
    h.end = 24;
    h.array[1] = 42;
    h.array[6] = 143;
    h.array[16] = 87;
    h.array[17] = 17;
    h.array[23] = 9;
    tst_solve("counterexample", 1021, 3557022, &h, false);
  }

  /* 2) the author's own TRUE example (gc_solve_test, commented in gc-put.c):
   * a legit solve must stay found and cheap. */
  {
    gc_dense_histogram_t h = {0};
    h.end = 31;
    const pgno_t src[31] = {6493, 705, 120, 14, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                            0,    0,   0,   0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4};
    memcpy(h.array, src, sizeof(src));
    tst_solve("author-example", 5, 8463, &h, true);
  }

  /* 3) deep-but-feasible shapes: the binary search does real per-level work
   * and still must stay found and cheap. */
  {
    gc_dense_histogram_t h = {0};
    h.end = 20;
    const pgno_t src[20] = {5, 0, 3, 0, 0, 7, 0, 0, 4, 0, 0, 6, 0, 8, 0, 0, 2, 0, 5, 3};
    memcpy(h.array, src, sizeof(src));
    tst_solve("deep-feasible", 80, 150000, &h, true);
  }

  /* 4) a boundary-infeasible shape: FALSE is expected, but via bounded work
   * (measured 742 predicate calls), not via an exploding search. */
  {
    gc_dense_histogram_t h = {0};
    h.end = 12;
    const pgno_t src[12] = {0, 8, 0, 21, 0, 0, 5, 0, 9, 0, 3, 1};
    memcpy(h.array, src, sizeof(src));
    tst_solve("boundary-infeasible", 60, 270000, &h, false);
  }

  printf("cases=%zu ok=%zu failed=%zu\n", tst_cases, tst_ok, tst_failed);
  if (tst_failed) {
    fprintf(stderr, "FAILED %zu checks\n", tst_failed);
    return 1;
  }
  return 0;
}
