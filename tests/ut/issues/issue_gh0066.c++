#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

using mdbx::slice;

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/66
// MDBX_APPEND performed a full root-to-leaf tree_search per put by calling
// outer_last()/inner_last() even when the persistent cursor already sat at the
// rightmost leaf after the previous append. The append path now positions from
// the cached stack in that case. These tests check the observable contract
// (correct ordering, exact-match, non-append key rejection and tree growth) so
// the optimization cannot break behavior.

static MDBX_error_t append_key(mdbx::cursor_managed &cur, uint64_t n, const slice &val) {
  uint8_t raw[8];
  for (unsigned b = 0; b < 8; ++b)
    raw[b] = (uint8_t)(n >> (56 - 8 * b)); /* big-endian ascending */
  slice key{static_cast<const void *>(raw), (size_t)8};
  slice value = val;
  return cur.put(key, &value, MDBX_put_flags_t(MDBX_APPEND));
}

static uint64_t decode_key(const slice &key) {
  uint64_t k = 0;
  for (unsigned b = 0; b < 8; ++b)
    k = (k << 8) | static_cast<const uint8_t *>(key.data())[b];
  return k;
}

TEST(issue_gh0066, append_builds_ordered_tree) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue66.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  auto map = txn.create_map("t");
  mdbx::cursor_managed cur = txn.open_cursor(map);
  const slice val("v", 1);

  /* Grow the tree past several page splits so the append path deals with a
   * multi-level tree, then verify the full ascending order. */
  const uint64_t N = 10000;
  for (uint64_t i = 0; i < N; ++i)
    ASSERT_EQ(MDBX_SUCCESS, append_key(cur, i, val));
  txn.commit();

  /* integrity must hold for the appended tree */
  MDBX_chk_context_t ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  ASSERT_EQ(0, env.chk(ctx).result.total_problems);

  /* full ordered scan from a fresh read transaction */
  mdbx::txn_managed rtxn = env.start_read();
  mdbx::cursor_managed scan = rtxn.open_cursor(map);
  uint64_t last = 0;
  size_t count = 0;
  auto r = scan.to_first(false);
  while (r && count < N) {
    const uint64_t k = decode_key(r.key);
    if (count > 0) {
      EXPECT_GT(k, last) << "the appended order must be strictly increasing";
    }
    last = k;
    ++count;
    r = scan.to_next(false);
  }
  EXPECT_EQ(N, count);
  rtxn.abort();

  /* appending a key not greater than the last must fail, never corrupt */
  {
    mdbx::txn_managed wt = env.start_write();
    mdbx::cursor_managed cur2 = wt.open_cursor(map);
    uint8_t raw_small[8];
    for (unsigned b = 0; b < 8; ++b)
      raw_small[b] = 0;
    raw_small[7] = 1; /* 0x0000000000000001, less than the appended 0..N-1 */
    slice small_key{static_cast<const void *>(raw_small), (size_t)8};
    slice v = val;
    EXPECT_NE(MDBX_SUCCESS, cur2.put(small_key, &v, MDBX_put_flags_t(MDBX_APPEND)));
    wt.abort();
  }

  env.close();
  mdbx::env::remove(testdb);
}

TEST(issue_gh0066, append_to_empty_and_exact_match) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue66b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  auto map = txn.create_map("t");
  mdbx::cursor_managed cur = txn.open_cursor(map);
  const slice val("v", 1);

  /* the very first append into a new tree (the cursor is poor) */
  ASSERT_EQ(MDBX_SUCCESS, append_key(cur, 7, val));
  ASSERT_EQ(MDBX_SUCCESS, append_key(cur, 9, val));

  /* exact match of the last key: plain append semantics replace the value */
  uint8_t raw[8];
  for (unsigned b = 0; b < 8; ++b)
    raw[b] = (uint8_t)(UINT64_C(9) >> (56 - 8 * b));
  slice key9{static_cast<const void *>(raw), (size_t)8};
  slice val2("w", 1);
  ASSERT_EQ(MDBX_SUCCESS, cur.put(key9, &val2, MDBX_put_flags_t(MDBX_APPEND)));
  slice got = txn.get(map, key9);
  EXPECT_EQ(0, memcmp(got.data(), "w", 1)) << "append of the last key must update the value";

  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}