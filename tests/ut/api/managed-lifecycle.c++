/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <thread>

namespace {
const char *kTestPath = "test-managed-lifecycle";
} // namespace

TEST(ut_managed_lifecycle, txn_move_transfers_ownership) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
TEST(ut_managed_lifecycle, config_setters_throw_on_error) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::env::remove(mdbx::path(kTestPath));
  mdbx::env_managed env(kTestPath, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));

  /* A plain value-building set_*() is noexcept and cannot fail, but the
   * applying env::set_*() methods must report errors instead of silently
   * swallowing them (issue #143). */
  mdbx::env::geometry geo;
  static_assert(noexcept(geo.set_size_upper(1 << 20)), "geometry::set_*() is a noexcept value builder");

  /* Shrinking the upper bound below the current file size is rejected by the
   * C API; derive the bound from the actual page size (a platform-specific
   * value, 16 KiB on recent macOS) instead of hardcoding it. */
  mdbx::env::geometry invalid_geo;
  invalid_geo.set_size_upper((intptr_t)env.get_pagesize());
  EXPECT_THROW(env.set_geometry(invalid_geo), mdbx::db_full)
      << "a contradictory geometry must not be silently accepted";

  env.close();
  mdbx::env::remove(mdbx::path(kTestPath));
}

TEST(ut_managed_lifecycle, close_from_foreign_thread_best_effort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::env::remove(mdbx::path(kTestPath));
  /* The environment is created and owned here (the "owner" thread), then the
   * ownership is moved into a foreign thread where it is closed and destroyed:
   * the documented contract (issue #145) is a best-effort release, so this
   * must neither crash nor double-close. */
  auto *env = new mdbx::env_managed(kTestPath, mdbx::create_parameters(),
                                    mdbx::operate_parameters().set_max_maps(8));
  std::thread([&env]() {
    env->close(); /* explicit close on the foreign thread must work */
    delete env;   /* destructor with handle_ == nullptr is a no-op */
  }).join();
  mdbx::env::remove(mdbx::path(kTestPath));
}
