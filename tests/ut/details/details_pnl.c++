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
 * carries no representation knowledge of its own. A pure-algorithm unit:
 * it opens no environments and creates no files, so there is nothing for
 * the §4 hygiene guards to track. */

/* A pure-algorithm unit: no environments, no files - mdbx.h (the testing
 * API declarations) and gtest are all it needs. */
#include "mdbx.h"
#include <gtest/gtest.h>

#include <stdint.h>

/*-----------------------------------------------------------------------------*/

/* Page numbers are stored in descending order, as unconditionally used by
 * the library; the caller supplies the numbers already in that order and
 * mdbx_tst_pnl_make() copies them verbatim. The alloclen word is the
 * buffer's capacity claim: the merge checks below use deliberately tight
 * values, and the intersect/check ones just pass the full capacity. */

namespace {

TEST(details_pnl, intersect) {
  /* C++ has no compound literals (ISO C++ forbids them), so the items are
   * named arrays; the two-page `ab` list serves most of the cases. */
  static const uint32_t ab[] = {4, 3};
  static const uint32_t disjoint[] = {12, 11, 6};
  static const uint32_t shares_tail[] = {12, 11, 3};
  uint32_t buf_a[17], buf_b[17];
  const uint32_t *a, *b;

  /* disjoint ranges */
  a = mdbx_tst_pnl_make(buf_a, 15, 2, ab);
  b = mdbx_tst_pnl_make(buf_b, 15, 3, disjoint);
  EXPECT_FALSE(mdbx_tst_pnl_intersect(a, b));
  EXPECT_FALSE(mdbx_tst_pnl_intersect(b, a));

  /* one shared page (the issue #83 case: two records both listing page 3) */
  a = mdbx_tst_pnl_make(buf_a, 15, 2, ab);
  b = mdbx_tst_pnl_make(buf_b, 15, 3, shares_tail);
  EXPECT_TRUE(mdbx_tst_pnl_intersect(a, b));
  EXPECT_TRUE(mdbx_tst_pnl_intersect(b, a));

  /* exact duplicate lists */
  a = mdbx_tst_pnl_make(buf_a, 15, 2, ab);
  b = mdbx_tst_pnl_make(buf_b, 15, 2, ab);
  EXPECT_TRUE(mdbx_tst_pnl_intersect(a, b));

  /* empty lists never intersect; an empty pnl is just a zero size word */
  a = mdbx_tst_pnl_make(buf_a, 15, 0, nullptr);
  b = mdbx_tst_pnl_make(buf_b, 15, 2, ab);
  EXPECT_FALSE(mdbx_tst_pnl_intersect(a, b));
  EXPECT_FALSE(mdbx_tst_pnl_intersect(b, a));
}

TEST(details_pnl, check_full) {
  static const uint32_t descending[] = {12, 11, 6};
  static const uint32_t duplicated[] = {12, 11, 6, 6};
  static const uint32_t out_of_order[] = {11, 12, 6};
  uint32_t buf_a[17];
  const uint32_t *a;

  /* strictly descending (expected internal order) passes */
  a = mdbx_tst_pnl_make(buf_a, 15, 3, descending);
  EXPECT_TRUE(mdbx_tst_pnl_check_full(a, 1 << 20));

  /* duplicated page inside one record fails the full check */
  a = mdbx_tst_pnl_make(buf_a, 15, 4, duplicated);
  EXPECT_FALSE(mdbx_tst_pnl_check_full(a, 1 << 20));

  /* out-of-order record fails the full check */
  a = mdbx_tst_pnl_make(buf_a, 15, 3, out_of_order);
  EXPECT_FALSE(mdbx_tst_pnl_check_full(a, 1 << 20));

  /* the same malformed list passes the lightweight check */
  EXPECT_TRUE(mdbx_tst_pnl_check_lite(a, 1 << 20));
}

TEST(details_pnl, merge) {
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

  { /* disjoint lists: the plain concatenation keeps the full length */
    static const uint32_t src[] = {20, 10}, dst_items[] = {50, 40, 30};
    b = mdbx_tst_pnl_make(hb, 64, 2, src);
    dst = mdbx_tst_pnl_make(merged, 64, 3, dst_items);
    EXPECT_EQ(5u, mdbx_tst_pnl_merge(dst, b));
  }
  { /* the shared page at the tails of both lists (the issue #83 case: page
     * 3 listed by two GC records) */
    static const uint32_t src[] = {6, 3}, dst_items[] = {12, 11, 3};
    b = mdbx_tst_pnl_make(hb, 64, 2, src);
    dst = mdbx_tst_pnl_make(merged, 64, 3, dst_items);
    EXPECT_EQ(4u, mdbx_tst_pnl_merge(dst, b));
  }
  { /* the shared page in the middle of both lists */
    static const uint32_t src[] = {40, 30, 10}, dst_items[] = {50, 30, 20};
    b = mdbx_tst_pnl_make(hb, 64, 3, src);
    dst = mdbx_tst_pnl_make(merged, 64, 3, dst_items);
    EXPECT_EQ(5u, mdbx_tst_pnl_merge(dst, b));
  }
  { /* several duplicated pairs: the flag is not a counter, the length
     * shrinks by exactly one */
    static const uint32_t src[] = {50, 40, 30}, dst_items[] = {50, 30};
    b = mdbx_tst_pnl_make(hb, 64, 3, src);
    dst = mdbx_tst_pnl_make(merged, 64, 2, dst_items);
    EXPECT_EQ(4u, mdbx_tst_pnl_merge(dst, b));
  }
  { /* interleaved lists without duplicates are not flagged */
    static const uint32_t src[] = {45, 35, 5}, dst_items[] = {50, 40, 10};
    b = mdbx_tst_pnl_make(hb, 64, 3, src);
    dst = mdbx_tst_pnl_make(merged, 64, 3, dst_items);
    EXPECT_EQ(6u, mdbx_tst_pnl_merge(dst, b));
  }
  { /* an empty source is trivially clean; an empty list is just the zero
     * size word */
    static const uint32_t dst_items[] = {9, 8};
    b = mdbx_tst_pnl_make(hb, 64, 0, nullptr);
    dst = mdbx_tst_pnl_make(merged, 64, 2, dst_items);
    EXPECT_EQ(2u, mdbx_tst_pnl_merge(dst, b));
  }
  { /* merging into an empty destination takes the source as-is */
    static const uint32_t src[] = {6, 3};
    dst = mdbx_tst_pnl_make(merged, 64, 0, nullptr);
    b = mdbx_tst_pnl_make(hb, 64, 2, src);
    EXPECT_EQ(2u, mdbx_tst_pnl_merge(dst, b));
  }
}

} /* namespace */
