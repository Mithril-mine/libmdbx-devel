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

#include "../../../src/internals.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/67
// A nested-transaction begin moves the parent's loose pages into the parent's
// reclaim list one at a time. Before the fix the parent's loose_count was
// zeroed only after the whole loop had finished, so an MDBX_ENOMEM from
// pnl_insert_span() partway through left the still-open parent with loose_count
// overcounting its actual loose-page list (and a stale loose_refund_wl).
// The fix decrements loose_count as each page is unlinked, keeping the counter
// consistent with the list at every exit point.
// The failure is injected deterministically via the probe-bus FAULT site
// `txn_nested_loose_move` placed after the unlink+wash of a page, so the
// injected error happens with one page already moved (K=1). Requires a build
// with MDBX_PROBES=ON and MDBX_PROBES=1 in the environment.
// NB: probe sites are registered lazily on first fire, so the site must be
// "warmed up" by one successful nested begin before it can be armed.

namespace {

// Exact equivalent of the engine's page_next() link (page-ops.h): the loose
// chain is threaded through the freed space at the start of the entries.
static page_t *loose_link(page_t *lp) {
  return *(page_t **)((char *)lp + PAGEHDRSZ + (sizeof(void *) - sizeof(uint32_t)));
}

/* Accumulate loose pages: store values large enough to spawn single-page
 * overflow, then overwrite them with tiny values — the released overflow
 * pages go to the parent's loose list (page_retire, page-ops.c). */
static void accumulate_loose(MDBX_txn *txn, MDBX_dbi dbi, size_t base, size_t count) {
  MDBX_cursor *cur = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cur));
  std::vector<char> big(3400, 'x');
  char tiny[8] = {'t', 'i', 'n', 'y', '!', '!', '!', 0};
  char kbuf[32];
  for (size_t i = 0; i < count; i++) {
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", base + i);
    MDBX_val key = {kbuf, size_t(kn)};
    MDBX_val data = {big.data(), big.size()};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_put(cur, &key, &data, MDBX_UPSERT));
  }
  for (size_t i = 0; i < count; i++) {
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", base + i);
    MDBX_val key = {kbuf, size_t(kn)};
    MDBX_val data = {tiny, sizeof tiny};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_put(cur, &key, &data, MDBX_UPSERT));
  }
  mdbx_cursor_close(cur);
}

TEST(issue_gh0067, loose_count_stays_consistent_on_alloc_failure) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue67-nested-loosen.mdbx";
  remove(dbname);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR, 0644));

  MDBX_txn *parent = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &parent));
  MDBX_dbi main_dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(parent, nullptr, MDBX_db_flags_t(0), &main_dbi));

  /* Warm-up: let the probe site fire once (it is registered lazily on first
   * fire), so the FAULT rule can be armed below. The successful nested begin
   * moves the parent's loose pages into its reclaim list. */
  const size_t batch = 2000;
  accumulate_loose(parent, main_dbi, 0, batch);
  {
    MDBX_txn *warm = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, parent, MDBX_TXN_READWRITE, &warm));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(warm));
  }

  /* Accumulate loose pages in the parent again. */
  accumulate_loose(parent, main_dbi, batch, batch);

  const size_t loose = parent->wr.loose_count;
  ASSERT_GE(loose, 2u);
  /* sanity: the counter already matches the list before any failure */
  size_t prelinked = 0;
  for (page_t *lp = parent->wr.loose_pages; lp; lp = loose_link(lp))
    prelinked++;
  EXPECT_EQ(loose, prelinked);

  char reply[512];
  ASSERT_EQ(0, mdbx_probe_ctl("fault txn_nested_loose_move none", reply, sizeof reply));

  /* Arm the FAULT site and begin a nested transaction: the injected error is
   * consumed after the first loose page has been unlinked and washed (K=1). */
  char cmd[128];
  std::snprintf(cmd, sizeof cmd, "fault txn_nested_loose_move %d", (int)MDBX_ENOMEM);
  ASSERT_EQ(0, mdbx_probe_ctl(cmd, reply, sizeof reply));

  MDBX_txn *nested = nullptr;
  const int rc = mdbx_txn_begin(env, parent, MDBX_TXN_READWRITE, &nested);
  if (rc == (int)MDBX_SUCCESS) {
    /* The fault was not consumed by the loose-page move site (e.g. the parent
     * had no loose pages at that point): nothing to verify. */
    mdbx_txn_abort(nested);
    mdbx_txn_abort(parent);
    mdbx_env_close(env);
    remove(dbname);
    GTEST_SKIP() << "fault was not consumed by the loose-page move site";
  }
  ASSERT_EQ((int)MDBX_ENOMEM, rc) << "nested begin must fail with the injected code";

  /* The parent is not aborted on this failure: its loose accounting must stay
   * consistent — loose_count equal to the number of pages still linked. */
  size_t linked = 0;
  for (page_t *lp = parent->wr.loose_pages; lp; lp = loose_link(lp))
    linked++;
  EXPECT_EQ(loose - 1, linked) << "one page was already moved into repnl";
  EXPECT_EQ(linked, parent->wr.loose_count);

  /* Disarm the fault; the parent must remain open and usable. */
  ASSERT_EQ(0, mdbx_probe_ctl("fault txn_nested_loose_move none", reply, sizeof reply));
  char kbuf[32], vbuf[32];
  int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", 2 * batch);
  int vn = std::snprintf(vbuf, sizeof vbuf, "v%07zu", 2 * batch);
  MDBX_val key = {kbuf, size_t(kn)};
  MDBX_val data = {vbuf, size_t(vn)};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(parent, main_dbi, &key, &data, MDBX_UPSERT));

  /* A fresh nested transaction works afterwards. */
  MDBX_txn *child = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, parent, MDBX_TXN_READWRITE, &child));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(child, main_dbi, &key, &data, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(child));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(parent));

  mdbx_env_close(env);
  remove(dbname);
}

} // namespace