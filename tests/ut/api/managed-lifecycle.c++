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
const char *kTestPath = "test-managed-lifecycle";
} // namespace

TEST(ut_managed_lifecycle, txn_move_transfers_ownership) {
  mdbx::env::remove(mdbx::path(kTestPath));
  mdbx::env_managed env(kTestPath, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));

  auto txn = env.start_write();
  auto table = txn.create_map("table");
  txn.upsert(table, "k", "v");

  auto moved = std::move(txn);
  EXPECT_EQ(moved.get(table, "k"), mdbx::slice("v")) << "moved transaction keeps working";
  moved.commit();

  env.close();
  mdbx::env::remove(mdbx::path(kTestPath));
}

TEST(ut_managed_lifecycle, cursor_withdraw_handle) {
  mdbx::env::remove(mdbx::path(kTestPath));
  mdbx::env_managed env(kTestPath, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  auto txn = env.start_write();
  auto table = txn.create_map("table");
  txn.upsert(table, "k", "v");

  {
    auto cursor = txn.open_cursor(table);
    cursor.to_first();
    MDBX_cursor *raw = cursor.withdraw_handle();
    EXPECT_NE(raw, nullptr);
    mdbx_cursor_close(raw); /* withdrawn handle closes via the C API */
  }

  txn.commit();
  env.close();
  mdbx::env::remove(mdbx::path(kTestPath));
}

TEST(ut_managed_lifecycle, context_roundtrip) {
  mdbx::env::remove(mdbx::path(kTestPath));
  mdbx::env_managed env(kTestPath, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));

  int marker = 42;
  env.set_context(&marker);
  EXPECT_EQ(env.get_context(), &marker);

  auto txn = env.start_write();
  txn.set_context(&marker);
  EXPECT_EQ(txn.get_context(), &marker);

  auto table = txn.create_map("table");
  auto cursor = txn.open_cursor(table);
  cursor.set_context(&marker);
  EXPECT_EQ(cursor.get_context(), &marker);

  cursor.close();
  txn.abort();
  env.close();
  mdbx::env::remove(mdbx::path(kTestPath));
}

TEST(ut_managed_lifecycle, destructor_order) {
  mdbx::env::remove(mdbx::path(kTestPath));
  {
    mdbx::env_managed env(kTestPath, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    {
      auto txn = env.start_write();
      auto table = txn.create_map("table");
      txn.upsert(table, "k", "v");
      {
        auto cursor = txn.open_cursor(table);
        cursor.to_first();
        EXPECT_EQ(cursor.current().value, mdbx::slice("v"));
        /* cursor destroyed here, before txn */
      }
      txn.commit();
      /* txn destroyed here, before env */
    }
    /* env destroyed last */
  }
  mdbx::env::remove(mdbx::path(kTestPath));
}