#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/98
// lookup/scan wrappers reported wrong results or threw on normal outcomes:
// get_equal_or_great() never reported an exact key on a multimap; fullscan()/
// get_batch() threw not_found on an empty table and after the last chunk
// instead of returning false/empty; get_batch(1) always threw MDBX_EINVAL.

TEST(issue_gh0098, get_equal_or_great_exact_on_multimap) {
  const std::string testdb = "issue98.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  auto plain = txn.create_map("plain");
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  txn.upsert(plain, mdbx::slice("b"), mdbx::slice("v"));
  txn.upsert(plain, mdbx::slice("d"), mdbx::slice("v"));
  for (auto v : {"v1", "v2"}) {
    txn.upsert(multi, mdbx::slice("b"), mdbx::slice(v));
    txn.upsert(multi, mdbx::slice("d"), mdbx::slice(v));
  }

  /* The key must compare equal against the returned key, so an exact match on
   * a multimap is reported as such even though the C call needs both key and
   * value to match. */
auto r = txn.get_equal_or_great(plain, mdbx::slice("b"));
  EXPECT_TRUE(r.done) << "plain exact key must be found and reported exact";
  EXPECT_EQ(0, memcmp(r.key.data(), "b", 1)) << "plain exact key must be found";

  auto m = txn.get_equal_or_great(multi, mdbx::slice("b"));
  EXPECT_TRUE(m.done) << "multi exact key must be found and reported exact";
  EXPECT_EQ(0, memcmp(m.key.data(), "b", 1)) << "multi exact key must be found";

  auto absent = txn.get_equal_or_great(multi, mdbx::slice("c"));
  EXPECT_FALSE(absent.done) << "a non-exact key must be reported as such";
  EXPECT_EQ(0, memcmp(absent.key.data(), "d", 1)) << "the next key must be returned";
  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}

TEST(issue_gh0098, scan_and_batch_on_empty_and_end) {
  const std::string testdb = "issue98b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  auto empty = txn.create_map("empty");
  auto plain = txn.create_map("plain");
  txn.commit();

  {
    mdbx::txn_managed wt = env.start_write();
    wt.upsert(plain, mdbx::slice("a"), mdbx::slice("1"));
    wt.upsert(plain, mdbx::slice("b"), mdbx::slice("2"));
    wt.commit();
  }

  mdbx::txn_managed rtxn = env.start_read();
  mdbx::cursor_managed c = rtxn.open_cursor(empty);
  bool called = false;
  EXPECT_FALSE(c.fullscan([&](const mdbx::pair &) { called = true; return true; }))
      << "fullscan() on an empty table must return false";
  EXPECT_FALSE(called);

  /* get_batch() on an empty table and after the last chunk must return an
   * empty vector with is_last, not throw. */
  bool last = false;
  EXPECT_TRUE(c.get_batch(10, mdbx::cursor::first, &last).empty()) << "empty batch on an empty table";
  EXPECT_TRUE(last);

  mdbx::cursor_managed p = rtxn.open_cursor(plain);
  last = false;
  auto batch1 = p.get_batch(2, mdbx::cursor::first, &last);
  ASSERT_EQ(2u, batch1.size()) << "the first batch must return both pairs";
  EXPECT_TRUE(last) << "two pairs in one batch fill it";
  auto batch2 = p.get_batch(2, mdbx::cursor::next, &last);
  EXPECT_TRUE(batch2.empty()) << "the batch after the end must be empty, not throw";
  EXPECT_TRUE(last) << "the batch after the end must report is_last";

  /* get_batch(1) must work (limit is pairs, not slots). */
  mdbx::cursor_managed q = rtxn.open_cursor(plain);
  last = false;
  auto single = q.get_batch(1, mdbx::cursor::first, &last);
  ASSERT_EQ(1u, single.size()) << "a single-pair batch must work";
  EXPECT_FALSE(last);

  rtxn.abort();
  env.close();
  mdbx::env::remove(testdb);
}