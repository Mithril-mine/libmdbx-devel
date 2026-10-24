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

#if defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L
#include <filesystem>
#endif /* __cpp_lib_filesystem >= 201703L */
#include <thread>

#if defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L

namespace {
const char *kSource = "test-copy-src";
const char *kPlainCopy = "test-copy-plain";
const char *kCompactCopy = "test-copy-compact";

uintmax_t file_size_of(const char *name) {
  std::error_code ec;
  return std::filesystem::file_size(name, ec);
}
} // namespace

TEST(ut_copy_ops, env_copy_plain_and_compact) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));
  mdbx::env::remove(mdbx::path(kCompactCopy));

  {
    mdbx::env_managed env(kSource, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    {
      auto txn = env.start_write();
      auto table = txn.create_map("table");
      for (int i = 0; i < 100; ++i)
        txn.upsert(table, mdbx::slice(std::string("key") + std::to_string(i)), mdbx::slice("small-value"));
      /* one large value to exercise the overflow/bigfoot path */
      txn.upsert(table, mdbx::slice("big"), mdbx::slice(std::string(64 * 1024, 'B')));
      txn.commit();
    }
    env.copy(kPlainCopy, /*compactify=*/false);
    env.copy(kCompactCopy, /*compactify=*/true);
  }

  EXPECT_GE(file_size_of(kPlainCopy), 65536u);
  EXPECT_GE(file_size_of(kCompactCopy), 65536u);

  const auto verify = [](const char *path, const char *expect_big) {
    mdbx::env_managed env(path, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = env.start_read();
    auto table = txn.open_map("table");
    EXPECT_EQ(txn.get(table, "big"), mdbx::slice(expect_big));
    for (int i = 0; i < 100; ++i)
      EXPECT_EQ(txn.get(table, mdbx::slice(std::string("key") + std::to_string(i))), mdbx::slice("small-value"))
          << "i=" << i;
  };
  verify(kPlainCopy, std::string(64 * 1024, 'B').c_str());
  verify(kCompactCopy, std::string(64 * 1024, 'B').c_str());

  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));
  mdbx::env::remove(mdbx::path(kCompactCopy));
}

TEST(ut_copy_ops, txn_copy_snapshot) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));

  mdbx::env_managed env(kSource, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table");
    for (int i = 0; i < 50; ++i)
      txn.upsert(table, mdbx::slice(std::string("key") + std::to_string(i)), mdbx::slice("v"));
    txn.commit();
  }
  {
    /* snapshot copy taken from a read txn while a concurrent writer proceeds */
    auto snapshot = env.start_read();
    auto table = snapshot.open_map("table");
    std::thread writer([&] {
      auto w = env.start_write();
      w.upsert(w.create_map("later"), "k", "x");
      w.commit();
    });
    EXPECT_EQ(snapshot.get(table, "key0"), mdbx::slice("v"));
    snapshot.copy(kPlainCopy, /*compactify=*/false);
    writer.join();
  }
  /* On Windows a memory-mapped environment cannot be deleted while it is
   * still open (decl_env.h++ remove_mode notes), so close before removing. */
  env.close();

  /* the copy contains the snapshot state, but not the later table */
  {
    mdbx::env_managed copy(kPlainCopy, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = copy.start_read();
    EXPECT_EQ(txn.get(txn.open_map("table"), "key0"), mdbx::slice("v"));
    EXPECT_THROW((void)txn.open_map("later"), mdbx::not_found) << "later table must not leak into the snapshot";
  }

  mdbx::env::remove(mdbx::path(kSource));
  mdbx::env::remove(mdbx::path(kPlainCopy));
}

#else /* !(filesystem available) */

TEST(ut_copy_ops, env_copy_plain_and_compact) {
  GTEST_SKIP() << "requires <filesystem> (C++17), disabled since the requested standard is older";
}

TEST(ut_copy_ops, txn_copy_snapshot) {
  GTEST_SKIP() << "requires <filesystem> (C++17), disabled since the requested standard is older";
}

#endif /* __cpp_lib_filesystem >= 201703L */