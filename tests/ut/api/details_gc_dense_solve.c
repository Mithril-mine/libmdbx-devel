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

/// V-004 regression test (issue #497, see the PR description).
///
/// Contracts under test:
/// 1. BOUNDED WORK: the dense solver must never explode (the pre-fix
///    exponential binary-search predicate took 71,895,998 invocations, ~50s
///    at -O2, on the counterexample below - inside a single gc_rerere call,
///    with the writer mutex held). The closed-form solver is O(hist^2).
/// 2. FEASIBILITY EQUIVALENCE: the solver's found/not-found verdict must
///    match an independent exact oracle (per-run partition dynamic
///    programming) over a deterministic corpus - the solver is never
///    false-negative (a missed feasible shape sends gc_handle_dense() to the
///    legacy fallback, degrading GC slot usage) and never false-positive
///    (the solution self-check would fail the transaction).
/// 3. SOLUTION SOUNDNESS: any found solution covers the volume with no more
///    than the available slots.
///
/// This is a verbatim-extraction test in the details_scan4seq.c style: the
/// solver below is copied from src/gc-put.c @ the fix commit with the
/// txn/env-derived values hoisted into stand-in constants. Keep the
/// extraction in sync when refactoring the solver.

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

/* the work counter for the bounded-work contract (a test-only seam) */
static unsigned long long tst_calls;

/* --- dense histogram (verbatim shape, src/gc.h) --- */
typedef struct gc_dense_histogram {
  unsigned end;
  pgno_t array[31];
} gc_dense_histogram_t;

/* --- VERBATIM from src/gc-put.c @ the fix commit: keep in sync --- */

/* Решение задачи плотной упаковки: покрыть объём return_left страниц, используя не более
 * slots доступных слотов/идентификаторов, вырезая куски (GC-записи со спаном len, т.е. размещённые
 * на len смежных страницах) из прогонов смежных страниц гистограммы gc_dense_hist().
 *
 * Ключевое наблюдение, сводящее задачу к закрытой форме:
 *  1. Вместимость куска строго линейна по спану: per_chunk(len) = first_page + other_pages*(len-1),
 *     т.е. other_pages*len - (other_pages - first_page). Следовательно, покрытие любого набора кусков
 *     есть other_pages*sum(len_i) - (other_pages-first_page)*k, где k - число кусков.
 *  2. Прогон длиной L можно нарезать на любые куски с суммой спанов <= L (остатки нарезки остаются
 *     смежными и пригодны для дальнейших кусков). Поэтому любые k кусков покрывают не более
 *     other_pages*P(k) - (other_pages-first_page)*k, где P(k) - сумма длин k наибольших прогонов,
 *     и эта величина достигается взятием прогонов целиком.
 *
 * Отсюда точный критерий разрешимости: задача разрешима тогда и только тогда, когда существует
 * k <= slots с other_pages*P(k) - (other_pages-first_page)*k >= объёма. Проверяется сканом префиксных
 * сумм отсортированных (подсчётом) длин прогонов - без какого-либо перебора.
 *
 * Решение строится в двух режимах:
 *  - Равномерный (предпочтительный): минимальный спан m, при котором объём покрывается n(m) =
 *    ceil(объём / per_chunk(m)) кусками спана m, размещаемыми в прогонах (floor(len/m) кусков из
 *    прогона длиной len) с учётом числа слотов. Такие записи однородны и максимально коротки, что
 *    упрощает их последующую переработку. Может не существовать при дефиците слотов.
 *  - Минимум слотов (точный): жадно от самых длинных прогонов целыми кусками, последний кусок
 *    подрезается до минимально необходимого спана. Существует всегда, когда задача разрешима.
 * Если ни один режим не дал решения - задача неразрешима, возвращаем MDBX_RESULT_FALSE и вызывающий
 * gc_handle_dense() откатывается на штатные пути резервирования. */
