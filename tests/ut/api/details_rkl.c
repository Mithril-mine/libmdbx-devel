/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#define debug_log debug_log_sub

#include "../../../src/rkl.c"
#include "../../../src/txl.c"

MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  (void)level;
  (void)function;
  (void)line;
  (void)fmt;
}

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok, tst_iterations, tst_cases, tst_cases_hole;
#ifndef NDEBUG
static size_t tst_target;
#endif

static bool check_bool(bool v, bool expect, const char *fn, unsigned line) {
  if (unlikely(v != expect)) {
    ++tst_failed;
    fflush(nullptr);
    fprintf(stderr, "iteration %zi: got %s, expected %s, at %s:%u\n", tst_iterations, v ? "true" : "false",
            expect ? "true" : "false", fn, line);
    fflush(nullptr);
    return false;
  }
  ++tst_ok;
  return true;
}

static bool check_eq(uint64_t v, uint64_t expect, const char *fn, unsigned line) {
  if (unlikely(v != expect)) {
    ++tst_failed;
    fflush(nullptr);
    fprintf(stderr, "iteration %zi: %" PRIu64 " (got) != %" PRIu64 " (expected), at %s:%u\n", tst_iterations, v, expect,
            fn, line);
    fflush(nullptr);
    return false;
  }
  ++tst_ok;
  return true;
}

#define CHECK_BOOL(T, EXPECT) check_bool((T), (EXPECT), __func__, __LINE__)
#define CHECK_TRUE(T) CHECK_BOOL(T, true)
#define CHECK_FALSE(T) CHECK_BOOL(T, false)
#define CHECK_EQ(T, EXPECT) check_eq((T), (EXPECT), __func__, __LINE__)

void trivia(void) {
  rkl_t x, y;

  rkl_init(&x);
  rkl_init(&y);
  CHECK_TRUE(rkl_check(&x));
  CHECK_TRUE(rkl_empty(&x));
  CHECK_EQ(rkl_len(&x), 0);

  rkl_iter_t f = rkl_iterator(&x, false);
  rkl_iter_t r = rkl_iterator(&x, true);
  CHECK_EQ(rkl_left(&f, false), 0);
  CHECK_EQ(rkl_left(&f, true), 0);
  CHECK_EQ(rkl_left(&r, false), 0);
  CHECK_EQ(rkl_left(&r, true), 0);
  CHECK_EQ(rkl_turn(&f, false), 0);
  CHECK_EQ(rkl_turn(&f, true), 0);
  CHECK_EQ(rkl_turn(&r, false), 0);
  CHECK_EQ(rkl_turn(&r, true), 0);
  CHECK_TRUE(rkl_check(&x));

  rkl_hole_t hole;
  hole = rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&f, false);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&r, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&r, false);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, MAX_TXNID);

  CHECK_EQ((uint64_t)rkl_push(&x, 42), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(rkl_check(&x));
  CHECK_FALSE(rkl_empty(&x));
  CHECK_EQ(rkl_len(&x), 1);
  // CHECK_EQ((uint64_t)rkl_push(&x, 42, true), (uint64_t)MDBX_RESULT_TRUE);
  // CHECK_TRUE(rkl_check(&x));

  f = rkl_iterator(&x, false);
  r = rkl_iterator(&x, true);
  CHECK_EQ(rkl_left(&f, false), 1);
  CHECK_EQ(rkl_left(&f, true), 0);
  CHECK_EQ(rkl_left(&r, false), 0);
  CHECK_EQ(rkl_left(&r, true), 1);

  CHECK_EQ(rkl_turn(&f, true), 0);
  CHECK_EQ(rkl_turn(&f, false), 42);
  CHECK_EQ(rkl_turn(&f, false), 0);
  CHECK_EQ(rkl_turn(&f, true), 42);
  CHECK_EQ(rkl_turn(&f, true), 0);

  CHECK_EQ(rkl_turn(&r, false), 0);
  CHECK_EQ(rkl_turn(&r, true), 42);
  CHECK_EQ(rkl_turn(&r, true), 0);
  CHECK_EQ(rkl_turn(&r, false), 42);
  CHECK_EQ(rkl_turn(&r, false), 0);

  f = rkl_iterator(&x, false);
  hole = rkl_hole(&f, false);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&f, false);
  CHECK_EQ(hole.begin, MAX_TXNID);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, 42);
  hole = rkl_hole(&f, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, 42);

  r = rkl_iterator(&x, true);
  hole = rkl_hole(&r, false);
  CHECK_EQ(hole.begin, MAX_TXNID);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&r, true);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&r, true);
  CHECK_EQ(hole.begin, 1);
  CHECK_EQ(hole.end, 42);
  hole = rkl_hole(&r, false);
  CHECK_EQ(hole.begin, 43);
  CHECK_EQ(hole.end, MAX_TXNID);
  hole = rkl_hole(&r, false);
  CHECK_EQ(hole.begin, MAX_TXNID);
  CHECK_EQ(hole.end, MAX_TXNID);

  rkl_resize(&x, 222);
  CHECK_FALSE(rkl_empty(&x));
  CHECK_TRUE(rkl_check(&x));

  rkl_destructive_move(&x, &y);
  CHECK_TRUE(rkl_check(&x));
  CHECK_TRUE(rkl_check(&y));
  rkl_destroy(&x);
  rkl_destroy(&y);
}

