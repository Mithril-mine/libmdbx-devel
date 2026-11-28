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

/// gc-dense-solver-unbounded-stall regression test (issue #497, see the PR description).
///
/// The dense solver is exercised through the testing API
/// (mdbx_tst_gc_dense_pack(), tests/api.h): the REAL src/gc-dense.h closed
/// form compiled into the library - not a copy, per tests/AGENTS.md §3.
/// Contracts under test:
/// 1. BOUNDED WORK: the closed-form solver is structurally O(hist^2); the
///    pre-fix exponential binary-search predicate took 71,895,998
///    invocations (~50s at -O2) on the counterexample below, inside a single
///    gc_rerere call with the writer mutex held. Boundedness here is
///    structural and enforced by the ctest TIMEOUT (a reintroduced search
///    would blow it), not by a counter.
/// 2. FEASIBILITY EQUIVALENCE: the solver's found/not-found verdict must
///    match an independent exact oracle (per-run partition dynamic
///    programming, written from the specification - allowed by §3) over a
///    deterministic corpus - never false-negative (a missed feasible shape
///    sends gc_handle_dense() to the legacy fallback, degrading GC slot
///    usage) and never false-positive (the solution self-check would fail
///    the transaction).
/// 3. SOLUTION SOUNDNESS: any found solution covers the volume with no more
///    than the available slots.

#include "mdbx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stand-ins for the txn/env-derived values (4K pages) */
static const size_t tst_ps = 4096;
static const size_t tst_maxgc_large1page = (4096u - MDBX_TST_PAGEHDRSZ) / sizeof(uint32_t) - 1;

static size_t tst_first_page(void) { return tst_maxgc_large1page + 1; }
static size_t tst_other_pages(void) { return tst_ps / sizeof(uint32_t) + 1; }

/* thin adapter over the real gc_dense_pack(): 0 = done (see *found), <0 = self-check failure */
static int tst_dense_solve(size_t slots, size_t volume, const uint32_t *hist_counts, unsigned hist_end,
                           uint32_t *solution_counts, unsigned *solution_end, bool *found) {
  return mdbx_tst_gc_dense_pack(slots, volume, hist_counts, hist_end, tst_first_page(), tst_other_pages(),
                                solution_counts, solution_end, found);
}

/*-----------------------------------------------------------------------------*/

/* --- the independent exact oracle: per-run partition DP ---
 * For every run, all partitions into pieces (compositions of lengths <= L)
 * are enumerated; the DP keeps the Pareto-max coverage per used-slots count.
 * This is exact (any allocation is a per-run partition, coverage and slots
 * are additive over runs) and written from the specification, independently
 * of the solver's math. */
static bool tst_oracle(const uint32_t *h, unsigned h_end, size_t slots, size_t volume, size_t first_page,
                       size_t other_pages, size_t *min_slots) {
  static size_t dp[64 + 1], next[64 + 1];
  const size_t cap = slots < 64 ? slots : 64;
  for (size_t k = 0; k <= cap; ++k)
    dp[k] = 0;
  unsigned runs[64], nruns = 0;
  for (unsigned len = 1; len <= h_end; ++len)
    for (uint32_t c = 0; c < h[len - 1] && nruns < 64; ++c)
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

static void tst_solve(const char *name, size_t left_slots, size_t left_volume, const uint32_t *hist_counts,
                      unsigned hist_end, bool expect_found) {
  ++tst_cases;
  uint32_t solution_counts[31];
  unsigned solution_end = 0;
  bool found = false;
  const int rc =
      tst_dense_solve(left_slots, left_volume, hist_counts, hist_end, solution_counts, &solution_end, &found);
  char what[96];
  snprintf(what, sizeof(what), "%s: rc", name);
  tst_report(what, rc == 0, "rc %d", rc);
  snprintf(what, sizeof(what), "%s: outcome", name);
  tst_report(what, rc == 0 && found == expect_found, "found %d, want %d", (int)found, (int)expect_found);
  if (found) {
    size_t items = 0, covered = 0, chunk = tst_first_page();
    for (unsigned i = 0; i < solution_end; ++i) {
      items += solution_counts[i];
      covered += solution_counts[i] * chunk;
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
   * both before and after - under the closed form it is decided in
   * microseconds. */
  {
    uint32_t h[31] = {0};
    unsigned end = 24;
    h[1] = 42;
    h[6] = 143;
    h[16] = 87;
    h[17] = 17;
    h[23] = 9;
    tst_solve("counterexample", 1021, 3557022, h, end, false);
  }

  /* 2) the author's own TRUE example (gc_solve_test, the pre-fix commented
   * code): a legit solve must stay found and cheap. */
  {
    uint32_t h[31] = {0};
    unsigned end = 31;
    const uint32_t src[31] = {6493, 705, 120, 14, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                              0,    0,   0,   0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4};
    memcpy(h, src, sizeof(src));
    tst_solve("author-example", 5, 8463, h, end, true);
  }

  /* 3) deep-but-feasible and boundary-infeasible shapes */
  {
    uint32_t h[31] = {0};
    unsigned end = 20;
    const uint32_t src[20] = {5, 0, 3, 0, 0, 7, 0, 0, 4, 0, 0, 6, 0, 8, 0, 0, 2, 0, 5, 3};
    memcpy(h, src, sizeof(src));
    tst_solve("deep-feasible", 80, 150000, h, end, true);
  }
  {
    uint32_t h[31] = {0};
    unsigned end = 12;
    const uint32_t src[12] = {0, 8, 0, 21, 0, 0, 5, 0, 9, 0, 3, 1};
    memcpy(h, src, sizeof(src));
    tst_solve("boundary-infeasible", 60, 270000, h, end, false);
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
      uint32_t h[31] = {0};
      for (unsigned i = 0; i < end; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        h[i] = (uint32_t)((seed >> 45) % 3u);
      }
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      const size_t slots = 1 + (seed >> 60) % 10u;
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      const size_t volume = 1 + (seed >> 50) % 120000u;
      size_t oracle_min = 0;
      const bool oracle = tst_oracle(h, end, slots, volume, tst_first_page(), tst_other_pages(), &oracle_min);
      uint32_t solution_counts[31];
      unsigned solution_end = 0;
      bool found = false;
      const int rc = tst_dense_solve(slots, volume, h, end, solution_counts, &solution_end, &found);
      const bool solver_ok = (rc == 0) && found;
      ++tst_cases;
      char what[80];
      snprintf(what, sizeof(what), "corpus#%d: verdict matches the oracle", it);
      tst_report(what, (rc == 0) && solver_ok == oracle, "solver %d oracle %d (end=%u slots=%zu volume=%zu)",
                 (int)solver_ok, (int)oracle, end, slots, volume);
      if (rc == 0 && solver_ok == oracle) {
        ++agree;
        if (solver_ok) {
          size_t items = 0;
          for (unsigned i = 0; i < solution_end; ++i)
            items += solution_counts[i];
          snprintf(what, sizeof(what), "corpus#%d: items sane", it);
          tst_report(what, items >= oracle_min && items <= slots, "items %zu, oracle min %zu", items, oracle_min);
        }
      } else {
        ++mismatch;
        if (mismatch <= 5)
          fprintf(stderr, "    mismatch: end=%u slots=%zu volume=%zu solver=%d oracle=%d\n", end, slots, volume,
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
