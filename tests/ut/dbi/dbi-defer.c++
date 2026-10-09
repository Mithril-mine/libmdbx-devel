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

TEST(ut_dbi_defer, drop_and_reuse_handle_in_same_txn) {
  auto env = make_env("test-dbi-defer");
  auto txn = env.start_write();
  auto dropped = txn.create_map("dropped");
  auto neighbor = txn.create_map("neighbor");

  txn.upsert(dropped, "k", "v");
  txn.upsert(neighbor, "k", "keep");
  txn.drop_map("dropped");

  EXPECT_THROW((void)txn.get(dropped, "k"), mdbx::bad_map_id)
      << "using the dropped handle must fail, not silently access data";
  EXPECT_EQ(txn.get(neighbor, "k"), mdbx::slice("keep")) << "neighboring handles stay valid";

  txn.commit();
  env.close();
  mdbx::env::remove("test-dbi-defer");
}

TEST(ut_dbi_defer, drop_commit_then_reopen) {
  auto env = make_env("test-dbi-defer");
  {
    auto txn = env.start_write();
    auto table = txn.create_map("victim");
    txn.upsert(table, "k", "v");
    txn.commit();
  }
  {
    auto txn = env.start_write();
    txn.drop_map("victim");
    txn.commit();
  }
  {
    auto txn = env.start_read();
    EXPECT_THROW((void)txn.open_map("victim"), mdbx::not_found)
        << "opening by the old name after commit returns NOTFOUND";
  }
  env.close();
  mdbx::env::remove("test-dbi-defer");
}

TEST(ut_dbi_defer, drop_in_read_only_txn) {
  auto env = make_env("test-dbi-defer");
  {
    auto txn = env.start_write();
    auto table = txn.create_map("victim");
    txn.upsert(table, "k", "v");
    txn.commit();
  }
  {
    auto txn = env.start_read();
    EXPECT_THROW((void)txn.drop_map("victim"), mdbx::permission_denied_or_not_writeable)
        << "schema changes are impossible in a read-only transaction";
  }
  env.close();
  mdbx::env::remove("test-dbi-defer");
}