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

namespace {
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}
} // namespace

TEST(ut_env_maintenance, sync_to_disk) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  auto env = make_env("test-env-maintenance");
  {
    auto w = env.start_write();
    auto map = w.create_map("table");
    w.upsert(map, "k", "v");
    w.commit();
  }
  EXPECT_TRUE(env.sync_to_disk()) << "forced flush must succeed";
  EXPECT_TRUE(env.poll_sync_to_disk()) << "polling after forced flush must report nothing pending";

  env.close();
  mdbx::env::remove("test-env-maintenance");
}

TEST(ut_env_maintenance, enumerate_readers) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  auto env = make_env("test-env-maintenance");
  {
    auto w = env.start_write();
    w.create_map("table");
    w.commit();
  }

  auto reader = env.start_read();
  int seen = 0;
  bool saw_active_snapshot = false;
  auto visitor = [&](const mdbx::env::reader_info &info, int) -> int {
    ++seen;
    EXPECT_GE(info.slot, 0);
    if (info.transaction_id > 0)
      saw_active_snapshot = true;
    return mdbx::loop_control::continue_loop;
  };
  EXPECT_EQ(env.enumerate_readers(visitor), mdbx::loop_control::continue_loop);
  EXPECT_GE(seen, 1) << "the active read txn must hold a reader slot";
  EXPECT_TRUE(saw_active_snapshot) << "the active reader must publish its snapshot id";

  EXPECT_EQ(env.check_readers(), 0u) << "no stale readers in a healthy single-process env";

  reader.abort();
  env.close();
  mdbx::env::remove("test-env-maintenance");
}