#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/90
// cursor::erase("B") and cursor::erase(key, "v1") bound to the `bool`
// overloads, because a string literal converts to `bool` by a standard
// conversion and to `slice` only by a user-defined one: erase("B") deleted
// all values at the cursor position, and erase(key, "v1") deleted every value
// of the key instead of one pair. The literal-array overloads must win, so
// the natural forms erase exactly what they name.

static std::string dump_all(mdbx::txn &txn, mdbx::map_handle map) {
  std::string out;
  mdbx::cursor_managed c = txn.open_cursor(map);
  mdbx::cursor::move_result r = c.to_first(false);
  while (r) {
    if (!out.empty())
      out += ' ';
    out += r.key.as_string<char>() + ":" + r.value.as_string<char>();
    r = c.to_next(false);
  }
  return out;
}

TEST(issue_gh0090, erase_literal_binds_to_key_not_bool) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue90.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  mdbx::map_handle map = txn.create_map("d", mdbx::key_mode::usual, mdbx::value_mode::multi);

  static const char *const keys[] = {"A", "B", "C"};
  static const char *const vals[] = {"v1", "v2"};
  for (auto k : keys)
    for (auto v : vals)
      txn.upsert(map, mdbx::slice(k), mdbx::slice(v));
  ASSERT_EQ("A:v1 A:v2 B:v1 B:v2 C:v1 C:v2", dump_all(txn, map));

  /* Case 1: erase("B") must delete the whole key "B", not all dups of the
   * current position. */
  mdbx::cursor_managed c = txn.open_cursor(map);
  c.to_first(false); /* on A:v1 */
  EXPECT_TRUE(c.erase("B"));
  EXPECT_EQ("A:v1 A:v2 C:v1 C:v2", dump_all(txn, map)) << "erase(\"B\") must remove key B";

  /* Case 2: erase(slice("C"), "v1") must delete only the C:v1 pair. */
  c.bind(txn, map);
  c.to_first(false);
  EXPECT_TRUE(c.erase(mdbx::slice("C"), "v1"));
  EXPECT_EQ("A:v1 A:v2 C:v2", dump_all(txn, map)) << "erase(slice(C), \"v1\") must remove only C:v1";

  /* Case 3: erase("A", "v2") must delete only the A:v2 pair. */
  c.bind(txn, map);
  c.to_first(false);
  EXPECT_TRUE(c.erase("A", "v2"));
  EXPECT_EQ("A:v1 C:v2", dump_all(txn, map)) << "erase(\"A\", \"v2\") must remove only A:v2";

  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}

static void fill_map(mdbx::txn &txn, mdbx::map_handle map) {
  txn.clear_map(map);
  static const char *const keys[] = {"A", "B", "C"};
  static const char *const vals[] = {"v1", "v2"};
  for (auto k : keys)
    for (auto v : vals)
      txn.upsert(map, mdbx::slice(k), mdbx::slice(v));
}

TEST(issue_gh0090, erase_c_string_key_and_slice_value) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue90b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  mdbx::map_handle map = txn.create_map("d", mdbx::key_mode::usual, mdbx::value_mode::multi);
  mdbx::cursor_managed c = txn.open_cursor(map);

  /* A const char* key must bind to the key overload, not to the bool
   * whole_multivalue parameter: erase("B") as a c-string removes key B. */
  fill_map(txn, map);
  ASSERT_EQ("A:v1 A:v2 B:v1 B:v2 C:v1 C:v2", dump_all(txn, map));
  const char *const key_cstr = "B";
  c.to_first(false); /* on A:v1 */
  EXPECT_TRUE(c.erase(key_cstr));
  EXPECT_EQ("A:v1 A:v2 C:v1 C:v2", dump_all(txn, map)) << "erase(const char*) key must remove key B";

  /* C-string key with an explicit bool removes the current pair only. */
  fill_map(txn, map);
  c.to_first(false); /* on A:v1 */
  EXPECT_TRUE(c.erase(key_cstr, false));
  EXPECT_EQ("A:v1 A:v2 B:v2 C:v1 C:v2", dump_all(txn, map))
      << "erase(const char*, false) must remove only the first value of B";

  /* A literal key paired with a slice value must not be ambiguous: only the
   * C:v1 pair is removed. */
  fill_map(txn, map);
  c.to_first(false);
  EXPECT_TRUE(c.erase("C", mdbx::slice("v1")));
  EXPECT_EQ("A:v1 A:v2 B:v1 B:v2 C:v2", dump_all(txn, map)) << "erase(\"C\", slice(\"v1\")) must remove only C:v1";

  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}