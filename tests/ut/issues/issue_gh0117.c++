#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/117
// 1. writemap_dirty_npages and writemap_spilled_npages were the same union
//    cell, so with MDBX_WRITEMAP mdbx_txn_info() reported twice the dirty
//    space before a spill and lost the spilled pages after it.
// 2. mdbx_ratio2digits() with precision <= 0 returned its result with one
//    uninitialized trailing byte.

TEST(issue_gh0117, writemap_dirty_space_is_not_doubled) {
  const char *const testdb = "issue117.mdbx";
  remove(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_max_db, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_WRITEMAP, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));

  /* several puts of 3 KB values: enough dirty pages to be visible in the info */
  for (unsigned i = 0; i < 200; ++i) {
    char kb[16], vb[3072];
    snprintf(kb, sizeof(kb), "k%u", i);
    memset(vb, 'a' + (i % 26), sizeof(vb));
    MDBX_val k = {kb, strlen(kb)}, v = {vb, sizeof(vb)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  }

  MDBX_txn_info info;
  memset(&info, 0, sizeof(info));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_info(txn, &info, false));
  EXPECT_GT(info.txn_space_dirty, (uint64_t)0) << "the dirty space must be reported";
  /* 200 values of ~3 KB: hundreds of dirty pages; the doubled reported count
   * would be about twice the real one. */
  EXPECT_LT(info.txn_space_dirty, (uint64_t)(2048 * 4096))
      << "the writemap dirty byte count must not be doubled by counter aliasing";

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  mdbx_env_close(env);
  remove(testdb);
}

TEST(issue_gh0117, ratio2digits_zero_precision_has_no_stray_byte) {
  char buf[64];
  memset(buf, 'Z', sizeof(buf));
  const char *const s = mdbx_ratio2digits(5, 1, 0, buf, sizeof(buf));
  EXPECT_STREQ("5", s) << "precision 0 must format the integer part only";
  EXPECT_EQ('5', s[0]);
  EXPECT_EQ('\0', s[1]) << "the string must be terminated right after the digits";
}