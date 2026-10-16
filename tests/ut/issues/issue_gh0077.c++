#include "mdbx.h++"
#include <gtest/gtest.h>

#include <string>
#include <type_traits>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/77
// env::start_write()/start_read() return an owning txn_managed by value, but
// the non-owning base txn was publicly copyable/movable, so
// `mdbx::txn t = env.start_write();` silently sliced the managed temporary
// into the base handle: ownership was dropped, the destructor never aborted
// the transaction, and a writer kept the environment's writer lock forever
// (every later writer blocking or failing with MDBX_BUSY). The exact-match
// deleted overloads below turn the slice into a compile error while the base
// txn stays a legitimate non-owning, copyable handle.

TEST(issue_gh0077, txn_slicing_from_managed_is_a_compile_error) {
  static_assert(!std::is_constructible<mdbx::txn, mdbx::txn_managed>::value,
                "rvalue txn_managed -> base txn must not compile");
  static_assert(!std::is_constructible<mdbx::txn, mdbx::txn_managed &>::value,
                "lvalue txn_managed -> base txn must not compile");
  static_assert(!std::is_constructible<mdbx::txn, const mdbx::txn_managed &>::value,
                "const lvalue txn_managed -> base txn must not compile");
  static_assert(!std::is_assignable<mdbx::txn &, mdbx::txn_managed>::value,
                "txn_managed -> base txn assignment must not compile");
  static_assert(!std::is_assignable<mdbx::txn &, mdbx::txn_managed &>::value,
                "txn_managed lvalue -> base txn assignment must not compile");
  /* the non-owning handle itself and the owning wrapper stay usable */
  static_assert(std::is_copy_constructible<mdbx::txn>::value, "base txn remains a copyable handle");
  static_assert(std::is_move_constructible<mdbx::txn>::value, "base txn remains movable");
  static_assert(std::is_constructible<mdbx::txn>::value, "base txn stays default-constructible");
  static_assert(std::is_move_constructible<mdbx::txn_managed>::value, "txn_managed stays movable");
}

TEST(issue_gh0077, managed_writer_releases_the_writer_lock) {
  const std::string testdb = "issue77.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  /* ending the managed writer (abort in the destructor) must free the writer
   * slot, so a second writer obtained with dont_wait must not get MDBX_BUSY */
  {
    mdbx::txn_managed write_txn = env.start_write();
    auto map = write_txn.create_map("t");
    write_txn.insert(map, mdbx::slice("k"), mdbx::slice("v"));
  }
  mdbx::txn_managed second = env.start_write(true /* dont_wait */);
  second.abort();

  env.close();
  mdbx::env::remove(testdb);
}