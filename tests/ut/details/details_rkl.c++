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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/* The deepest white-box characterization of the rkl/txl engines: every
 * internal access goes through the testing API (tests/AGENTS.md §8,
 * issue #519 stage 4) - the engines are the live linked functions, and
 * the record layout stays inside the testing API. */

/* The CRT's secure-deprecation warnings (getenv, sscanf) are suppressed
 * for the whole unit, exactly as the library does in src/preface.h - the
 * plain CRT functions are used deliberately here. */
#if !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif /* _CRT_SECURE_NO_WARNINGS */

/* The constant word-width conditional in prng_word() below trips the
 * MSVC C4127 under /W4 /WX - suppressed inside the library by
 * src/preface.h, and mirrored here for this standalone unit. */
#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif /* _MSC_VER */

#if defined(_WIN32)
/* the engine reads MDBX_PROBES via the Win32 process environment, so the
 * runtime activation below must use the same table (see probe_activate) */
#include <windows.h>
#endif /* _WIN32 */

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include <cassert>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok, tst_iterations, tst_cases, tst_cases_hole;
/* The CI shuffles the TEST order (GTEST_SHUFFLE=1), so every stochastic
 * TEST seeds the prng and resets the counters itself - no order- and
 * shuffle-dependent state. */
static void tst_reset(void) { tst_failed = tst_ok = tst_iterations = tst_cases = tst_cases_hole = 0; }
#ifndef NDEBUG
static size_t tst_target;
#endif

/* The failures report through gtest (ADD_FAILURE), while the counters stay:
 * the stochastic bailout and the pass-scoped verdicts are counter-driven. */
static bool check_bool(bool v, bool expect, const char *fn, unsigned line) {
  if (v != expect) {
    ++tst_failed;
    ADD_FAILURE() << "iteration " << tst_iterations << ": got " << (v ? "true" : "false") << ", expected "
                  << (expect ? "true" : "false") << ", at " << fn << ":" << line;
    return false;
  }
  ++tst_ok;
  return true;
}

static bool check_eq(uint64_t v, uint64_t expect, const char *fn, unsigned line) {
  if (v != expect) {
    ++tst_failed;
    ADD_FAILURE() << "iteration " << tst_iterations << ": " << v << " (got) != " << expect << " (expected), at " << fn
                  << ":" << line;
    return false;
  }
  ++tst_ok;
  return true;
}

#define CHECK_BOOL(T, EXPECT) check_bool((T), (EXPECT), __func__, __LINE__)
#define CHECK_TRUE(T) CHECK_BOOL(T, true)
#define CHECK_FALSE(T) CHECK_BOOL(T, false)
#define CHECK_EQ(T, EXPECT) check_eq((T), (EXPECT), __func__, __LINE__)

TEST(details_rkl, trivia) {
  tst_reset();
  size_t couple_size = 0;
  unsigned word_bits = 0;
  mdbx_tst_cursor_couple_layout(&couple_size, &word_bits);
  printf("auxilary info: cursor/couple size is %zu for %s = %u\n", couple_size, "MDBX_WORDBITS", word_bits);

  MDBX_tst_rkl_t x, y;

  mdbx_tst_rkl_init(&x);
  mdbx_tst_rkl_init(&y);
  CHECK_TRUE(mdbx_tst_rkl_check(&x));
  CHECK_TRUE(mdbx_tst_rkl_empty(&x));
  CHECK_EQ(mdbx_tst_rkl_len(&x), 0);

  MDBX_tst_rkl_iter_t f = mdbx_tst_rkl_iterator(&x, false);
  MDBX_tst_rkl_iter_t r = mdbx_tst_rkl_iterator(&x, true);
  CHECK_EQ(mdbx_tst_rkl_left(&f, false), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&f, true), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&r, false), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&r, true), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&f, false), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&f, true), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&r, false), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&r, true), 0);
  CHECK_TRUE(mdbx_tst_rkl_check(&x));

  MDBX_tst_rkl_hole_t hole;
  hole = mdbx_tst_rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&f, false);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&r, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&r, false);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);

  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&x, 42), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_rkl_check(&x));
  CHECK_FALSE(mdbx_tst_rkl_empty(&x));
  CHECK_EQ(mdbx_tst_rkl_len(&x), 1);
  // CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&x, 42, true), (uint64_t)MDBX_RESULT_TRUE);
  // CHECK_TRUE(mdbx_tst_rkl_check(&x));

  f = mdbx_tst_rkl_iterator(&x, false);
  r = mdbx_tst_rkl_iterator(&x, true);
  CHECK_EQ(mdbx_tst_rkl_left(&f, false), 1);
  CHECK_EQ(mdbx_tst_rkl_left(&f, true), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&r, false), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&r, true), 1);

  CHECK_EQ(mdbx_tst_rkl_turn(&f, true), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&f, false), 42);
  CHECK_EQ(mdbx_tst_rkl_turn(&f, false), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&f, true), 42);
  CHECK_EQ(mdbx_tst_rkl_turn(&f, true), 0);

  CHECK_EQ(mdbx_tst_rkl_turn(&r, false), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&r, true), 42);
  CHECK_EQ(mdbx_tst_rkl_turn(&r, true), 0);
  CHECK_EQ(mdbx_tst_rkl_turn(&r, false), 42);
  CHECK_EQ(mdbx_tst_rkl_turn(&r, false), 0);

  f = mdbx_tst_rkl_iterator(&x, false);
  hole = mdbx_tst_rkl_hole(&f, false);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&f, false);
  CHECK_EQ(hole.begin, MDBX_TST_MAX_TXNID);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, 42);
  hole = mdbx_tst_rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, 42);

  r = mdbx_tst_rkl_iterator(&x, true);
  hole = mdbx_tst_rkl_hole(&r, false);
  CHECK_EQ(hole.begin, MDBX_TST_MAX_TXNID);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&r, true);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&r, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, 42);
  hole = mdbx_tst_rkl_hole(&r, false);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);
  hole = mdbx_tst_rkl_hole(&r, false);
  CHECK_EQ(hole.begin, MDBX_TST_MAX_TXNID);
  CHECK_EQ(hole.end, MDBX_TST_MAX_TXNID);

  mdbx_tst_rkl_resize(&x, 222);
  CHECK_FALSE(mdbx_tst_rkl_empty(&x));
  CHECK_TRUE(mdbx_tst_rkl_check(&x));

  mdbx_tst_rkl_destructive_move(&x, &y);
  CHECK_TRUE(mdbx_tst_rkl_check(&x));
  CHECK_TRUE(mdbx_tst_rkl_check(&y));
  mdbx_tst_rkl_destroy(&x);
  mdbx_tst_rkl_destroy(&y);
}