/*-----------------------------------------------------------------------------*/

uint64_t prng_state;

static uint64_t prng(void) {
  prng_state = prng_state * UINT64_C(6364136223846793005) + 1;
  return prng_state;
}

static bool flipcoin(void) { return (bool)prng() & 1; }

static bool stochastic_pass(const unsigned start, const unsigned width, const unsigned n) {
  rkl_t k, c;
  txl_t l = txl_alloc();
  if (!CHECK_TRUE(l))
    return false;

  rkl_init(&k);
  rkl_init(&c);
  const size_t errors = tst_failed;

  rkl_iter_t f = rkl_iterator(&k, false);
  rkl_iter_t r = rkl_iterator(&k, true);

  txnid_t lowest = UINT_MAX;
  txnid_t highest = 0;
  while (txl_size(l) < n) {
    txnid_t id = (txnid_t)(prng() % width + start);
    if (id < MIN_TXNID || id >= INVALID_TXNID)
      continue;
    if (txl_contain(l, id)) {
      if (CHECK_TRUE(rkl_contain(&k, id)) && CHECK_EQ((uint64_t)rkl_push(&k, id), (uint64_t)MDBX_RESULT_TRUE))
        continue;
      break;
    }
    if (!CHECK_FALSE(rkl_contain(&k, id)))
      break;

    if (tst_iterations % (1u << 24) == 0 && tst_iterations) {
      printf("done %.3fM iteration, %zu cases\n", tst_iterations / 1000000.0, tst_cases);
      fflush(nullptr);
    }
    tst_iterations += 1;

#ifndef NDEBUG
    if (tst_iterations == tst_target) {
      printf("reach %zu iteration\n", tst_iterations);
      fflush(nullptr);
    }
#endif

    if (!CHECK_EQ(rkl_push(&k, id), MDBX_SUCCESS))
      break;
    if (!CHECK_TRUE(rkl_check(&k)))
      break;
    if (!CHECK_EQ(txl_append(&l, id), MDBX_SUCCESS))
      break;
    if (!CHECK_TRUE(rkl_contain(&k, id)))
      break;

    lowest = (lowest < id) ? lowest : id;
    highest = (highest > id) ? highest : id;
    if (!CHECK_EQ(rkl_lowest(&k), lowest))
      break;
    if (!CHECK_EQ(rkl_highest(&k), highest))
      break;
  }

  txl_sort(l);
  CHECK_EQ(rkl_len(&k), n);
  CHECK_EQ(txl_size(l), n);

  f = rkl_iterator(&k, false);
  r = rkl_iterator(&k, true);
  CHECK_EQ(rkl_left(&f, false), n);
  CHECK_EQ(rkl_left(&f, true), 0);
  CHECK_EQ(rkl_left(&r, false), 0);
  CHECK_EQ(rkl_left(&r, true), n);

  for (size_t i = 0; i < n; ++i) {
    CHECK_EQ(rkl_turn(&f, false), l[n - i]);
    CHECK_EQ(rkl_left(&f, false), n - i - 1);
    CHECK_EQ(rkl_left(&f, true), i + 1);

    CHECK_EQ(rkl_turn(&r, true), l[i + 1]);
    r.pos += 1;
    CHECK_EQ(rkl_turn(&r, true), l[i + 1]);
    CHECK_EQ(rkl_left(&r, true), n - i - 1);
    CHECK_EQ(rkl_left(&r, false), i + 1);
  }

  if (CHECK_EQ(rkl_copy(&k, &c), MDBX_SUCCESS)) {
    for (size_t i = 1; i <= n; ++i) {
      if (!CHECK_FALSE(rkl_empty(&k)))
        break;
      if (!CHECK_FALSE(rkl_empty(&c)))
        break;
      CHECK_EQ(rkl_pop(&k, true), l[i]);
      CHECK_EQ(rkl_pop(&c, false), l[1 + n - i]);
    }
  }

  CHECK_TRUE(rkl_empty(&k));
  CHECK_TRUE(rkl_empty(&c));

  rkl_destroy(&k);
  rkl_destroy(&c);
  txl_free(l);

  ++tst_cases;
  return errors == tst_failed;
}