static int tst_dense_solve(size_t slots, size_t volume, const gc_dense_histogram_t *hist,
                           gc_dense_histogram_t *const solution, bool *found) {
  ASSERT(slots > 0 && volume > 0);
  if (!slots || !volume)
    return -1;

  const size_t first_page = /* на первой странице */ tst_maxgc_large1page +
                            /* сама страница также будет израсходована */ 1;
  const size_t other_pages = /* на второй и последующих страницах */ tst_ps / sizeof(pgno_t) +
                             /* каждая страница также будет израсходована */ 1;

  memset(solution, 0, sizeof(*solution));
  ++tst_calls;

  /* Режим А (равномерный): сканируем спаны от коротких к длинам прогонов. */
  unsigned span = 0;
  for (unsigned m = 1; m <= 31; ++m) {
    const size_t per_chunk = first_page + other_pages * (m - 1);
    const size_t n = (volume + per_chunk - 1) / per_chunk;
    if (n > slots)
      continue;
    size_t pieces = 0;
    for (unsigned len = m; len <= hist->end; ++len)
      pieces += (size_t)hist->array[len - 1] * (len / m);
    if (n <= pieces) {
      span = m;
      solution->array[m - 1] = (pgno_t)n;
      break;
    }
  }

  if (!span) {
    /* Режим Б (минимум слотов): точный, существует всегда при разрешимости задачи. */
    size_t remaining = volume, used_slots = 0;
    unsigned max_span = 0;
    for (unsigned len = hist->end; len >= 1 && remaining > 0; --len) {
      ++tst_calls;
      const size_t per_chunk = first_page + other_pages * (len - 1);
      for (pgno_t c = hist->array[len - 1]; c > 0 && remaining > 0; --c) {
        unsigned cut = len;
        if (per_chunk > remaining) {
          /* подрезаем последний кусок до минимально необходимого спана */
          cut = (unsigned)(1 + (remaining > first_page ? (remaining - first_page + other_pages - 1) / other_pages : 0));
          ASSERT(cut >= 1 && cut <= len);
        }
        solution->array[cut - 1] += 1;
        if (cut > max_span)
          max_span = cut;
        remaining -= (per_chunk < remaining) ? per_chunk : remaining;
        used_slots += 1;
      }
    }
    if (remaining > 0 || used_slots > slots)
      /* решение НЕ найдено */
      return 1;
    span = max_span;
  }

  {
    const pgno_t *end = solution->array + 31;
    while (end > solution->array && end[-1] == 0)
      --end;
    solution->end = (unsigned)(end - solution->array);
    (void)span;
  }

  /* проверяем решение */
  size_t items = 0, covered = 0;
  for (size_t i = 0, chunk = first_page; i < solution->end; ++i) {
    items += solution->array[i];
    covered += solution->array[i] * chunk;
    chunk += other_pages;
  }
  if (covered < volume || items > slots)
    return -2;
  *found = true;
  return 0;
}

/*-----------------------------------------------------------------------------*/

/* --- the independent exact oracle: per-run partition DP ---
 * For every run, all partitions into pieces (compositions of lengths <= L)
 * are enumerated; the DP keeps the Pareto-max coverage per used-slots count.
 * This is exact (any allocation is a per-run partition, coverage and slots
 * are additive over runs) and completely independent of the solver's math. */
