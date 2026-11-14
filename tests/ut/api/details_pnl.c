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

#define debug_log debug_log_sub

#include "../../../src/pnl.c"

/* NOTE: the master version of this file also covers pnl_intersect(), which
 * exists on master only (it was added by the original #83 fix and has no
 * call sites in the library); the stable line does not carry it. */

MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  (void)level;
  (void)function;
  (void)line;
  (void)fmt;
}

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok;

static bool check_bool(bool v, bool expect, const char *fn, unsigned line) {
  if (unlikely(v != expect)) {
    ++tst_failed;
    fflush(nullptr);
    fprintf(stderr, "got %s, expected %s, at %s:%u\n", v ? "true" : "false", expect ? "true" : "false", fn, line);
    fflush(nullptr);
    return false;
  }
  ++tst_ok;
  return true;
}

#define CHECK_TRUE(v) check_bool(v, true, __func__, __LINE__)
#define CHECK_FALSE(v) check_bool(v, false, __func__, __LINE__)
#define CHECK_EQ(v, exp)                                                                                               \
  do {                                                                                                                 \
    const size_t got = (size_t)(v), want = (size_t)(exp);                                                              \
    if (unlikely(got != want)) {                                                                                       \
      ++tst_failed;                                                                                                    \
      fprintf(stderr, "got %zi, expected %zi, at %s:%u\n", got, want, __func__, __LINE__);                             \
    } else                                                                                                             \
      ++tst_ok;                                                                                                        \
  } while (0)

/* Build a bare page-number list (not an allocated pnl: we only need the data
 * part, size word included). Page numbers are stored in descending order, as
 * unconditionally used by the library since MDBX_PNL_ASCENDING = 0; the
 * caller supplies the numbers already in that order. */
static pgno_t make_pnl(pgno_t *arr, const pgno_t *src, size_t n) {
  arr[0] = (pgno_t)n;
  for (size_t i = 1; i <= n; ++i)
    arr[i] = src[i - 1];
  return (pgno_t)n;
}

static void test_check_full(void) {
  pgno_t a[16];

  /* strictly descending (expected internal order) passes */
  make_pnl(a, (pgno_t[]){12, 11, 6}, 3);
  CHECK_TRUE(pnl_check_full(a, 1 << 20));

  /* duplicated page inside one record fails the full check */
  make_pnl(a, (pgno_t[]){12, 11, 6, 6}, 4);
  CHECK_FALSE(pnl_check_full(a, 1 << 20));

  /* out-of-order record fails the full check */
  make_pnl(a, (pgno_t[]){11, 12, 6}, 3);
  CHECK_FALSE(pnl_check_full(a, 1 << 20));

  /* the same malformed list passes the lightweight check */
  CHECK_TRUE(pnl_check_lite(a, 1 << 20));
}

/* Build a headed pnl (the alloclen word precedes the size word), as the
 * merging functions assert the allocated length of the destination. The
 * buffer is oversized, so the merge always has room. */
static pgno_t *make_pnl_headed(pgno_t *buf, const pgno_t *src, size_t n, size_t alloclen) {
  buf[0] = (pgno_t)alloclen;
  buf[1] = (pgno_t)n;
  for (size_t i = 1; i <= n; ++i)
    buf[1 + i] = src[i - 1];
  return buf + 1;
}

static void test_merge(void) {
  /* pnl_merge() reports the duplicate-page detection by shrinking the
   * resulting length: pnl_merge_inner() sets a flag (not a counter, which is
   * cheaper) when an equal pair of items is seen, i.e. the same page is
   * listed by both lists, and the length shrinks by exactly one regardless
   * of the number of such pairs. This is what makes the "length after
   * merge" comparison in gc_alloc_ex() a real always-on cross-record check
   * (issues 83/452: a page listed by two GC records). Note the buffer
   * content is unspecified once duplicates are detected, since the only
   * caller (gc_alloc_ex) rejects the list immediately and never consumes it. */

  pgno_t hb[16], merged[64];
  pgno_t *b, *dst;

  /* disjoint lists: the plain concatenation keeps the full length */
  b = make_pnl_headed(hb, (pgno_t[]){20, 10}, 2, 64);
  dst = make_pnl_headed(merged, (pgno_t[]){50, 40, 30}, 3, 64);
  CHECK_EQ(pnl_merge(dst, b), 5);

  /* the shared page at the tails of both lists (the issue #83 case: page 3
   * listed by two GC records) */
  b = make_pnl_headed(hb, (pgno_t[]){6, 3}, 2, 64);
  dst = make_pnl_headed(merged, (pgno_t[]){12, 11, 3}, 3, 64);
  CHECK_EQ(pnl_merge(dst, b), 4);

  /* the shared page in the middle of both lists */
  b = make_pnl_headed(hb, (pgno_t[]){40, 30, 10}, 3, 64);
  dst = make_pnl_headed(merged, (pgno_t[]){50, 30, 20}, 3, 64);
  CHECK_EQ(pnl_merge(dst, b), 5);

  /* several duplicated pairs: the flag is not a counter, the length shrinks
   * by exactly one */
  b = make_pnl_headed(hb, (pgno_t[]){50, 40, 30}, 3, 64);
  dst = make_pnl_headed(merged, (pgno_t[]){50, 30}, 2, 64);
  CHECK_EQ(pnl_merge(dst, b), 4);

  /* interleaved lists without duplicates are not flagged */
  b = make_pnl_headed(hb, (pgno_t[]){45, 35, 5}, 3, 64);
  dst = make_pnl_headed(merged, (pgno_t[]){50, 40, 10}, 3, 64);
  CHECK_EQ(pnl_merge(dst, b), 6);

  /* an empty source is trivially clean */
  hb[0] = 64;
  hb[1] = 0; /* an empty list is just the zero size word */
  dst = make_pnl_headed(merged, (pgno_t[]){9, 8}, 2, 64);
  CHECK_EQ(pnl_merge(dst, hb + 1), 2);

  /* merging into an empty destination takes the source as-is; an empty
   * list is just the zero size word, and an empty compound literal must be
   * avoided for the sake of MSVC (C7757) */
  dst = make_pnl_headed(merged, (pgno_t[]){9, 8}, 0, 64);
  b = make_pnl_headed(hb, (pgno_t[]){6, 3}, 2, 64);
  CHECK_EQ(pnl_merge(dst, b), 2);
}

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;

  test_check_full();
  test_merge();

  if (tst_failed == 0) {
    fprintf(stdout, "Ok: %zu checks passed\n", tst_ok);
    return EXIT_SUCCESS;
  }
  fprintf(stderr, "Failed: %zu checks failed, %zu passed\n", tst_failed, tst_ok);
  return EXIT_FAILURE;
}