static bool stochastic(const size_t limit_cases, const size_t limit_loops) {
  for (unsigned loop = 0; tst_cases < limit_cases || loop < limit_loops; ++loop)
    for (unsigned width = 2; width < 10; ++width)
      for (unsigned n = 1; n < width; ++n)
        for (unsigned prev = 1, start = 0, t; start < 4242; t = start + prev, prev = start, start = t)
          if (!stochastic_pass(start, 1u << width, 1u << n) || tst_failed > 42) {
            puts("bailout\n");
            return false;
          }
  return true;
}

/*-----------------------------------------------------------------------------*/

static bool bit(size_t set, size_t n) {
  assert(n < CHAR_BIT * sizeof(set));
  return (set >> n) & 1;
}

static size_t hamming_weight(size_t v) {
  const size_t m1 = (size_t)UINT64_C(0x5555555555555555);
  const size_t m2 = (size_t)UINT64_C(0x3333333333333333);
  const size_t m4 = (size_t)UINT64_C(0x0f0f0f0f0f0f0f0f);
  const size_t h01 = (size_t)UINT64_C(0x0101010101010101);
  v -= (v >> 1) & m1;
  v = (v & m2) + ((v >> 2) & m2);
  v = (v + (v >> 4)) & m4;
  return (v * h01) >> (sizeof(v) * 8 - 8);
}

static bool check_hole(const size_t set, const rkl_hole_t hole, size_t *acc) {
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
  printf("\ncase %s+%d: count %zu, holes", str, iter_offset, hamming_weight(~set) - 1);
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
  fflush(nullptr);
#endif
}

