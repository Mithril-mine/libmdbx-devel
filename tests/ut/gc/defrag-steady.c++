/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement. In no event shall the
/// authors or copyright holders be liable for any claim, damages or other liability, whether in an action of contract,
/// tort or otherwise, arising from, out of or in connection with the software or the use or other dealings in the
/// software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

// The durability of a defragmentation in a non-sync mode.
//
// Truncating the datafile while no steady checkpoint covers the new end is the most dangerous of the things a
// defragmentation does: recovery may pick a checkpoint whose pages are above the new end. So the truncation follows a
// steady checkpoint, and a call that found a steady checkpoint leaves one behind. In a non-sync mode a truncation in
// the middle of the call would have to force a durable checkpoint each time, so the datafile is truncated only at the
// end of the call; the cycles themselves keep the non-sync mode of the environment.

#include "../env-guard.h++"
#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h++"

#include <cstring>
#include <string>
#include <vector>

namespace {

const char *const kPath = "defrag-steady.mdbx";
const char *const kTable = "main";
const unsigned kRecords = 600;
const size_t kValueBytes = 1500;

struct notification {
  MDBX_env *env;
  std::vector<size_t> sizes;
  unsigned calls;
  unsigned last_cycle;
};

/* The logical size of the datafile: it is lowered by the truncation of the call, and also when a platform defers the
 * physical truncation of a mapped file (Windows: a reader may hold the remap-lock, and the file keeps its size until a
 * later commit cuts it). */
size_t current_size(MDBX_env *env) {
  MDBX_envinfo info;
  memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  return static_cast<size_t>(info.mi_geo.current);
}

/* The callback is called at the beginning and at the end of each cycle: the logical size of the datafile must not
 * decrease while the call goes on, since the tail is cut only at the end. */
int on_progress(void *ctx, const MDBX_defrag_result_t *progress) MDBX_CXX17_NOEXCEPT {
  notification *const n = static_cast<notification *>(ctx);
  /* the size is sampled once a cycle: a truncation is committed with a cycle, and the next one would see it */
  if (n->calls == 0 || n->last_cycle != progress->cycles) {
    n->sizes.push_back(current_size(n->env));
    n->last_cycle = progress->cycles;
  }
  n->calls += 1;
  return 0;
}

/* One record to a page, every second one deleted: the free pages are below the data, and the defragmentation moves the
 * end of the data down and cuts it off. */
void fill_and_delete(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, kTable, MDBX_CREATE, &dbi));
  const std::string value(kValueBytes, 'v');
  for (unsigned i = 0; i < kRecords; ++i) {
    MDBX_val k = {&i, sizeof(i)};
    MDBX_val v = {const_cast<char *>(value.data()), value.size()};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  }
  for (unsigned i = 0; i < kRecords; i += 2) {
    MDBX_val k = {&i, sizeof(i)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_del(txn, dbi, &k, nullptr));
  }
  owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

/* Every odd record is read back and checked; the even ones are gone. */
void verify(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, kTable, MDBX_DB_DEFAULTS, &dbi));
  const std::string value(kValueBytes, 'v');
  for (unsigned i = 0; i < kRecords; ++i) {
    MDBX_val k = {&i, sizeof(i)};
    MDBX_val v = {nullptr, 0};
    const int rc = mdbx_get(txn, dbi, &k, &v);
    if (i & 1) {
      ASSERT_EQ(MDBX_SUCCESS, rc) << "record " << i;
      ASSERT_EQ(value.size(), v.iov_len) << "record " << i;
      ASSERT_EQ(0, memcmp(value.data(), v.iov_base, value.size())) << "record " << i;
    } else
      ASSERT_EQ(MDBX_NOTFOUND, rc) << "record " << i;
  }
}

/* The environment of the tests: a small shrink threshold, so that the free tail is cut off at the end of a call. */
void create_database(const char *path) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  env_handle_guard owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, 64 << 10, -1, 64 << 20, 128 << 10, 64 << 10, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_NOSTICKYTHREADS, 0644));
  fill_and_delete(env);
}

} // namespace

/* A defragmentation that found a steady checkpoint in the meta-pages truncates the datafile only at its end and leaves
 * a steady checkpoint behind, although the environment carries no durability promise of its own. */