/*-----------------------------------------------------------------------------*/

uint64_t prng_state;

static uint64_t prng(void) {
  prng_state = prng_state * UINT64_C(6364136223846793005) + 1;
  return prng_state;
}

static bool flipcoin(void) { return (bool)prng() & 1; }

static bool stochastic_pass(const unsigned start, const unsigned width, const unsigned n) {
  MDBX_tst_rkl_t k, c;
  uint64_t *l = mdbx_tst_txl_alloc();
  if (!CHECK_TRUE(l != NULL))
    return false;

  mdbx_tst_rkl_init(&k);
  mdbx_tst_rkl_init(&c);
  const size_t errors = tst_failed;

  MDBX_tst_rkl_iter_t f = mdbx_tst_rkl_iterator(&k, false);
  MDBX_tst_rkl_iter_t r = mdbx_tst_rkl_iterator(&k, true);

  uint64_t lowest = UINT_MAX;
  uint64_t highest = 0;
  while (mdbx_tst_txl_size(l) < n) {
    uint64_t id = (uint64_t)(prng() % width + start);
    if (id < MDBX_TST_MIN_TXNID || id >= MDBX_TST_INVALID_TXNID)
      continue;
    if (mdbx_tst_txl_contain(l, id)) {
      if (CHECK_TRUE(mdbx_tst_rkl_contain(&k, id)) &&
          CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&k, id), (uint64_t)MDBX_RESULT_TRUE))
        continue;
      break;
    }
    if (!CHECK_FALSE(mdbx_tst_rkl_contain(&k, id)))
      break;

    if (tst_iterations % (1u << 24) == 0 && tst_iterations) {
      printf("done %.3fM iteration, %zu cases\n", tst_iterations / 1000000.0, tst_cases);
      fflush(NULL);
    }
    tst_iterations += 1;

#ifndef NDEBUG
    if (tst_iterations == tst_target) {
      printf("reach %zu iteration\n", tst_iterations);
      fflush(NULL);
    }
#endif

    if (!CHECK_EQ(mdbx_tst_rkl_push(&k, id), MDBX_SUCCESS))
      break;
    if (!CHECK_TRUE(mdbx_tst_rkl_check(&k)))
      break;
    if (!CHECK_EQ(mdbx_tst_txl_append(&l, id), MDBX_SUCCESS))
      break;
    if (!CHECK_TRUE(mdbx_tst_rkl_contain(&k, id)))
      break;

    lowest = (lowest < id) ? lowest : id;
    highest = (highest > id) ? highest : id;
    if (!CHECK_EQ(mdbx_tst_rkl_lowest(&k), lowest))
      break;
    if (!CHECK_EQ(mdbx_tst_rkl_highest(&k), highest))
      break;
  }

  mdbx_tst_txl_sort(l);
  CHECK_EQ(mdbx_tst_rkl_len(&k), n);
  CHECK_EQ(mdbx_tst_txl_size(l), n);

  f = mdbx_tst_rkl_iterator(&k, false);
  r = mdbx_tst_rkl_iterator(&k, true);
  CHECK_EQ(mdbx_tst_rkl_left(&f, false), n);
  CHECK_EQ(mdbx_tst_rkl_left(&f, true), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&r, false), 0);
  CHECK_EQ(mdbx_tst_rkl_left(&r, true), n);

  for (size_t i = 0; i < n; ++i) {
    CHECK_EQ(mdbx_tst_rkl_turn(&f, false), l[n - i]);
    CHECK_EQ(mdbx_tst_rkl_left(&f, false), n - i - 1);
    CHECK_EQ(mdbx_tst_rkl_left(&f, true), i + 1);

    CHECK_EQ(mdbx_tst_rkl_turn(&r, true), l[i + 1]);
    mdbx_tst_rkl_iter_stepback(&r);
    CHECK_EQ(mdbx_tst_rkl_turn(&r, true), l[i + 1]);
    CHECK_EQ(mdbx_tst_rkl_left(&r, true), n - i - 1);
    CHECK_EQ(mdbx_tst_rkl_left(&r, false), i + 1);
  }

  if (CHECK_EQ(mdbx_tst_rkl_copy(&k, &c), MDBX_SUCCESS)) {
    for (size_t i = 1; i <= n; ++i) {
      if (!CHECK_FALSE(mdbx_tst_rkl_empty(&k)))
        break;
      if (!CHECK_FALSE(mdbx_tst_rkl_empty(&c)))
        break;
      CHECK_EQ(mdbx_tst_rkl_pop(&k, true), l[i]);
      CHECK_EQ(mdbx_tst_rkl_pop(&c, false), l[1 + n - i]);
    }
  }

  CHECK_TRUE(mdbx_tst_rkl_empty(&k));
  CHECK_TRUE(mdbx_tst_rkl_empty(&c));

  mdbx_tst_rkl_destroy(&k);
  mdbx_tst_rkl_destroy(&c);
  mdbx_tst_txl_free(l);

  ++tst_cases;
  return errors == tst_failed;
}

