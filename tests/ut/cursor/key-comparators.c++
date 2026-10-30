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

/// SC-69 (test-coverage-gaps.md): tree-search comparator branches.
/// cursor_to_search_branch() выбирает специализированные циклы поиска по
/// типу ключа: ordinal (INTEGERKEY, 4/8 байт), lexical/reverse/lenfast
/// (сравнители по-умолчанию), custom (пользовательский cmp), а также
/// foliage-ветви dupfix-таблиц. Дефолтные тесты гоняют только lenfast —
/// остальные ветви (1527 блоков) не исполняются.

namespace {
/// Наполнить таблицу и вернуть отсортированный вектор ключей в порядке
/// НАСТОЯЩЕЙ сортировки (зависит от компаратора таблицы).
template <typename KEYFN>
void fill_and_check(mdbx::txn_managed &txn, mdbx::map_handle map, size_t count, const KEYFN &make_key,
                    const std::vector<std::string> &sorted_keys) {
  for (const auto &k : sorted_keys)
    txn.put(map, mdbx::slice(k), mdbx::slice("v-" + k), mdbx::upsert);
}
} // namespace

/// ordinal-ветви: MDBX_INTEGERKEY (key_mode::ordinal), ключи 4 байта
/// (uint32) и 8 байт (uint64) — search_branch_uint32/uint64 + ordinal.
TEST(ut_key_comparators, ordinal_uint32) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-ord32";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("ord32", mdbx::key_mode::ordinal);
    const uint32_t count = 3000;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t key = i * 7 + 1; // неконsecutive, детерминированный порядок
      txn.put(map, mdbx::slice(&key, sizeof(key)), mdbx::slice("v"), mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("ord32", mdbx::key_mode::ordinal);
  auto cur = txn.open_cursor(map);
  // exact-поиск по всем (покрытие int-compare циклов uint32-ветви)
  for (uint32_t i = 0; i < 3000; ++i) {
    const uint32_t key = i * 7 + 1;
    auto found = cur.find(mdbx::slice(&key, sizeof(key)), false);
    EXPECT_TRUE(found.done);
  }
  // последовательная навигация (переходы страниц в ordinal-порядке)
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  size_t n = 1;
  while (cur.move(mdbx::cursor::move_operation::next, false))
    ++n;
  EXPECT_EQ(n, 3000u);
}

TEST(ut_key_comparators, ordinal_uint64) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-ord64";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("ord64", mdbx::key_mode::ordinal);
    const uint64_t count = 3000;
    for (uint64_t i = 0; i < count; ++i) {
      const uint64_t key = i * 11 + 3;
      txn.put(map, mdbx::slice(&key, sizeof(key)), mdbx::slice("v"), mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("ord64", mdbx::key_mode::ordinal);
  auto cur = txn.open_cursor(map);
  for (uint64_t i = 0; i < 3000; ++i) {
    const uint64_t key = i * 11 + 3;
    EXPECT_TRUE(cur.find(mdbx::slice(&key, sizeof(key)), false).done);
  }
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::last, true).done);
  size_t m = 1;
  while (cur.move(mdbx::cursor::move_operation::previous, false))
    ++m;
  EXPECT_EQ(m, 3000u);
}

/// reverse-ветвь: MDBX_REVERSEKEY (key_mode::reverse) — байты сравниваются
/// от конца к началу.
TEST(ut_key_comparators, reverse_keys) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-reverse";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("rev", mdbx::key_mode::reverse);
    // ключи с общим ПРЕФИКСОМ: в reverse-сравнении сортируются по СУФФИКСУ
    for (int i = 0; i < 2000; ++i) {
      auto tail = std::to_string(100000 + i);
      txn.put(map, mdbx::slice("prefix-" + tail), mdbx::slice("v"), mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("rev", mdbx::key_mode::reverse);
  auto cur = txn.open_cursor(map);
  for (int i = 0; i < 2000; ++i) {
    auto tail = std::to_string(100000 + i);
    EXPECT_TRUE(cur.find(mdbx::slice("prefix-" + tail), false).done);
  }
  // диапазонная навигация в reverse-порядке
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  size_t n = 1;
  while (cur.move(mdbx::cursor::move_operation::next, false))
    ++n;
  EXPECT_EQ(n, 2000u);
}

/// lexical-ветвь: обычные (usual) ключи c ncmp_lexical — длинные ключи
/// с общими префиксами разной длины (лексикографический цикл сравнения).
TEST(ut_key_comparators, lexical_long_prefixes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-lexical";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("lex");
    // ключи-префиксы: "a", "ab", "abc", ... + длинные варианты — покрытие
    // лексического сравнения с длиной-первичным-отсекателем
    std::string base(64, 'x');
    for (size_t len = 1; len <= 64; ++len) {
      txn.put(map, mdbx::slice(base.substr(0, len)), mdbx::slice("v"), mdbx::upsert);
    }
    for (int i = 0; i < 1500; ++i) {
      auto k = base + std::to_string(100000 + i);
      txn.put(map, mdbx::slice(k), mdbx::slice("v"), mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("lex");
  auto cur = txn.open_cursor(map);
  std::string base(64, 'x');
  for (size_t len = 1; len <= 64; ++len)
    EXPECT_TRUE(cur.find(mdbx::slice(base.substr(0, len)), false).done);
  // upper_bound между префиксами (достаёт срезы сравнения)
  auto ub = cur.upper_bound(mdbx::slice(base.substr(0, 32)), false);
  EXPECT_TRUE(ub.done);
}

/// custom-ветвь: пользовательский компаратор (search_branch_custom +
/// foliage-custom) через mdbx_dbi_open_ex (deprecated, но единственный
/// путь задать custom cmp; подавляем предупреждение локально).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static int gap_cmp(const MDBX_val *a, const MDBX_val *b) noexcept {
  const auto av = std::atoi(static_cast<const char *>(a->iov_base));
  const auto bv = std::atoi(static_cast<const char *>(b->iov_base));
  return (av > bv) - (av < bv);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

TEST(ut_key_comparators, custom_comparator) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-custom";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  MDBX_dbi dbi;
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    ASSERT_EQ(mdbx_dbi_open_ex(txn, "custom", MDBX_CREATE, &dbi,
                               gap_cmp, nullptr), MDBX_SUCCESS);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    for (int i = 0; i < 1500; ++i) {
      auto k = std::to_string(100000 + i);
      MDBX_val key{const_cast<char *>(k.data()), k.size()};
      char vbuf = 'v';
      MDBX_val val{&vbuf, 1};
      ASSERT_EQ(mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
    MDBX_cursor *cur;
    ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cur), MDBX_SUCCESS);
    for (int i = 0; i < 1500; ++i) {
      auto k = std::to_string(100000 + i);
      MDBX_val key{const_cast<char *>(k.data()), k.size()};
      MDBX_val val;
      ASSERT_EQ(mdbx_get_ex(txn, dbi, &key, &val, nullptr), MDBX_SUCCESS);
    }
    mdbx_cursor_close(cur);
    mdbx_txn_abort(txn);
  }
}