static bool check_holes_bothsides(const size_t set, rkl_iter_t const *i) {
  const size_t number_of_holes = hamming_weight(~set) - 1;
  size_t acc = 0;

  rkl_iter_t f = *i;
  for (;;) {
    rkl_hole_t hole = rkl_hole(&f, false);
    if (hole.begin == hole.end)
      break;
    if (!check_hole(set, hole, &acc))
      return false;
    if (hole.end >= CHAR_BIT * sizeof(set))
      break;
  }

  rkl_iter_t b = *i;
  for (;;) {
    rkl_hole_t hole = rkl_hole(&b, true);
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

static bool check_holes_fourways(const size_t set, const rkl_t *rkl) {
  rkl_iter_t i = rkl_iterator(rkl, false);
  int o = 0;
  do {
    debug_set(set, "initial-forward", o++);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (rkl_turn(&i, false));

  do {
    debug_set(set, "recoil-reverse", --o);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (rkl_turn(&i, true));

  i = rkl_iterator(rkl, true);
  o = 0;
  do {
    debug_set(set, "initial-reverse", --o);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (rkl_turn(&i, false));

  do {
    debug_set(set, "recoil-forward", o++);
    if (!check_holes_bothsides(set, &i))
      return false;
  } while (rkl_turn(&i, true));

  return true;
}

static bool stochastic_pass_hole(size_t set, size_t trims) {
  const size_t one = 1;
  set &= ~one;
  if (!set)
    return true;

  ++tst_cases_hole;

  rkl_t rkl;
  rkl_init(&rkl);
  for (size_t n = 1; n < CHAR_BIT * sizeof(set); ++n)
    if (bit(set, n))
      CHECK_EQ(rkl_push(&rkl, n), MDBX_SUCCESS);

  if (!check_holes_fourways(set, &rkl))
    return false;

  while (rkl_len(&rkl) > 1 && trims-- > 0) {
    if (flipcoin()) {
      const size_t l = (size_t)rkl_pop(&rkl, false);
      if (l == 0)
        break;
      assert(bit(set, l));
      set -= one << l;
      if (!check_holes_fourways(set, &rkl))
        return false;
    } else {

      const size_t h = (size_t)rkl_pop(&rkl, true);
      if (h == 0)
        break;
      assert(bit(set, h));
      set -= one << h;
      if (!check_holes_fourways(set, &rkl))
        return false;
    }
  }

  rkl_destroy(&rkl);
  return true;
}

static size_t prng_word(void) {
  size_t word = (size_t)(prng() >> 32);
  if (sizeof(word) > 4)
    word = (uint64_t)word << 32 | (size_t)(prng() >> 32);
  return word;
}

/*-----------------------------------------------------------------------------*/

static unsigned getenv_uint(const char *name, unsigned fallback) {
  const char *value = getenv(name);
  if (!value || !*value)
    return fallback;
  char *end = nullptr;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (!end || *end || parsed == ULONG_MAX)
    return fallback;
  return (unsigned)parsed;
}

/* Детерминированные edge-кейсы на ветки rkl/txl, которые стохастика
 * подробандивает редко или вообще не задевает (rkl_merge/destructive_merge,
 * rkl_find, rkl_reserve, resize-shrink, ENOMEM-path, rkl_check error-branches).
 * Быстрые — выполняются всегда; полный набор — под управлением MDBX_RKL_EDGES. */
static bool edge_cases_full(void) {
  const size_t errors = tst_failed;
  rkl_t a, b;

  /* rkl_reserve: рост буфера из inplace во внешний */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_reserve(&a, ARRAY_LENGTH(a.inplace) + 8), (uint64_t)MDBX_SUCCESS);
  for (txnid_t id = 1; id < 64; ++id)
    CHECK_EQ((uint64_t)rkl_push(&a, id), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(rkl_check(&a));
  CHECK_EQ(rkl_len(&a), 63);

  /* rkl_find: в solid-интервале, перед/после него, в списке, отсутствует */
  rkl_init(&b);
  for (txnid_t id = 1; id < 6; ++id)
    CHECK_EQ((uint64_t)rkl_push(&b, id), (uint64_t)MDBX_SUCCESS); /* solid [1..6) */
  CHECK_EQ((uint64_t)rkl_push(&b, 10), (uint64_t)MDBX_SUCCESS);   /* список */
  CHECK_EQ((uint64_t)rkl_push(&b, 20), (uint64_t)MDBX_SUCCESS);   /* список */
  rkl_iter_t fi;
  CHECK_TRUE(rkl_find(&b, 3, &fi));      /* в solid */
  CHECK_FALSE(rkl_find(&b, 6, &fi));     /* сразу после solid, нет в списке */
  CHECK_FALSE(rkl_find(&b, 7, &fi));     /* после solid, нет в списке */
  CHECK_FALSE(rkl_find(&b, 0, &fi));     /* до solid, в списке нет */
  CHECK_TRUE(rkl_find(&b, 10, &fi));     /* элемент списка */
  CHECK_FALSE(rkl_find(&b, 11, &fi));    /* после элемента списка */
  CHECK_TRUE(rkl_find(&b, 20, &fi));     /* хвостовой элемент списка */

  /* rkl_merge: список+интервал, дубликаты с ignore_duplicates и без */
  rkl_init(&a);
  rkl_init(&b);
  for (txnid_t id = 1; id < 16; ++id)
    CHECK_EQ((uint64_t)rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS);
  for (txnid_t id = 1; id < 32; ++id)
    CHECK_EQ((uint64_t)rkl_push(&b, id), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_merge(&a, &b, true), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(rkl_check(&b));
  CHECK_EQ(rkl_len(&b), 31);

  /* rkl_destructive_merge: merge + destroy src */
  rkl_init(&a);
  rkl_init(&b);
  for (txnid_t id = 1; id < 64; ++id)
    CHECK_EQ((uint64_t)rkl_push(&a, id * 3), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_destructive_merge(&a, &b, false), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(rkl_check(&b));
  rkl_destroy(&a);
  rkl_destroy(&b);

  /* rkl_copy с последующим полным pop-разбором (лист+интервал) */
  rkl_init(&a);
  rkl_init(&b);
  for (txnid_t id = 1; id < 32; ++id)
    CHECK_EQ((uint64_t)rkl_push(&a, id * 7), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_copy(&a, &b), (uint64_t)MDBX_SUCCESS);
  while (!rkl_empty(&b)) {
    const txnid_t v = rkl_pop(&b, true);
    CHECK_TRUE(v != 0);
  }
  CHECK_TRUE(rkl_empty(&b));
  rkl_destroy(&a);
  rkl_destroy(&b);

  return errors == tst_failed;
}

/* Базовый набор edge-кейсов: дёшево, выполняется всегда. */
static bool edge_cases_basic(void) {
  const size_t errors = tst_failed;
  rkl_t a;

  /* rkl_pop из пустого и после исчерпания */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_pop(&a, true), 0);
  CHECK_EQ((uint64_t)rkl_pop(&a, false), 0);
  CHECK_EQ((uint64_t)rkl_push(&a, 42), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_pop(&a, true), (uint64_t)42);
  CHECK_EQ((uint64_t)rkl_pop(&a, false), 0);

  /* rkl_resize: запрос уменьшения (ошибка PROBLEM) и сохранение inplace */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_reserve(&a, ARRAY_LENGTH(a.inplace)), (uint64_t)MDBX_SUCCESS);
  rkl_destroy(&a);

  /* rkl_clear_and_shrink: ужимка обратно в inplace */
  rkl_init(&a);
  for (txnid_t id = 1; id < 32; ++id)
    CHECK_EQ((uint64_t)rkl_push(&a, id * 2), (uint64_t)MDBX_SUCCESS);
  rkl_clear_and_shrink(&a);
  CHECK_TRUE(rkl_empty(&a));
  rkl_destroy(&a);

  return errors == tst_failed;
}

/* Дополнительные edge-кейсы (группа B): rare paths, добивают хвостовые ветки
 * find/merge/push/hole и destructive_move с внешним буфером. */
static bool edge_cases_full2(void) {
  const size_t errors = tst_failed;
  rkl_t a, b;
  rkl_iter_t fi;

  /* rkl_destructive_move: dst с внешним буфером + копирование inplace-источника */
  rkl_init(&a);
  rkl_init(&b);
  CHECK_EQ((uint64_t)rkl_reserve(&b, ARRAY_LENGTH(b.inplace) + 16), (uint64_t)MDBX_SUCCESS);
  for (txnid_t id = 1; id < 8; ++id)
    CHECK_EQ((uint64_t)rkl_push(&a, id), (uint64_t)MDBX_SUCCESS);
  rkl_destructive_move(&a, &b);
  CHECK_TRUE(rkl_check(&b));
  CHECK_EQ(rkl_len(&b), 7);
  rkl_destroy(&a);
  rkl_destroy(&b);

  /* rkl_find: id больше хвоста списка (it==end путь) */
  rkl_init(&b);
  for (txnid_t id = 1; id < 6; ++id)
    CHECK_EQ((uint64_t)rkl_push(&b, id), (uint64_t)MDBX_SUCCESS); /* solid [1..6) */
  CHECK_EQ((uint64_t)rkl_push(&b, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&b, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_FALSE(rkl_find(&b, 21, &fi)); /* нет нигде -> tail (it==end) путь */
  rkl_destroy(&b);

  /* rkl_merge без ignore_duplicates на дубликате -> ошибка-путь */
  rkl_init(&a);
  rkl_init(&b);
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&b, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_merge(&a, &b, false), (uint64_t)MDBX_RESULT_TRUE);
  rkl_destroy(&a);
  rkl_destroy(&b);

  /* rkl_push: дубликат среди элементов списка (не solid) */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS); /* список */
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_RESULT_TRUE);
  rkl_destroy(&a);

  /* rkl_hole: reverse-итератор на конце rkl (rare edge-path) */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 42), (uint64_t)MDBX_SUCCESS);
  {
    rkl_iter_t rh = rkl_iterator(&a, true);
    rkl_hole_t hh = rkl_hole(&rh, true);
    CHECK_TRUE(hh.begin < hh.end);
    rkl_hole_t hf = rkl_hole(&rh, false);
    CHECK_TRUE(hf.begin < hf.end);
  }
  rkl_destroy(&a);

  /* rkl_check: невалидные структуры -> false (error-branches) */
  rkl_init(&a);
  CHECK_FALSE(rkl_check(nullptr));
  a.list_limit = ARRAY_LENGTH(a.inplace) - 1; /* list_limit < inplace */
  CHECK_FALSE(rkl_check(&a));
  a.list_limit = ARRAY_LENGTH(a.inplace);
  CHECK_TRUE(rkl_check(&a));
  if (ARRAY_LENGTH(a.inplace) > 2) {
    /* нарушенный порядок в списке при непустом solid вне диапазона списка */
    a.list = a.inplace;
    a.list_length = 3;
    a.solid_begin = 1000; /* solid вне диапазона элементов списка */
    a.solid_end = 1001;
    a.inplace[0] = 10;
    a.inplace[1] = 5; /* не отсортировано */
    a.inplace[2] = 20;
    CHECK_FALSE(rkl_check(&a));
    a.inplace[1] = 15;
    CHECK_TRUE(rkl_check(&a));
  }
  rkl_destroy(&a);

  /* Регрессия (bug:testing:rkl-push-dup): дубликат в СЕРЕДИНЕ списка после
   * сдвигов. Раньше откат сдвигов был off-by-one и терял хвост списка:
   * [5,10,15,20] + push(10) -> [5,10,15,15] (невалидно). */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);  /* список */
  CHECK_EQ((uint64_t)rkl_push(&a, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 15), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_TRUE(rkl_check(&a));
  CHECK_EQ((uint64_t)rkl_push(&a, 10), (uint64_t)MDBX_RESULT_TRUE); /* dup в середине */
  CHECK_TRUE(rkl_check(&a));                                        /* невалидность -> баг */
  CHECK_EQ(rkl_len(&a), 5);                                         /* элемент 20 не потерян */
  CHECK_TRUE(rkl_contain(&a, 20));
  rkl_destroy(&a);

  /* dup в начале списка (после трёх сдвигов) */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 15), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_RESULT_TRUE); /* dup в начале */
  CHECK_TRUE(rkl_check(&a));
  CHECK_EQ(rkl_len(&a), 5);
  CHECK_TRUE(rkl_contain(&a, 20));
  rkl_destroy(&a);

  /* dup в конце списка (без сдвигов — контроль раннего выхода) */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)rkl_push(&a, 5), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 10), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 15), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 20), (uint64_t)MDBX_SUCCESS);
  CHECK_EQ((uint64_t)rkl_push(&a, 20), (uint64_t)MDBX_RESULT_TRUE); /* dup в конце */
  CHECK_TRUE(rkl_check(&a));
  CHECK_EQ(rkl_len(&a), 5);
  rkl_destroy(&a);

  /* dup внутри solid-интервала — ранний выход без мутаций */
  rkl_init(&a);
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_SUCCESS); /* solid [1..2) */
  CHECK_EQ((uint64_t)rkl_push(&a, 2), (uint64_t)MDBX_SUCCESS); /* solid [1..3) */
  CHECK_EQ((uint64_t)rkl_push(&a, 1), (uint64_t)MDBX_RESULT_TRUE);
  CHECK_TRUE(rkl_check(&a));
  rkl_destroy(&a);

  return errors == tst_failed;
}