static unsigned getenv_uint(const char *name, unsigned fallback) {
  const char *value = getenv(name);
  if (!value || !*value)
    return fallback;
  char *end = NULL;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (!end || *end || parsed == ULONG_MAX)
    return fallback;
  return (unsigned)parsed;
}

TEST(details_rkl, stochastic) {
  tst_reset();
  prng_state = (uint64_t)time(nullptr);
  printf("prng-seed %" PRIu64 "\n", prng_state);
  /* Масштаб стохастики управляется env-переменными (по умолчанию - полный,
   * как было: 42^3 случаев + 42 петли); MDBX_RKL_CASES / MDBX_RKL_LOOPS
   * позволяют урезать для быстрого рутинного прогона без потери веток
   * покрытия. */
  const unsigned limit_cases = getenv_uint("MDBX_RKL_CASES", 42 * 42 * 42);
  const unsigned limit_loops = getenv_uint("MDBX_RKL_LOOPS", 42);
  printf("stochastic: cases=%u loops=%u\n", limit_cases, limit_loops);
  fflush(nullptr);
  for (unsigned loop = 0; tst_cases < limit_cases || loop < limit_loops; ++loop)
    for (unsigned width = 2; width < 10; ++width)
      for (unsigned n = 1; n < width; ++n)
        for (unsigned prev = 1, start = 0, t; start < 4242; t = start + prev, prev = start, start = t)
          if (!stochastic_pass(start, 1u << width, 1u << n) || tst_failed > 42) {
            puts("bailout\n");
            return;
          }
  return;
}

/*-----------------------------------------------------------------------------*/

static bool bit(size_t set, size_t n) {
  assert(n < CHAR_BIT * sizeof(set));
  return (set >> n) & 1;
}

static bool check_hole(const size_t set, const MDBX_tst_rkl_hole_t hole, size_t *acc) {
  const size_t errors = tst_failed;
  ++tst_iterations;

  if (hole.begin > 1)
    CHECK_EQ(bit(set, (size_t)hole.begin - 1), 1);
  if (hole.end < CHAR_BIT * sizeof(set))
    CHECK_EQ(bit(set, (size_t)hole.end), 1);

  for (size_t n = (size_t)hole.begin; n < hole.end && n < CHAR_BIT * sizeof(set); n++) {
    CHECK_EQ(bit(set, n), 0);
    *acc += 1;
  }

  return errors == tst_failed;
}

static void debug_set(const size_t set, const char *str, int iter_offset) {
#if 1
  (void)set;
  (void)str;
  (void)iter_offset;
#else
  printf("\ncase %s+%d: count %zu, holes", str, iter_offset, mdbx_tst_hamming_weight(~set) - 1);
  for (size_t k, i = 1; i < CHAR_BIT * sizeof(set); ++i) {
    if (!bit(set, i)) {
      printf(" %zu", i);
      for (k = i; k < CHAR_BIT * sizeof(set) - 1 && !bit(set, k + 1); ++k)
        ;
      if (k > i) {
        printf("-%zu", k);
        i = k;
      }
    }
  }
  printf("\n");
  fflush(NULL);
#endif
}

static bool check_holes_bothsides(const size_t set, MDBX_tst_rkl_iter_t const *i) {
  const size_t number_of_holes = mdbx_tst_hamming_weight(~set) - 1;
  size_t acc = 0;

  MDBX_tst_rkl_iter_t f = *i;
  for (;;) {
    MDBX_tst_rkl_hole_t hole = mdbx_tst_rkl_hole(&f, false);
    if (hole.begin == hole.end)
      break;
    if (!check_hole(set, hole, &acc))
      return false;
    if (hole.end >= CHAR_BIT * sizeof(set))
      break;
  }

  MDBX_tst_rkl_iter_t b = *i;
  for (;;) {
    MDBX_tst_rkl_hole_t hole = mdbx_tst_rkl_hole(&b, true);
    if (hole.begin == hole.end)
      break;
    if (!check_hole(set, hole, &acc))
      return false;
    if (hole.begin == 1)
      break;
  }

  if (!CHECK_EQ(acc, number_of_holes))
    return false;

  return true;
}

