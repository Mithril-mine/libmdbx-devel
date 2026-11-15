#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdint>

/* Regression for the defect V-037 of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * mdbx_put(MDBX_CURRENT) pre-checked the new value's size on a multi-value
 * key and bailed out with MDBX_BAD_VALSIZE without unregistering its stack
 * cursor couple from txn->cursors[dbi]. The dead stack frame stayed linked
 * in the cursor list, so any later cursor operation on the same table — and
 * the end-of-transaction cursor sweep — dereferenced a dangling pointer:
 * an ASAN stack-use-after-return in frame mdbx_put() 'cx'.
 *
 * Scenario: a MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP table (the
 * value-size limit is exactly 4 bytes), a key with two dups (the node
 * carries N_DUP, so the MDBX_CURRENT size-precheck branch is entered),
 * MDBX_ALLDUPS and MDBX_NOOVERWRITE both unset, and a 5-byte new value. */

TEST(issue_gh0485, put_current_dangling_cursor) {
  const env_files_guard guard({"issue485_put.mdbx"});

  MDBX_env *env = nullptr;
  env_handle_guard henv(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, "issue485_put.mdbx", MDBX_NOSUBDIR, 0644));

  /* two dups for one key, so the outer node carries N_DUP */
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_CREATE, &dbi));
  const char key_data[] = "k";
  const uint32_t val_a = 42, val_b = 43;
  const MDBX_val key = {const_cast<char *>(key_data), 1};
  MDBX_val val4a = {const_cast<uint32_t *>(&val_a), sizeof(val_a)};
  MDBX_val val4b = {const_cast<uint32_t *>(&val_b), sizeof(val_b)};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &val4a, MDBX_APPEND));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &val4b, MDBX_APPENDDUP));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  /* the poison: a value length outside [4;4] with MDBX_CURRENT */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  txn_handle_guard htxn(&txn);
  const uint32_t bad_storage[2] = {0, 0};
  MDBX_val bad_len = {const_cast<uint32_t *>(bad_storage), 5};
  EXPECT_EQ(MDBX_BAD_VALSIZE, mdbx_put(txn, dbi, &key, &bad_len, MDBX_CURRENT));

  /* the transaction must stay healthy: mdbx_del() and every other cursor
   * operation walk txn->cursors[dbi], which still held the dead mdbx_put()
   * frame before the fix (ASAN: stack-use-after-return) */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_del(txn, dbi, &key, nullptr));
  MDBX_val got;
  EXPECT_EQ(MDBX_NOTFOUND, mdbx_get(txn, dbi, &key, &got));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn = nullptr;

  /* the twin path from the same branch: MDBX_NOOVERWRITE makes mdbx_put()
   * return MDBX_EMULTIVAL through the shared exit (no early return), which
   * must leave no dangling cursor either */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &val4a, MDBX_NOOVERWRITE));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &val4b, MDBX_APPENDDUP));
  EXPECT_EQ(MDBX_EMULTIVAL, mdbx_put(txn, dbi, &key, &bad_len, MDBX_CURRENT | MDBX_NOOVERWRITE));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_del(txn, dbi, &key, nullptr));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn = nullptr;
}
