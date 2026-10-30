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
constexpr size_t paged_count = 6000;
constexpr size_t paged_key_len = 48;

std::string paged_key(size_t i) {
  std::string key(paged_key_len, 'a');
  memcpy(&key[0], &i, sizeof(i));
  return key;
}
} // namespace

/// SC-61: tree-search — ветви перехода страницы (B+tree multi-level).
TEST(ut_multi_page_nav, navigation_across_pages) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-tree-search";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("main");
    for (size_t i = 0; i < paged_count; ++i) {
      auto key = paged_key(i);
      auto val = std::to_string(i);
      txn.put(map, mdbx::slice(key), mdbx::slice(val), mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("main");
  auto cur = txn.open_cursor(map);

  for (size_t i = 0; i < paged_count; ++i) {
    auto key = paged_key(i);
    auto found = cur.find(mdbx::slice(key), false);
    EXPECT_TRUE(found.done);
    if (found.done) {
      EXPECT_EQ(found.value, mdbx::slice(std::to_string(i)));
    }
  }

  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  size_t n = 1;
  while (cur.move(mdbx::cursor::move_operation::next, false))
    ++n;
  EXPECT_EQ(n, paged_count);

  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::last, true).done);
  size_t m = 1;
  while (cur.move(mdbx::cursor::move_operation::previous, false))
    ++m;
  EXPECT_EQ(m, paged_count);
}

/// SC-62: tree-search — DUP-границы (value_mode::multi).
TEST(ut_multi_page_nav, dup_boundaries) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-tree-search-dup";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("multi", mdbx::key_mode::usual,
                              mdbx::value_mode::multi);
    for (uint32_t k = 0; k < 400; ++k) {
      std::string key(32, 'k');
      memcpy(&key[0], &k, sizeof(k));
      for (int v = 0; v < 4; ++v) {
        std::string val = "v" + std::to_string(k * 10 + v);
        txn.put(map, mdbx::slice(key), mdbx::slice(val), mdbx::upsert);
      }
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("multi", mdbx::key_mode::usual,
                          mdbx::value_mode::multi);
  auto cur = txn.open_cursor(map);

  for (uint32_t k = 0; k < 400; ++k) {
    std::string key(32, 'k');
    memcpy(&key[0], &k, sizeof(k));
    auto found = cur.find_multivalue(mdbx::slice(key),
                                     mdbx::slice("v" + std::to_string(k * 10 + 2)), false);
    EXPECT_TRUE(found.done);
    if (found.done) {
      EXPECT_EQ(found.value,
                mdbx::slice("v" + std::to_string(k * 10 + 2)));
    }

    auto absent = mdbx::slice("v99999");
    EXPECT_FALSE(cur.find_multivalue(mdbx::slice(key), absent, false).done);
    EXPECT_THROW((void)cur.find_multivalue(mdbx::slice(key), absent, true),
                 mdbx::not_found);

    auto lb = cur.lower_bound_multivalue(mdbx::slice(key),
                                         mdbx::slice("v" + std::to_string(k * 10 + 1)), false);
    EXPECT_TRUE(lb.done);
    if (lb.done) {
      EXPECT_EQ(lb.value, mdbx::slice("v" + std::to_string(k * 10 + 1)));
    }
  }
}