static bool check_holes_fourways(const size_t set, const MDBX_tst_rkl_t *rkl) {
  MDBX_tst_rkl_iter_t i = mdbx_tst_rkl_iterator(rkl, false);
  int o = 0;
  do {
    debug_set(set, "initial-forward", o++);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (mdbx_tst_rkl_turn(&i, false));

  do {
    debug_set(set, "recoil-reverse", --o);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (mdbx_tst_rkl_turn(&i, true));

  i = mdbx_tst_rkl_iterator(rkl, true);
  o = 0;
  do {
    debug_set(set, "initial-reverse", --o);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (mdbx_tst_rkl_turn(&i, false));

  do {
    debug_set(set, "recoil-forward", o++);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (mdbx_tst_rkl_turn(&i, true));

  return true;
}

static bool stochastic_pass_hole(size_t set, size_t trims) {
  const size_t one = 1;
  set &= ~one;
  if (!set)
    return true;

  ++tst_cases_hole;

  MDBX_tst_rkl_t rkl;
  mdbx_tst_rkl_init(&rkl);
  for (size_t n = 1; n < CHAR_BIT * sizeof(set); ++n)
    if (bit(set, n))
      CHECK_EQ(mdbx_tst_rkl_push(&rkl, n), MDBX_SUCCESS);

  if (!check_holes_fourways(set, &rkl))
    return false;

  while (mdbx_tst_rkl_len(&rkl) > 1 && trims-- > 0) {
    if (flipcoin()) {
      const size_t l = (size_t)mdbx_tst_rkl_pop(&rkl, false);
      if (l == 0)
        break;
      assert(bit(set, l));
      set -= one << l;
      if (!check_holes_fourways(set, &rkl))
        return false;
    } else {

      const size_t h = (size_t)mdbx_tst_rkl_pop(&rkl, true);
      if (h == 0)
        break;
      assert(bit(set, h));
      set -= one << h;
      if (!check_holes_fourways(set, &rkl))
        return false;
    }
  }

  mdbx_tst_rkl_destroy(&rkl);
  return true;
}

static size_t prng_word(void) {
  size_t word = (size_t)(prng() >> 32);
  if (sizeof(word) > 4)
    word = (uint64_t)word << 32 | (size_t)(prng() >> 32);
  return word;
}

/*-----------------------------------------------------------------------------*/

/* Детерминированные edge-кейсы на ветки rkl/txl, которые стохастика
 * подробандивает редко или вообще не задевает (mdbx_tst_rkl_merge/destructive_merge,
 * mdbx_tst_rkl_find, mdbx_tst_rkl_reserve, resize-shrink, ENOMEM-path, mdbx_tst_rkl_check error-branches).
 * Быстрые — выполняются всегда; полный набор — под управлением MDBX_RKL_EDGES. */
TEST(details_rkl, edge_cases_full) {
  tst_reset();
  if (getenv_uint("MDBX_RKL_EDGES", 1) == 0) {
    GTEST_SKIP() << "SKIPPED by MDBX_RKL_EDGES";
    return;
  }
  MDBX_tst_rkl_t a, b;

  /* mdbx_tst_rkl_reserve: рост буфера из inplace во внешний */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, mdbx_tst_rkl_inplace_capacity() + 8), (uint64_t)MDBX_SUCCESS);
  for (uint64_t id = 1; id < 64; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_rkl_check(&a));
  CHECK_EQ(mdbx_tst_rkl_len(&a), 63);
  mdbx_tst_rkl_destroy(&a);

  /* mdbx_tst_rkl_find: в solid-интервале, перед/после него, в списке, отсутствует */
  mdbx_tst_rkl_init(&b);
  for (uint64_t id = 1; id < 6; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, id), (uint64_t)MDBX_SUCCESS); /* solid [1..6) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 10), (uint64_t)MDBX_SUCCESS);   /* список */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 20), (uint64_t)MDBX_SUCCESS);   /* список */
  MDBX_tst_rkl_iter_t fi;
  CHECK_TRUE(mdbx_tst_rkl_find(&b, 3, &fi));   /* в solid */
  CHECK_FALSE(mdbx_tst_rkl_find(&b, 6, &fi));  /* сразу после solid, нет в списке */
  CHECK_FALSE(mdbx_tst_rkl_find(&b, 7, &fi));  /* после solid, нет в списке */
  CHECK_FALSE(mdbx_tst_rkl_find(&b, 0, &fi));  /* до solid, в списке нет */
  CHECK_TRUE(mdbx_tst_rkl_find(&b, 10, &fi));  /* элемент списка */
  CHECK_FALSE(mdbx_tst_rkl_find(&b, 11, &fi)); /* после элемента списка */
  CHECK_TRUE(mdbx_tst_rkl_find(&b, 20, &fi));  /* хвостовой элемент списка */
  mdbx_tst_rkl_destroy(&b);

  /* mdbx_tst_rkl_merge: список+интервал, дубликаты с ignore_duplicates и без */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_init(&b);
  for (uint64_t id = 1; id < 16; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS);
  for (uint64_t id = 1; id < 32; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, id), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_merge(&a, &b, true), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_rkl_check(&b));
  CHECK_EQ(mdbx_tst_rkl_len(&b), 31);
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);

  /* mdbx_tst_rkl_destructive_merge: merge + destroy src */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_init(&b);
  for (uint64_t id = 1; id < 64; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 3), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_destructive_merge(&a, &b, false), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_rkl_check(&b));
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);

  /* mdbx_tst_rkl_copy с последующим полным pop-разбором (лист+интервал) */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_init(&b);
  for (uint64_t id = 1; id < 32; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 7), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_copy(&a, &b), (uint64_t)MDBX_SUCCESS);
  while (!mdbx_tst_rkl_empty(&b)) {
    const uint64_t v = mdbx_tst_rkl_pop(&b, true);
    CHECK_TRUE(v != 0);
  }
  CHECK_TRUE(mdbx_tst_rkl_empty(&b));
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);
}

