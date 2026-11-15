#include "../env-guard.h++"
#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstring>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/444
// The z_inner same-size replace branch of cursor_put() rewrote the node key
// with memcpy(node_key(node), key->iov_base, key->iov_len) unconditionally.
// For a DUPSORT sub-db the "key" of an inner record is the duplicate VALUE,
// and a zero-length value is legitimate: then key->iov_base is nullptr and
// memcpy(dst, nullptr, 0) is UB (C11 7.24.1p2 requires valid pointers even
// for n == 0) — UBSan traps on it. The plain-table same-size replace
// (memcpy(old_data.iov_base, data->iov_base, 0) with a null base) has the
// same shape.
//
// The fix: memcpy_checklen() guards at both sites (skipping the call
// for a zero length is behavior-preserving), controlled by the
// MDBX_CHECK_MEMZEROLEN build option so that ASAN/UBSAN/MEMCHECK runs can
// enable them without taxing the default builds.

namespace {

const char kPlainDbPath[] = "issue0444-plain.mdbx";
const char kDupsortDbPath[] = "issue0444-dupsort.mdbx";

} // namespace

TEST(issue_gh0444, plain_same_size_empty_replace) {
  const env_files_guard guard({kPlainDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPlainDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_ACCEDE), 0644));
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));

  MDBX_val key = {const_cast<char *>("k"), 1};
  MDBX_val empty = {nullptr, 0};
  /* The first put creates the node with a zero-length value. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &empty, MDBX_UPSERT));
  /* The second put hits the same-size replace: memcpy(page, nullptr, 0). */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &empty, MDBX_UPSERT));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  env_owner.release();
}

TEST(issue_gh0444, z_inner_same_size_empty_replace) {
  const env_files_guard guard({"issue0444-dupsort.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDupsortDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_ACCEDE), 0644));
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DUPSORT | MDBX_CREATE, &dbi));

  /* The dup value is the "key" of the inner record: empty and null-base. */
  MDBX_val key = {const_cast<char *>("k"), 1};
  MDBX_val empty = {nullptr, 0};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &empty, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &empty, MDBX_UPSERT));

  /* The z_inner same-size replace: cursor_put() on the nested tree rewrites
   * the inner record's key (the duplicate value) in place. Reposition on the
   * empty duplicate and re-put it with a null base via MDBX_CURRENT. */
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &key, &empty, MDBX_SET));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_put(cursor, &key, &empty, MDBX_CURRENT));
  mdbx_cursor_close(cursor);

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  env_owner.release();
}
