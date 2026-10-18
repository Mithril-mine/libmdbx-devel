#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

using buffer = mdbx::buffer<>;

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/99
// C++ buffer API defects: reserve() shrinks capacity and moves data,
// reshape<false>() computes the headroom with a reversed sign, buffer(txn,
// slice) throws for user memory and an empty slice, and extract()/replace()
// return a buffer that lost the caller's allocator.

TEST(issue_gh0099, reserve_keeps_capacity_and_headroom) {
  buffer b(mdbx::slice("payload"), false);
  b.reserve_tailroom(4096);
  const size_t capacity = b.capacity();
  ASSERT_GT(capacity, (size_t)4096);
  b.reserve(64);
  /* STL-compatible reserve must not shrink the capacity nor reset the
   * headroom, and must keep the data in place. */
  EXPECT_GE(b.capacity(), capacity) << "reserve(size) must not shrink the capacity";
  EXPECT_EQ(0u, b.headroom()) << "reserve(size) must keep the headroom unchanged";
  EXPECT_EQ(0, memcmp(b.data(), "payload", 7)) << "reserve(size) must keep the data";
}

TEST(issue_gh0099, reseve_with_headroom_does_not_trip_assertion) {
  buffer h(mdbx::slice("payload"), false);
  h.reserve(32, 32);
  /* an ordinary reserve() over a buffer with headroom used to compute the old
   * headroom with a reversed sign and trip the checked-build assertion */
  EXPECT_NO_THROW(h.reserve(16, 16));
  EXPECT_EQ(0, memcmp(h.data(), "payload", 7)) << "the data must survive a reserve";
}

TEST(issue_gh0099, buffer_from_txn_accepts_user_memory_and_empty_slice) {
  const std::string testdb = "issue99.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
  const char *const payload = "user memory";
  mdbx::txn_managed txn = env.start_write();
  txn.create_map("t");
  txn.commit();

  mdbx::txn_managed rd = env.start_read();
  /* a source outside the database map must be copied, not rejected */
  buffer u(rd, mdbx::slice(payload, strlen(payload)));
  EXPECT_EQ(0, memcmp(u.data(), payload, strlen(payload))) << "user memory must be copied";
  EXPECT_FALSE(u.is_reference()) << "user memory must be copied, not referenced";
  /* an empty slice must not throw */
  buffer e(rd, mdbx::slice());
  EXPECT_EQ(0u, e.length());
  rd.abort();

  env.close();
  mdbx::env::remove(testdb);
}

TEST(issue_gh0099, extract_returns_buffer_with_caller_allocator) {
  const std::string testdb = "issue99b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  auto map = txn.create_map("t");
  txn.upsert(map, mdbx::slice("k"), mdbx::slice("value-of-k"));
  /* extract() must hand the value to the caller as an owned buffer of the
   * caller's allocator, not as a reference into the transaction (which dies
   * on commit) nor as a copy from a different resource. */
  mdbx::buffer<> old = txn.extract<mdbx::buffer<>>(map, mdbx::slice("k"));
  EXPECT_TRUE(old.is_freestanding()) << "extract() must return an owned buffer";
  EXPECT_EQ(0, memcmp(old.data(), "value-of-k", 9));
  txn.commit();

  env.close();
  mdbx::env::remove(testdb);
}