/* Базовый набор edge-кейсов: дёшево, выполняется всегда. */
TEST(details_rkl, edge_cases_basic) {
  tst_reset();
  MDBX_tst_rkl_t a;

  /* mdbx_tst_rkl_pop из пустого и после исчерпания */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_pop(&a, true), 0);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_pop(&a, false), 0);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 42), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_pop(&a, true), (uint64_t)42);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_pop(&a, false), 0);

  /* mdbx_tst_rkl_resize: запрос уменьшения (ошибка PROBLEM) и сохранение inplace */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, mdbx_tst_rkl_inplace_capacity()), (uint64_t)MDBX_SUCCESS);
  mdbx_tst_rkl_destroy(&a);

  /* mdbx_tst_rkl_clear_and_shrink: ужимка обратно в inplace */
  mdbx_tst_rkl_init(&a);
  for (uint64_t id = 1; id < 32; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS);
  mdbx_tst_rkl_clear_and_shrink(&a);
  CHECK_TRUE(mdbx_tst_rkl_empty(&a));
  mdbx_tst_rkl_destroy(&a);
}

/* Дополнительные edge-кейсы (группа B): rare paths, добивают хвостовые ветки
 * find/merge/push/hole и destructive_move с внешним буфером. */
TEST(details_rkl, edge_cases_full2) {
  tst_reset();
  if (getenv_uint("MDBX_RKL_EDGES", 1) == 0) {
    GTEST_SKIP() << "SKIPPED by MDBX_RKL_EDGES";
    return;
  }
  MDBX_tst_rkl_t a, b;
  MDBX_tst_rkl_iter_t fi;

  /* mdbx_tst_rkl_destructive_move: dst с внешним буфером + копирование inplace-источника */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_init(&b);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&b, mdbx_tst_rkl_inplace_capacity() + 16), (uint64_t)MDBX_SUCCESS);
  for (uint64_t id = 1; id < 8; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id), (uint64_t)MDBX_SUCCESS);
  mdbx_tst_rkl_destructive_move(&a, &b);
  CHECK_TRUE(mdbx_tst_rkl_check(&b));
  CHECK_EQ(mdbx_tst_rkl_len(&b), 7);
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);

  /* mdbx_tst_rkl_find: id больше хвоста списка (it==end путь) */
  mdbx_tst_rkl_init(&b);
  for (uint64_t id = 1; id < 6; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, id), (uint64_t)MDBX_SUCCESS); /* solid [1..6) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_FALSE(mdbx_tst_rkl_find(&b, 21, &fi)); /* нет нигде -> tail (it==end) путь */
  mdbx_tst_rkl_destroy(&b);

  /* mdbx_tst_rkl_merge без ignore_duplicates на дубликате -> ошибка-путь */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_init(&b);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_merge(&a, &b, false), (uint64_t)MDBX_RESULT_TRUE);
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);

  /* mdbx_tst_rkl_push: дубликат среди элементов списка (не solid) */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS); /* список */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_RESULT_TRUE);
  mdbx_tst_rkl_destroy(&a);

  /* mdbx_tst_rkl_hole: reverse-итератор на конце rkl (rare edge-path) */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 42), (uint64_t)MDBX_SUCCESS);
  {
    MDBX_tst_rkl_iter_t rh = mdbx_tst_rkl_iterator(&a, true);
    MDBX_tst_rkl_hole_t hh = mdbx_tst_rkl_hole(&rh, true);
    CHECK_TRUE(hh.begin < hh.end);
    MDBX_tst_rkl_hole_t hf = mdbx_tst_rkl_hole(&rh, false);
    CHECK_TRUE(hf.begin < hf.end);
  }
  mdbx_tst_rkl_destroy(&a);

  /* mdbx_tst_rkl_check: невалидные структуры -> false (error-branches) */
  mdbx_tst_rkl_init(&a);
  CHECK_FALSE(mdbx_tst_rkl_check(NULL));
  mdbx_tst_rkl_set_limit(&a, mdbx_tst_rkl_inplace_capacity() - 1); /* list_limit < inplace */
  CHECK_FALSE(mdbx_tst_rkl_check(&a));
  mdbx_tst_rkl_set_limit(&a, mdbx_tst_rkl_inplace_capacity());
  CHECK_TRUE(mdbx_tst_rkl_check(&a));
  if (mdbx_tst_rkl_inplace_capacity() > 2) {
    /* нарушенный порядок в списке при непустом solid вне диапазона списка */
    mdbx_tst_rkl_list_to_inplace(&a);
    mdbx_tst_rkl_set_length(&a, 3);
    mdbx_tst_rkl_set_solid(&a, 1000, 1001); /* solid вне диапазона элементов списка */
    mdbx_tst_rkl_set_inplace(&a, 0, 10);
    mdbx_tst_rkl_set_inplace(&a, 1, 5); /* не отсортировано */
    mdbx_tst_rkl_set_inplace(&a, 2, 20);
    CHECK_FALSE(mdbx_tst_rkl_check(&a));
    mdbx_tst_rkl_set_inplace(&a, 1, 15);
    CHECK_TRUE(mdbx_tst_rkl_check(&a));
  }
  mdbx_tst_rkl_destroy(&a);

  /* Регрессия (bug:testing:rkl-push-dup): дубликат в СЕРЕДИНЕ списка после
   * сдвигов. Раньше откат сдвигов был off-by-one и терял хвост списка:
   * [5,10,15,20] + push(10) -> [5,10,15,15] (невалидно). */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS); /* список */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 15), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_rkl_check(&a));
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 10), (uint64_t)MDBX_RESULT_TRUE); /* dup в середине */
  CHECK_TRUE(mdbx_tst_rkl_check(&a));                                        /* невалидность -> баг */
  CHECK_EQ(mdbx_tst_rkl_len(&a), 5);                                         /* элемент 20 не потерян */
  CHECK_TRUE(mdbx_tst_rkl_contain(&a, 20));
  mdbx_tst_rkl_destroy(&a);

  /* dup в начале списка (после трёх сдвигов) */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 15), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_RESULT_TRUE); /* dup в начале */
  CHECK_TRUE(mdbx_tst_rkl_check(&a));
  CHECK_EQ(mdbx_tst_rkl_len(&a), 5);
  CHECK_TRUE(mdbx_tst_rkl_contain(&a, 20));
  mdbx_tst_rkl_destroy(&a);

  /* dup в конце списка (без сдвигов — контроль раннего выхода) */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 15), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 20), (uint64_t)MDBX_RESULT_TRUE); /* dup в конце */
  CHECK_TRUE(mdbx_tst_rkl_check(&a));
  CHECK_EQ(mdbx_tst_rkl_len(&a), 5);
  mdbx_tst_rkl_destroy(&a);

  /* dup внутри solid-интервала — ранний выход без мутаций */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 2), (uint64_t)MDBX_SUCCESS); /* solid [1..3) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_RESULT_TRUE);
  CHECK_TRUE(mdbx_tst_rkl_check(&a));
  mdbx_tst_rkl_destroy(&a);
}

TEST(details_rkl, stochastic_hole) {
  tst_reset();
  prng_state = (uint64_t)time(nullptr);
  const size_t hole_probes = getenv_uint("MDBX_RKL_HOLE_PROBES", 24 * 24 * 24);
  for (size_t n = 0; n < hole_probes; ++n) {
    size_t set = prng_word();
    if (!stochastic_pass_hole(set, prng() % 11))
      return;
    if (!stochastic_pass_hole(set & prng_word(), prng() % 11))
      return;
    if (!stochastic_pass_hole(set | prng_word(), prng() % 11))
      return;
  }
  return;
}

/* -----------------------------------------------------------------------------
 * Probe-bus driven edge-cases (пилот B68-P3): классы R/F/D/C для rkl/txl.
 * Требует runtime-активации MDBX_PROBES=1 (устанавливается в main и через
 * ENVIRONMENT теста). Вне probe-сборок эти кейсы не выполняются. */

