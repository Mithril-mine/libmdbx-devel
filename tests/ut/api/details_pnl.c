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

/* The pnl intersect/merge/check characterization: every internal access
 * goes through the testing API (tests/AGENTS.md §8, issue #519 stage 4).
 * The engine functions are the live linked ones, and the list layout is
 * encoded by mdbx_tst_pnl_make() inside the testing API, so this test
 * carries no representation knowledge of its own. */

#include "mdbx.h"

/* NB: no <stdbool.h> here - the library's mdbx.h defines bool/true/false
 * for the C mode, and the MSVC/mingw stdbool.h defines them unconditionally,
 * so a later inclusion would redefinition-warn under -Werror. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok;

static bool check_bool(bool v, bool expect, const char *fn, unsigned line) {
  if (v != expect) {
    ++tst_failed;
    fflush(NULL);
    fprintf(stderr, "got %s, expected %s, at %s:%u\n", v ? "true" : "false", expect ? "true" : "false", fn, line);
    fflush(NULL);
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
    if (got != want) {                                                                                                 \
      ++tst_failed;                                                                                                    \
      fprintf(stderr, "got %zi, expected %zi, at %s:%u\n", got, want, __func__, __LINE__);                             \
    } else                                                                                                             \
      ++tst_ok;                                                                                                        \
  } while (0)

/* Page numbers are stored in descending order, as unconditionally used by
 * the library; the caller supplies the numbers already in that order and
 * mdbx_tst_pnl_make() copies them verbatim. The alloclen word is the
 * buffer's capacity claim: the merge checks below use deliberately tight
 * values, and the intersect/check ones just pass the full capacity. */

static void test_intersect(void) {
  uint32_t buf_a[17], buf_b[17];
  const uint32_t *a, *b;

  /* disjoint ranges */
  a = mdbx_tst_pnl_make(buf_a, 15, 2, (const uint32_t[]){4, 3});
  b = mdbx_tst_pnl_make(buf_b, 15, 3, (const uint32_t[]){12, 11, 6});
  CHECK_FALSE(mdbx_tst_pnl_intersect(a, b));
  CHECK_FALSE(mdbx_tst_pnl_intersect(b, a));

  /* one shared page (the issue #83 case: two records both listing page 3) */
  a = mdbx_tst_pnl_make(buf_a, 15, 2, (const uint32_t[]){4, 3});
  b = mdbx_tst_pnl_make(buf_b, 15, 3, (const uint32_t[]){12, 11, 3});
  CHECK_TRUE(mdbx_tst_pnl_intersect(a, b));
  CHECK_TRUE(mdbx_tst_pnl_intersect(b, a));

  /* exact duplicate lists */
  a = mdbx_tst_pnl_make(buf_a, 15, 2, (const uint32_t[]){4, 3});
  b = mdbx_tst_pnl_make(buf_b, 15, 2, (const uint32_t[]){4, 3});
  CHECK_TRUE(mdbx_tst_pnl_intersect(a, b));

  /* empty lists never intersect; MSVC rejects an empty compound literal
   * `(uint32_t[]){}`, and an empty pnl is just a zero size word */
  a = mdbx_tst_pnl_make(buf_a, 15, 0, NULL);
  b = mdbx_tst_pnl_make(buf_b, 15, 2, (const uint32_t[]){4, 3});
  CHECK_FALSE(mdbx_tst_pnl_intersect(a, b));
  CHECK_FALSE(mdbx_tst_pnl_intersect(b, a));
}

static void test_check_full(void) {
  uint32_t buf_a[17];
  const uint32_t *a;

  /* strictly descending (expected internal order) passes */
  a = mdbx_tst_pnl_make(buf_a, 15, 3, (const uint32_t[]){12, 11, 6});
  CHECK_TRUE(mdbx_tst_pnl_check_full(a, 1 << 20));

  /* duplicated page inside one record fails the full check */
  a = mdbx_tst_pnl_make(buf_a, 15, 4, (const uint32_t[]){12, 11, 6, 6});
  CHECK_FALSE(mdbx_tst_pnl_check_full(a, 1 << 20));

  /* out-of-order record fails the full check */
  a = mdbx_tst_pnl_make(buf_a, 15, 3, (const uint32_t[]){11, 12, 6});
  CHECK_FALSE(mdbx_tst_pnl_check_full(a, 1 << 20));

  /* the same malformed list passes the lightweight check */
  CHECK_TRUE(mdbx_tst_pnl_check_lite(a, 1 << 20));
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

  uint32_t hb[16], merged[64];
  uint32_t *b, *dst;

  /* disjoint lists: the plain concatenation keeps the full length */
  b = mdbx_tst_pnl_make(hb, 64, 2, (const uint32_t[]){20, 10});
  dst = mdbx_tst_pnl_make(merged, 64, 3, (const uint32_t[]){50, 40, 30});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 5);

  /* the shared page at the tails of both lists (the issue #83 case: page 3
   * listed by two GC records) */
  b = mdbx_tst_pnl_make(hb, 64, 2, (const uint32_t[]){6, 3});
  dst = mdbx_tst_pnl_make(merged, 64, 3, (const uint32_t[]){12, 11, 3});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 4);

  /* the shared page in the middle of both lists */
  b = mdbx_tst_pnl_make(hb, 64, 3, (const uint32_t[]){40, 30, 10});
  dst = mdbx_tst_pnl_make(merged, 64, 3, (const uint32_t[]){50, 30, 20});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 5);

  /* several duplicated pairs: the flag is not a counter, the length shrinks
   * by exactly one */
  b = mdbx_tst_pnl_make(hb, 64, 3, (const uint32_t[]){50, 40, 30});
  dst = mdbx_tst_pnl_make(merged, 64, 2, (const uint32_t[]){50, 30});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 4);

  /* interleaved lists without duplicates are not flagged */
  b = mdbx_tst_pnl_make(hb, 64, 3, (const uint32_t[]){45, 35, 5});
  dst = mdbx_tst_pnl_make(merged, 64, 3, (const uint32_t[]){50, 40, 10});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 6);

  /* an empty source is trivially clean; an empty list is just the zero
   * size word */
  b = mdbx_tst_pnl_make(hb, 64, 0, NULL);
  dst = mdbx_tst_pnl_make(merged, 64, 2, (const uint32_t[]){9, 8});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 2);

  /* merging into an empty destination takes the source as-is; an empty
   * compound literal must be avoided for the sake of MSVC (C7757) */
  dst = mdbx_tst_pnl_make(merged, 64, 0, NULL);
  b = mdbx_tst_pnl_make(hb, 64, 2, (const uint32_t[]){6, 3});
  CHECK_EQ(mdbx_tst_pnl_merge(dst, b), 2);
}

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;

  test_intersect();
  test_check_full();
  test_merge();

  if (tst_failed == 0) {
    fprintf(stdout, "Ok: %zu checks passed\n", tst_ok);
    return EXIT_SUCCESS;
  }
  fprintf(stderr, "Failed: %zu checks failed, %zu passed\n", tst_failed, tst_ok);
  return EXIT_FAILURE;
}
