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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "../env-guard.h++"
#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstring>
#include <memory>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/342
// With MDBX_opt_spill_parent4child_denominator set, txn_nested_create() spills
// dirty pages of the parent to make room for the child, but it sampled the
// parent's MDBX_TXN_SPILLS flag before that spill. When it was the parent's
// first spill, the child lacked the flag, so page_unspill() stopped its search
// at the child: a grandchild touching a page that the parent spilled failed
// with MDBX_PROBLEM ("not found in the spill-list(s)"), and Debug builds
// asserted in page_unspill().
//
// The database page size and the key bytes are fixed, so the number of dirty
// pages, and with it whether starting the child spills, does not depend on the
// platform or its byte order. The test checks that starting the child did
// spill, without relying on the optional page-operation statistics.

namespace {

/* Deletes the database before the test and on every exit path of it. */
struct testdb_cleanup {
  const char *const path;
  explicit testdb_cleanup(const char *pathname) : path(pathname) { erase(); }
  ~testdb_cleanup() { erase(); }
  void erase() const { mdbx_env_delete(path, MDBX_ENV_JUST_DELETE); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};

struct txn_aborter {
  void operator()(MDBX_txn *txn) const { mdbx_txn_abort(txn); }
};

/* The record number as explicit little-endian bytes, the same on any host. */
struct record_key {
  uint8_t bytes[4];
  explicit record_key(int i) : bytes{uint8_t(i), uint8_t(i >> 8), uint8_t(i >> 16), uint8_t(i >> 24)} {}
  MDBX_val val() { return MDBX_val{bytes, sizeof(bytes)}; }
};

/* Room left for dirty pages: a spill of the parent for its child grows it. */
uint64_t dirty_room(MDBX_txn *txn) {
  MDBX_txn_info info;
  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_info(txn, &info, false));
  return info.txn_space_leftover;
}

/* The spill counter, or 0 when the build has no page-operation statistics. */
uint64_t spill_count(MDBX_env *env, MDBX_txn *txn) {
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, txn, &info, sizeof(info)));
  return info.mi_pgop_stat.spill;
}

} // namespace

TEST(issue_gh0342, grandchild_finds_pages_spilled_for_the_child) {
  const env_files_guard guard({"issue342.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const testdb_cleanup cleanup("issue342.mdbx");

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, 4096));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, cleanup.path, MDBX_NOSUBDIR, 0644));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_spill_parent4child_denominator, 2));
  /* Above what the basal transaction dirties, below what the child's spill request needs. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_txn_dp_limit, 1024));
  const uint64_t spills_at_open = spill_count(env, nullptr);

  const int records = 4000;
  char value[400];

  MDBX_txn *basal = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &basal));
  std::unique_ptr<MDBX_txn, txn_aborter> basal_owner(basal); /* aborts the nested ones too */
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(basal, nullptr, MDBX_DB_DEFAULTS, &dbi));
  std::memset(value, 'a', sizeof(value));
  for (int i = 0; i < records; ++i) {
    record_key k(i);
    MDBX_val key = k.val(), data = {value, sizeof(value)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(basal, dbi, &key, &data, MDBX_UPSERT)) << "record " << i;
  }
  if (spill_count(env, basal) != spills_at_open) {
    GTEST_SKIP() << "the basal transaction already spilled while being filled (e.g. MDBX_DEBUG_SPILLING), "
                    "so starting the child is not its first spill";
    return;
  }

  const uint64_t room_before = dirty_room(basal);
  MDBX_txn *child = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, basal, MDBX_TXN_READWRITE, &child));
  ASSERT_GT(dirty_room(child), room_before) << "precondition: starting the child must spill parent pages";

  MDBX_txn *grandchild = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, child, MDBX_TXN_READWRITE, &grandchild));
  std::memset(value, 'b', sizeof(value));
  for (int i = 0; i < records; ++i) { /* every record, so also every spilled page */
    record_key k(i);
    MDBX_val key = k.val(), data = {value, sizeof(value)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(grandchild, dbi, &key, &data, MDBX_UPSERT)) << "record " << i;
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(grandchild));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(child));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(basal_owner.release()));

  MDBX_txn *reader = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
  const std::unique_ptr<MDBX_txn, txn_aborter> reader_owner(reader);
  for (int i = 0; i < records; ++i) {
    record_key k(i);
    MDBX_val key = k.val(), data;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(reader, dbi, &key, &data)) << "record " << i;
    ASSERT_EQ(sizeof(value), data.iov_len);
    ASSERT_EQ(0, std::memcmp(value, data.iov_base, sizeof(value))) << "record " << i << " keeps the old value";
  }
}