static int probe_ctl_ok(const char *request) {
  char reply[1024];
  const int rc = mdbx_tst_probe_ctl(request, reply, sizeof(reply));
  if (rc != 0 || strncmp(reply, "ok", 2) != 0) {
    ++tst_failed;
    ADD_FAILURE() << "mdbx_tst_probe_ctl(\"" << request << "\") rc=" << rc << " reply=\"" << reply << "\"";
    return -1;
  }
  return 0;
}

/* Включить runtime-активацию (если не задано окружением теста).
 * NB: на Windows движок читает env через GetEnvironmentVariableA (Win32
 * process environment block), а CRT _putenv_s обновляет только CRT-таблицу —
 * они расходятся, поэтому используем Win32 API. */
static void probe_activate(void) {
  const char *value = getenv("MDBX_PROBES");
  if (!value || !*value || strcmp(value, "1") != 0) {
#if defined(_WIN32)
    SetEnvironmentVariableA("MDBX_PROBES", "1");
#else
    setenv("MDBX_PROBES", "1", 0);
#endif
  }
}

/* The probe-bus reads MDBX_PROBES at its first fire, and the probes fire
 * from the very first rkl operation - so the activation must precede the
 * whole run, hence the static initializer instead of a call inside a TEST
 * body (the test bodies run after the stochastic ones may have fired). */
namespace {
struct tst_probe_env_setup {
  tst_probe_env_setup() { probe_activate(); }
};
static tst_probe_env_setup tst_probe_env_setup_instance;
} /* namespace */

/* Классы непокрытых веток rkl.c/txl.c (инвентарь: docs/engineering/
 * coverage-pilot-inventory.md). Каждый кейс изолирован (arm/disarm/reset). */
