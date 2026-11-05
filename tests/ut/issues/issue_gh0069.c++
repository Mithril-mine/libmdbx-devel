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

#include "../env-guard.h++"
#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/69
// mdbx_drop(txn, FREE_DBI, ...) was accepted and unconditionally ran
// tbl_purge() on the GC handle before the `dbi < CORE_DBS` check, destroying
// the garbage-collection tree — permanently leaking every page the GC tracked
// and corrupting page accounting — while returning MDBX_SUCCESS. The GC table
// must be refused up front.

#define FREE_DBI 0

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static bool populate_gc(MDBX_env *env) {
  /* Insert in one write transaction, delete in a separate one, so that the
   * discarded pages are tracked by the GC tree (as in the issue's reproducer). */
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS)
    return false;
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  for (int i = 0; i < 20000; ++i) {
    char kb[24], vb[24];
    const int kn = snprintf(kb, sizeof kb, "key%08d", i);
    const int vn = snprintf(vb, sizeof vb, "val%08d", i);
    MDBX_val key = {kb, (size_t)kn};
    MDBX_val val = {vb, (size_t)vn};
    if (mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      return false;
    }
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }

  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS)
    return false;
  MDBX_cursor *mc = nullptr;
  if (mdbx_cursor_open(txn, dbi, &mc) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  MDBX_val key = {}, data = {};
  int rc;
  while ((rc = mdbx_cursor_get(mc, &key, &data, MDBX_NEXT)) == MDBX_SUCCESS) {
    if ((rc = mdbx_cursor_del(mc, MDBX_put_flags_t(0))) != MDBX_SUCCESS) {
      mdbx_cursor_close(mc);
      mdbx_txn_abort(txn);
      return false;
    }
  }
  mdbx_cursor_close(mc);
  if (rc != MDBX_NOTFOUND) {
    mdbx_txn_abort(txn);
    return false;
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  return true;
}

TEST(issue_gh0069, free_dbi_drop_is_rejected_and_gc_survives) {
  const env_files_guard guard({"issue69.mdbx", "issue308.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue69.mdbx";
  remove_db(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));
  ASSERT_TRUE(populate_gc(env)) << "could not populate the GC tree";

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn));
  MDBX_stat st;
  std::memset(&st, 0, sizeof(st));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn, FREE_DBI, &st, sizeof(st)));
  const uint64_t entries_before = st.ms_entries;
  ASSERT_GT(entries_before, 0u) << "the GC tree should have tracked discarded pages";

  /* Clearing (or dropping) the GC table must be refused, not executed. */
  EXPECT_EQ(MDBX_EACCESS, mdbx_drop(txn, FREE_DBI, false)) << "mdbx_drop(FREE_DBI) must be rejected";

  std::memset(&st, 0, sizeof(st));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn, FREE_DBI, &st, sizeof(st)));
  EXPECT_EQ(entries_before, st.ms_entries) << "the GC tree must survive a rejected drop";

  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}

// Follow-up https://github.com/Mithril-mine/libmdbx-devel/issues/308
// mdbx_dbi_open(MDBX_CHK_GC, MDBX_CREATE) on an empty GC took the "re-create an
// empty table" branch of dbi_bind(): it reset the GC flags to 0 and the commit
// stored them in a steady meta, so every later write transaction failed with
// MDBX_INCOMPATIBLE and the next open with MDBX_CORRUPTED. Like the drop above,
// MDBX_CREATE must be refused for the GC.

namespace {

/* Deletes the database before the test and on every exit path of it. */
struct db_remover {
  const char *const path;
  explicit db_remover(const char *pathname) : path(pathname) { remove_db(path); }
  ~db_remover() { remove_db(path); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};

struct txn_aborter {
  void operator()(MDBX_txn *txn) const { mdbx_txn_abort(txn); }
};

} // namespace

TEST(issue_gh0069, gc_open_with_create_is_rejected) {
  const env_files_guard guard({"issue69.mdbx", "issue308.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup("issue308.mdbx");
  char kb[] = "key", vb[] = "value";
  {
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, cleanup.path, MDBX_NOSUBDIR, 0644));

    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(txn);
    MDBX_dbi dbi = MDBX_dbi(0xdeadbeef);
    EXPECT_EQ(MDBX_EACCESS, mdbx_dbi_open(txn, (const char *)MDBX_CHK_GC, MDBX_CREATE, &dbi))
        << "MDBX_CREATE must be refused for the GC";
    /* A real change, so the commit writes a steady meta (with the GC flags). */
    MDBX_dbi main_dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
    MDBX_val key = {kb, sizeof(kb)}, data = {vb, sizeof(vb)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, main_dbi, &key, &data, MDBX_UPSERT));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn_owner.release()));

    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn))
        << "the next write transaction must start (the GC flags must be intact)";
    mdbx_txn_abort(txn);
  }

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, cleanup.path, MDBX_NOSUBDIR, 0644))
      << "the database must reopen (the steady meta must not be corrupted)";
  MDBX_txn *reader = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
  const std::unique_ptr<MDBX_txn, txn_aborter> reader_owner(reader);
  MDBX_dbi main_dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(reader, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
  MDBX_val key = {kb, sizeof(kb)}, data;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_get(reader, main_dbi, &key, &data)) << "the last commit must not be lost";
  EXPECT_EQ(sizeof(vb), data.iov_len);
}