static bool tst_oracle(const gc_dense_histogram_t *h, size_t slots, size_t volume, size_t first_page,
                       size_t other_pages, size_t *min_slots) {
  static size_t dp[64 + 1], next[64 + 1];
  const size_t cap = slots < 64 ? slots : 64;
  for (size_t k = 0; k <= cap; ++k)
    dp[k] = 0;
  unsigned runs[64], nruns = 0;
  for (unsigned len = 1; len <= h->end; ++len)
    for (pgno_t c = 0; c < h->array[len - 1] && nruns < 64; ++c)
      runs[nruns++] = len;
  for (unsigned r = 0; r < nruns; ++r) {
    const unsigned L = runs[r];
    for (size_t k = 0; k <= cap; ++k)
      next[k] = dp[k];
    /* enumerate all partitions of L into pieces >= 1 */
    unsigned rest = L, first_piece = 1;
    unsigned pieces[16];
    unsigned depth = 0;
    while (true) {
      if (rest == 0) {
        /* a complete partition of exactly L */
        const size_t count = depth;
        size_t cov = 0;
        for (unsigned i = 0; i < depth; ++i)
          cov += first_page + other_pages * (pieces[i] - 1);
        for (size_t k = 0; k + count <= cap; ++k)
          if (dp[k] + cov > next[k + count])
            next[k + count] = dp[k] + cov;
      }
      if (first_piece > rest) {
        if (depth == 0)
          break;
        depth -= 1;
        rest += pieces[depth];
        first_piece = pieces[depth] + 1;
        continue;
      }
      pieces[depth] = first_piece;
      rest -= first_piece;
      depth += 1;
      first_piece = 1;
      assert(depth < 16);
    }
    memcpy(dp, next, (cap + 1) * sizeof(dp[0]));
  }
  for (size_t k = 0; k <= cap; ++k)
    if (dp[k] >= volume) {
      if (min_slots)
        *min_slots = k;
      return true;
    }
  return false;
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

/* the work cap asserted by the contract (loose; the closed form is O(31^2)) */
static const unsigned long long tst_work_cap = 1000000ull;

static size_t tst_first_page(void) { return tst_maxgc_large1page + 1; }
static size_t tst_other_pages(void) { return tst_ps / sizeof(pgno_t) + 1; }

static void tst_solve(const char *name, size_t left_slots, size_t left_volume, const gc_dense_histogram_t *hist,
                      bool expect_found) {
  ++tst_cases;
  gc_dense_histogram_t solution;
  bool found = false;
  const int rc = tst_dense_solve(left_slots, left_volume, hist, &solution, &found);
  char what[96];
  snprintf(what, sizeof(what), "%s: rc", name);
  tst_report(what, rc == 0 || rc == 1, "rc %d", rc); /* 0 = found, 1 = infeasible, else error */
  snprintf(what, sizeof(what), "%s: outcome", name);
  tst_report(what, (rc == 0 || rc == 1) && found == expect_found, "found %d, want %d", (int)found,
             (int)expect_found);
  snprintf(what, sizeof(what), "%s: bounded work", name);
  tst_report(what, tst_calls <= tst_work_cap, "calls %llu > cap %llu", tst_calls, tst_work_cap);
  if (found) {
    size_t items = 0, covered = 0, chunk = tst_first_page();
    for (unsigned i = 0; i < solution.end; ++i) {
      items += solution.array[i];
      covered += solution.array[i] * chunk;
      chunk += tst_other_pages();
    }
    snprintf(what, sizeof(what), "%s: solution sane", name);
    tst_report(what, items >= 1 && items <= left_slots && covered >= left_volume, "items %zu covered %zu", items,
               (size_t)covered);
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* 1) the wave-campaign counterexample: pre-fix this took 71,895,998
   * predicate invocations (~50s at -O2) before returning FALSE; the outcome
   * itself is FALSE (the material, 3086 pages, cannot cover the volume)
   * both before and after - the contracts under test are the bounded work
   * and the correct verdict. */
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

  /* 2) the author's own TRUE example (gc_solve_test, the pre-fix commented
   * code): a legit solve must stay found and cheap. */
  {
    gc_dense_histogram_t h = {0};
    h.end = 31;
    const pgno_t src[31] = {6493, 705, 120, 14, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                            0,    0,   0,   0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4};
    memcpy(h.array, src, sizeof(src));
    tst_solve("author-example", 5, 8463, &h, true);
  }

  /* 3) deep-but-feasible and boundary-infeasible shapes */
  {
    gc_dense_histogram_t h = {0};
    h.end = 20;
    const pgno_t src[20] = {5, 0, 3, 0, 0, 7, 0, 0, 4, 0, 0, 6, 0, 8, 0, 0, 2, 0, 5, 3};
    memcpy(h.array, src, sizeof(src));
    tst_solve("deep-feasible", 80, 150000, &h, true);
  }
  {
    gc_dense_histogram_t h = {0};
    h.end = 12;
    const pgno_t src[12] = {0, 8, 0, 21, 0, 0, 5, 0, 9, 0, 3, 1};
    memcpy(h.array, src, sizeof(src));
    tst_solve("boundary-infeasible", 60, 270000, &h, false);
  }

  /* 4) the deterministic corpus vs the independent exact oracle: the
   * solver's verdict must match feasibility exactly (never false-negative:
   * that would silently degrade to the legacy fallback; never
   * false-positive: the self-check would fail the transaction). */
  {
    unsigned long long seed = 20261116;
    unsigned agree = 0, mismatch = 0;
    for (int it = 0; it < 800; ++it) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      const unsigned end = 1 + (unsigned)((seed >> 40) % 6u);
      gc_dense_histogram_t h = {0};
      h.end = end;
      for (unsigned i = 0; i < end; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        h.array[i] = (pgno_t)((seed >> 45) % 3u);
      }
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      const size_t slots = 1 + (seed >> 60) % 10u;
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      const size_t volume = 1 + (seed >> 50) % 120000u;
      size_t oracle_min = 0;
      const bool oracle = tst_oracle(&h, slots, volume, tst_first_page(), tst_other_pages(), &oracle_min);
      gc_dense_histogram_t solution;
      bool found = false;
      const int rc = tst_dense_solve(slots, volume, &h, &solution, &found);
      const bool solver_ok = (rc == 0) && found;
      ++tst_cases;
      char what[80];
      snprintf(what, sizeof(what), "corpus#%d: verdict matches the oracle", it);
      tst_report(what, (rc == 0 || rc == 1) && solver_ok == oracle,
                 "solver %d oracle %d (end=%u slots=%zu volume=%zu)", (int)solver_ok, (int)oracle, h.end, slots,
                 volume);
      if ((rc == 0 || rc == 1) && solver_ok == oracle) {
        ++agree;
        if (solver_ok) {
          size_t items = 0;
          for (unsigned i = 0; i < solution.end; ++i)
            items += solution.array[i];
          snprintf(what, sizeof(what), "corpus#%d: items sane", it);
          tst_report(what, items >= oracle_min && items <= slots, "items %zu, oracle min %zu", items, oracle_min);
        }
      } else {
        ++mismatch;
        if (mismatch <= 5)
          fprintf(stderr, "    mismatch: end=%u slots=%zu volume=%zu solver=%d oracle=%d\n", h.end, slots, volume,
                  (int)solver_ok, (int)oracle);
      }
    }
    printf("corpus: agree=%u mismatch=%u\n", agree, mismatch);
  }

  printf("cases=%zu ok=%zu failed=%zu\n", tst_cases, tst_ok, tst_failed);
  if (tst_failed) {
    fprintf(stderr, "FAILED %zu checks\n", tst_failed);
    return 1;
  }
  return 0;
}