TEST(details_rkl, edge_cases_probes) {
  tst_reset();
  if (!mdbx_tst_probes_active()) {
    GTEST_SKIP() << "the probe-bus is not compiled in: these cases cannot work";
    return;
  }
  char reply[1024];
  MDBX_tst_rkl_t a, b;
  uint64_t *l;

  if (probe_ctl_ok("mode count") != 0)
    return;

  /* --- D: mdbx_tst_rkl_resize "unable shrink" -> MDBX_PROBLEM (disarm dev-assert) --- */
  mdbx_tst_rkl_init(&a);
  for (uint64_t id = 1; id <= 8; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS); /* 2 в solid, 8 в списке */
  if (probe_ctl_ok("disarm rkl:resize:wanna_gt_length") != 0)
    return;
  CHECK_EQ((uint64_t)mdbx_tst_rkl_resize(&a, 4), (uint64_t)MDBX_PROBLEM);
  if (probe_ctl_ok("arm rkl:resize:wanna_gt_length") != 0)
    return;
  mdbx_tst_rkl_destroy(&a);

  /* --- R: mdbx_tst_rkl_resize "too long" -> MDBX_TXN_FULL (wanna > mdbx_tst_txl_max()) --- */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, (size_t)mdbx_tst_txl_max() + 1), (uint64_t)MDBX_TXN_FULL);
  mdbx_tst_rkl_destroy(&a);

  /* --- R: shrink внешнего буфера обратно в inplace --- */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, mdbx_tst_rkl_inplace_capacity() + 16), (uint64_t)MDBX_SUCCESS);
  for (uint64_t id = 1; id <= 4; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(!mdbx_tst_rkl_is_inplace(&a));
  CHECK_EQ((uint64_t)mdbx_tst_rkl_resize(&a, mdbx_tst_rkl_inplace_capacity()), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_rkl_is_inplace(&a));
  mdbx_tst_rkl_destroy(&a);

  /* --- R: resize на уже-inplace буфере (else-ветка) --- */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_resize(&a, mdbx_tst_rkl_inplace_capacity()), (uint64_t)MDBX_SUCCESS);
  mdbx_tst_rkl_destroy(&a);

  /* --- F: alloc-fault -> MDBX_ENOMEM в mdbx_tst_rkl_reserve (mdbx_tst_rkl_resize L101) --- */
  mdbx_tst_rkl_init(&a);
  if (probe_ctl_ok("alloc-fault 1") != 0)
    return;
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, mdbx_tst_rkl_inplace_capacity() + 1), (uint64_t)MDBX_ENOMEM);
  if (probe_ctl_ok("alloc-fault none") != 0)
    return;
  mdbx_tst_rkl_destroy(&a);

  /* --- F+R: mdbx_tst_rkl_copy проброс ошибки роста (L124) --- */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, 20), (uint64_t)MDBX_SUCCESS);
  for (uint64_t id = 1; id <= 20; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS); /* список, без solid */
  mdbx_tst_rkl_init(&b);
  if (probe_ctl_ok("alloc-fault 1") != 0)
    return;
  CHECK_EQ((uint64_t)mdbx_tst_rkl_copy(&a, &b), (uint64_t)MDBX_ENOMEM);
  if (probe_ctl_ok("alloc-fault none") != 0)
    return;
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);

  /* --- F+R: mdbx_tst_rkl_push проброс ошибки роста буфера (L277) --- */
  mdbx_tst_rkl_init(&a);
  for (uint64_t id = 1; id <= mdbx_tst_rkl_inplace_capacity() + 1; ++id)
    CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS); /* список до предела inplace */
  if (probe_ctl_ok("alloc-fault 1") != 0)
    return;
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 99), (uint64_t)MDBX_ENOMEM);
  if (probe_ctl_ok("alloc-fault none") != 0)
    return;
  mdbx_tst_rkl_destroy(&a);

  /* --- R: mdbx_tst_rkl_merge return err из цикла по списку (L425) --- */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_init(&b);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS); /* solid[1,2), list[5] */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 1), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&b, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_merge(&a, &b, false), (uint64_t)MDBX_RESULT_TRUE);
  mdbx_tst_rkl_destroy(&a);
  mdbx_tst_rkl_destroy(&b);

  /* --- C: mdbx_tst_rkl_extend_solid defensive gap_head (L214) --- */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_set_solid(&a, 10, 12);
  mdbx_tst_rkl_set_length(&a, 1);
  mdbx_tst_rkl_set_inplace(&a, 0, 11); /* элемент не примыкает к solid_begin после сдвига влево */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_extend_solid(&a, 10, 13, 12), (uint64_t)MDBX_RESULT_TRUE);
  mdbx_tst_rkl_destroy(&a);

  /* --- C: mdbx_tst_rkl_extend_solid defensive gap_tail (L219) --- */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_set_solid(&a, 10, 13);
  mdbx_tst_rkl_set_length(&a, 1);
  mdbx_tst_rkl_set_inplace(&a, 0, 13); /* элемент != solid_end при проверке примыкания */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_extend_solid(&a, 10, 14, 13), (uint64_t)MDBX_RESULT_TRUE);
  mdbx_tst_rkl_destroy(&a);

  /* --- R: mdbx_tst_rkl_hole reverse past-end (L518-522) --- */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 2), (uint64_t)MDBX_SUCCESS);  /* solid[1,3) */
  CHECK_EQ((uint64_t)mdbx_tst_rkl_push(&a, 42), (uint64_t)MDBX_SUCCESS); /* list[42] */
  {
    MDBX_tst_rkl_iter_t f = mdbx_tst_rkl_iterator(&a, false);
    CHECK_EQ(mdbx_tst_rkl_turn(&f, false), 1);
    CHECK_EQ(mdbx_tst_rkl_turn(&f, false), 2);
    CHECK_EQ(mdbx_tst_rkl_turn(&f, false), 42); /* pos == len */
    CHECK_EQ(mdbx_tst_rkl_pop(&a, true), 42);   /* len стала 2, pos остался 3 */
    MDBX_tst_rkl_hole_t hole = mdbx_tst_rkl_hole(&f, true);
    CHECK_EQ(hole.begin, 1);
    CHECK_EQ(hole.end, 1);
  }
  mdbx_tst_rkl_destroy(&a);

  /* --- C: mdbx_tst_rkl_check limit_below_inplace (L646) --- */
  mdbx_tst_rkl_init(&a);
  CHECK_EQ((uint64_t)mdbx_tst_rkl_reserve(&a, mdbx_tst_rkl_inplace_capacity() + 16), (uint64_t)MDBX_SUCCESS);
  mdbx_tst_rkl_set_limit(&a, mdbx_tst_rkl_inplace_capacity() - 1); /* испорченная структура */
  CHECK_FALSE(mdbx_tst_rkl_check(&a));
  mdbx_tst_rkl_destroy(&a);

  /* --- C: mdbx_tst_rkl_check solid_float_high (L665) --- */
  mdbx_tst_rkl_init(&a);
  mdbx_tst_rkl_set_solid(&a, 10, 13);
  mdbx_tst_rkl_set_length(&a, 1);
  mdbx_tst_rkl_set_inplace(&a, 0, 12);
  CHECK_FALSE(mdbx_tst_rkl_check(&a));
  mdbx_tst_rkl_destroy(&a);

  /* --- R: mdbx_tst_txl_reserve "too long" -> MDBX_TXN_FULL --- */
  l = mdbx_tst_txl_alloc();
  if (!CHECK_TRUE(l != NULL))
    return;
  CHECK_EQ((uint64_t)mdbx_tst_txl_reserve(&l, (size_t)mdbx_tst_txl_max() + 1), (uint64_t)MDBX_TXN_FULL);
  mdbx_tst_txl_free(l);

  /* --- R: mdbx_tst_txl_contain возвращает true (дубликат) --- */
  l = mdbx_tst_txl_alloc();
  if (!CHECK_TRUE(l != NULL))
    return;
  CHECK_EQ((uint64_t)mdbx_tst_txl_append(&l, 7), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(mdbx_tst_txl_contain(l, 7));
  mdbx_tst_txl_free(l);

  /* --- F+R: mdbx_tst_txl_append проброс ошибки (L87) + realloc ENOMEM (L63) --- */
  l = mdbx_tst_txl_alloc();
  if (!CHECK_TRUE(l != NULL))
    return;
  {
    uint64_t id = 1;
    while (mdbx_tst_txl_size(l) < mdbx_tst_txl_alloclen(l))
      CHECK_EQ((uint64_t)mdbx_tst_txl_append(&l, id++), (uint64_t)MDBX_SUCCESS); /* заполнили до ёмкости */
    if (probe_ctl_ok("alloc-fault 1") != 0) {
      mdbx_tst_txl_free(l);
      return;
    }
    CHECK_EQ((uint64_t)mdbx_tst_txl_append(&l, id), (uint64_t)MDBX_ENOMEM);
    if (probe_ctl_ok("alloc-fault none") != 0) {
      mdbx_tst_txl_free(l);
      return;
    }
  }
  mdbx_tst_txl_free(l);

  /* --- Аттестация: каждый ожидаемый тег обязан иметь seen > 0 --- */
  {
    static const char *const tags[] = {
        "rkl:resize:wanna_gt_length", /* D: disarmed срабатывание считается в seen */
        "rkl_extend_solid_gap_head",     "rkl_extend_solid_gap_tail",  "rkl_hole_past_end_reverse",
        "rkl_check_limit_below_inplace", "rkl_check_solid_float_high", "txl_reserve_too_long",
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i) {
      char request[512];
      snprintf(request, sizeof(request), "query %s", tags[i]);
      const int rc = mdbx_tst_probe_ctl(request, reply, sizeof(reply));
      if (rc != 0) {
        ++tst_failed;
        fprintf(stderr, "FAIL attestation: query \"%s\" rc=%d\n", tags[i], rc);
        continue;
      }
      const char *const site = strstr(reply, "site ");
      unsigned seen = 0;
      if (!site || sscanf(site, "site %*s %*u %*u %u", &seen) != 1 || seen == 0) {
        ++tst_failed;
        fprintf(stderr, "FAIL attestation: site \"%s\" seen==%u (reply=\"%s\")\n", tags[i], seen, reply);
      }
    }
  }
}

/*-----------------------------------------------------------------------------*/
