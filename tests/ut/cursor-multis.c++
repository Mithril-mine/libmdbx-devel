/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

namespace {
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}
} // namespace

TEST(ut_cursor_multis, count_and_deepmask) {
  auto env = make_env("test-cursor-multis");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  for (int i = 0; i < 3; ++i)
    txn.upsert(multi, "k1", mdbx::slice(std::string("v") + char('0' + i)));
  for (int i = 0; i < 5; ++i)
    txn.upsert(multi, "k2", mdbx::slice(std::string("w") + char('0' + i)));

  {
    auto cursor = txn.open_cursor(multi);
    cursor.to_first(); /* on k1 */
    EXPECT_EQ(cursor.count_multivalue(), 3u) << "dups under k1";
    MDBX_stat stat;
    EXPECT_EQ(cursor.count_multivalue(&stat), 3u) << "count_ex agrees with the stat-carrying form";
  }

  /* deepmask is a bitmask of nested-dups depths; a plain small dupsort may
   * keep dups in a leaf node, so the call must succeed and return a value,
   * but we cannot assert specific bits without a deep tree. */
  (void)txn.get_tree_deepmask(multi);

  txn.commit();
  env.close();
  mdbx::env::remove("test-cursor-multis");
}

TEST(ut_cursor_multis, put_update_nodupdata_and_del) {
  auto env = make_env("test-cursor-multis");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  txn.upsert(multi, "k", "v1");
  txn.upsert(multi, "k", "v2");

  auto cursor = txn.open_cursor(multi);
  cursor.to_first(); /* k/v1 */

  cursor.update_current("x1");
  EXPECT_EQ(cursor.current().value, mdbx::slice("x1"));
  EXPECT_EQ(cursor.count_multivalue(), 2u) << "update keeps the multivalue count";

  {
    mdbx::slice probe("x1");
    EXPECT_EQ(cursor.put(mdbx::slice("k"), &probe, MDBX_NODUPDATA), MDBX_KEYEXIST)
        << "NODUPDATA refuses an existing value";
  }
  {
    mdbx::slice fresh("x9");
    EXPECT_EQ(cursor.put(mdbx::slice("k"), &fresh, MDBX_NODUPDATA), MDBX_SUCCESS)
        << "NODUPDATA accepts a brand-new value";
    EXPECT_EQ(cursor.count_multivalue(), 3u);
  }

  EXPECT_TRUE(cursor.erase(/*whole_multivalue=*/false)) << "delete the current duplicate only";
  EXPECT_EQ(cursor.count_multivalue(), 2u);

  EXPECT_TRUE(cursor.erase(/*whole_multivalue=*/true)) << "delete all remaining dups of the key";
  EXPECT_FALSE(cursor.to_next(false)) << "no entries left";

  txn.commit();
  env.close();
  mdbx::env::remove("test-cursor-multis");
}

TEST(ut_cursor_multis, bind_unbind_rebind) {
  auto env = make_env("test-cursor-multis");
  auto txn = env.start_write();
  auto a = txn.create_map("a");
  auto b = txn.create_map("b");
  txn.upsert(a, "k", "va");
  txn.upsert(b, "k", "vb");

  auto cursor = txn.open_cursor(a);
  cursor.to_first();
  EXPECT_EQ(cursor.current().value, mdbx::slice("va"));

  cursor.unbind();
  EXPECT_ANY_THROW((void)cursor.current()) << "unbound cursor cannot read";

  cursor.bind(txn, b);
  EXPECT_EQ(cursor.lower_bound("k").value, mdbx::slice("vb")) << "rebound cursor works on the new table";

  txn.commit();
  env.close();
  mdbx::env::remove("test-cursor-multis");
}