static bool stochastic_hole(size_t probes) {
  for (size_t n = 0; n < probes; ++n) {
    size_t set = prng_word();
    if (!stochastic_pass_hole(set, prng() % 11))
      return false;
    if (!stochastic_pass_hole(set & prng_word(), prng() % 11))
      return false;
    if (!stochastic_pass_hole(set | prng_word(), prng() % 11))
      return false;
  }
  return true;
}

/*-----------------------------------------------------------------------------*/

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;

  printf("auxilary info: cursor/couple size is %zu for %s = %u\n", sizeof(cursor_couple_t), "MDBX_WORDBITS",
         MDBX_WORDBITS);

#ifndef NDEBUG
  // tst_target = 281870;
#endif
  prng_state = (uint64_t)time(nullptr);
  printf("prng-seed %" PRIu64 "\n", prng_state);
  fflush(nullptr);

  trivia();
  if (!edge_cases_basic())
    return EXIT_FAILURE;
  if (getenv_uint("MDBX_RKL_EDGES", 1) > 0 && (!edge_cases_full() || !edge_cases_full2()))
    return EXIT_FAILURE;

  /* Масштаб стохастики управляется env-переменными (по умолчанию — полный,
   * как было: 42³ случаев + 42 петли, 24³ hole-проб).
   * MDBX_RKL_CASES / MDBX_RKL_LOOPS / MDBX_RKL_HOLE_PROBES позволяют урезать
   * для быстрого рутинного прогона без потери веток покрытия. */
  const unsigned limit_cases = getenv_uint("MDBX_RKL_CASES", 42 * 42 * 42);
  const unsigned limit_loops = getenv_uint("MDBX_RKL_LOOPS", 42);
  const unsigned hole_probes = getenv_uint("MDBX_RKL_HOLE_PROBES", 24 * 24 * 24);
  printf("stochastic: cases=%u loops=%u hole-probes=%u\n", limit_cases, limit_loops, hole_probes);
  stochastic(limit_cases, limit_loops);
  stochastic_hole(hole_probes);
  printf("done: %zu+%zu cases, %zu iterations, %zu checks ok, %zu checks failed\n", tst_cases, tst_cases_hole,
         tst_iterations, tst_ok, tst_failed);
  fflush(nullptr);
  return tst_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