TEST(ut_defrag_steady, lazy_mode_call_truncates_at_the_end_and_keeps_the_durability) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  /* the database is filled by a durable environment, so a steady checkpoint is written */
  create_database(kPath);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  env_handle_guard owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, 64 << 10, -1, 64 << 20, 128 << 10, 64 << 10, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPath, MDBX_UTTERLY_NOSYNC | MDBX_NOSTICKYTHREADS, 0644));

  uint64_t steady_txnid = 0;
  int is_steady = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_tst_env_steady_checkpoint(env, &steady_txnid, &is_steady));
  ASSERT_NE(0, is_steady) << "the durable environment left a steady checkpoint to keep, txnid=" << steady_txnid;
  ASSERT_GT(steady_txnid, 0u);

  const size_t size_before = current_size(env);
  notification note = {env, {}, 0, ~0u};
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  const int rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 48, on_progress, &note, &result);
  EXPECT_EQ(MDBX_SUCCESS, rc) << "stopping_reasons=" << result.stopping_reasons;
  EXPECT_EQ(0u, result.stopping_reasons & MDBX_defrag_error);
  EXPECT_GT(result.cycles, 1u) << "the call takes more than one cycle";

  ASSERT_GT(note.sizes.size(), 2u);
  /* The last notification may follow the final commits of the call, so it is not judged: everything before it is taken
   * while the call goes on, and must show the datafile grown or unchanged. */
  const size_t last = note.sizes.size() - 1;
  for (size_t i = 1; i < last; ++i)
    EXPECT_GE(note.sizes[i], note.sizes[i - 1]) << "the datafile shrank in the middle of the call, after " << i
                                                << " of " << note.sizes.size() << " notifications";
  for (size_t i = 0; i < last; ++i)
    EXPECT_GE(note.sizes[i], size_before) << "the datafile shrank in the middle of the call";

  const size_t size_after = current_size(env);
  EXPECT_LT(size_after, size_before) << "the datafile became smaller at the end of the call";
  EXPECT_GT(result.pages_shrinked, intptr_t(0)) << "pages_shrinked";

  /* the call keeps the durability it found */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_tst_env_steady_checkpoint(env, &steady_txnid, &is_steady));
  EXPECT_NE(0, is_steady) << "the call left a steady checkpoint behind, as it found one";

  verify(env);

  /* nothing is left to move */
  MDBX_defrag_result_t again;
  memset(&again, 0, sizeof(again));
  const int again_rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 0, nullptr, nullptr, &again);
  EXPECT_TRUE(again_rc == MDBX_SUCCESS || again_rc == MDBX_RESULT_TRUE) << "again_rc=" << again_rc;
  EXPECT_EQ(0u, again.stopping_reasons & MDBX_defrag_error);
  EXPECT_EQ(0u, again.pages_moved);
  verify(env);
}

/* A defragmentation called within a transaction of the caller takes the durability mode of that transaction: the
 * datafile is truncated only at the end of the call, although the environment itself is durable, and the flags of the
 * transaction are left as they were. */
TEST(ut_defrag_steady, caller_transaction_with_relaxed_durability_defers_the_shrink) {
  const char *const path = "defrag-steady-caller.mdbx";
  const env_files_guard guard({path});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  create_database(path);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  env_handle_guard owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, 64 << 10, -1, 64 << 20, 128 << 10, 64 << 10, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, path, MDBX_NOSTICKYTHREADS, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE | MDBX_TXN_NOSYNC, &txn));
  txn_handle_guard txn_owner(&txn);
  const MDBX_txn_flags_t txn_flags = mdbx_txn_flags(txn);

  const size_t size_before = current_size(env);
  notification note = {env, {}, 0, ~0u};
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  const int rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 48, on_progress, &note, &result);
  EXPECT_EQ(MDBX_SUCCESS, rc) << "stopping_reasons=" << result.stopping_reasons;
  EXPECT_EQ(0u, result.stopping_reasons & MDBX_defrag_error);
  EXPECT_GT(result.cycles, 1u) << "the call takes more than one cycle";

  ASSERT_GT(note.sizes.size(), 2u);
  const size_t last = note.sizes.size() - 1;
  for (size_t i = 1; i < last; ++i)
    EXPECT_GE(note.sizes[i], note.sizes[i - 1]) << "the datafile shrank in the middle of the call";
  for (size_t i = 0; i < last; ++i)
    EXPECT_GE(note.sizes[i], size_before) << "the datafile shrank in the middle of the call";
  EXPECT_LT(current_size(env), size_before) << "the datafile became smaller at the end of the call";

  /* the transaction is still the caller's, with the flags it was started with */
  EXPECT_EQ(txn_flags, mdbx_txn_flags(txn)) << "the flags of the caller's transaction are left as they were";

  uint64_t steady_txnid = 0;
  int is_steady = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_tst_env_steady_checkpoint(env, &steady_txnid, &is_steady));
  EXPECT_NE(0, is_steady) << "the call left a steady checkpoint behind, as it found one";

  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  verify(env);
}
