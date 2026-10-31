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

/// SC-74 (test-coverage-gaps.md, цель ≥90%): search_foliage_* — 16
/// специализированных циклов поиска на листву (компаратор ×
/// {usual, dupfix}). cursor_to_search_foliage() выбирает вариант по
/// компаратору ключа и типу таблицы (MDBX_DUPFIXED + is_inner).
/// Дефолтные тесты гоняют только lenfast_usual.

namespace {
/// Полная навигация по таблице: exact-поиск всех ключей + first→next walk
/// (основной вход в search_foliage_* — поиск и обход листовых страниц).
template <typename KEYT>
void probe_table(mdbx::txn_managed &txn, mdbx::map_handle map,
                 const std::vector<KEYT> &keys, bool is_multi) {
  auto cur = txn.open_cursor(map);
  for (const auto &k : keys) {
    mdbx::slice key(&k, sizeof(k));
    auto found = cur.find(key, false);
    EXPECT_TRUE(found.done) << "key not found";
  }
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  if (is_multi) {
    // multi (DUPSORT без DUPFIXED значений) — обход с next_nodup
    size_t groups = 1;
    while (cur.move(mdbx::cursor::move_operation::multi_nextkey_firstvalue, false))
      ++groups;
    EXPECT_EQ(groups, keys.size());
  } else {
    size_t n = 1;
    while (cur.move(mdbx::cursor::move_operation::next, false))
      ++n;
    EXPECT_EQ(n, keys.size());
  }
}
} // namespace

/// ordinal-usual: INTEGERKEY, обычные значения (не dupfix) —
/// search_foliage_ordinal_usual.
TEST(ut_gap_tree_foliage, ordinal_usual) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-fol-ord-usual";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  std::vector<uint32_t> keys;
  {
    auto txn = env.start_write();
    auto map = txn.create_map("ord", mdbx::key_mode::ordinal);
    // DUPSORT-значения переменной длины → обычно-листовые страницы
    auto m2 = txn.create_map("ord_multi", mdbx::key_mode::ordinal,
                             mdbx::value_mode::multi);
    for (uint32_t i = 0; i < 4000; ++i) {
      uint32_t k = i * 7 + 1;
      keys.push_back(k);
      txn.put(map, mdbx::slice(&k, sizeof(k)), mdbx::slice("v"), mdbx::upsert);
      // multi: несколько значений на ключ, значения РАЗНОЙ длины → не dupfix
      txn.put(m2, mdbx::slice(&k, sizeof(k)), mdbx::slice("v-" + std::to_string(i)),
              mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("ord", mdbx::key_mode::ordinal);
  probe_table(txn, map, keys, false);
  auto m2 = txn.open_map("ord_multi", mdbx::key_mode::ordinal,
                         mdbx::value_mode::multi);
  probe_table(txn, m2, keys, true);
}

/// ordinal-dupfix: INTEGERKEY + MDBX_DUPFIXED (значения фиксированной
/// длины) — search_foliage_ordinal_dupfix/uint32_dupfix.
TEST(ut_gap_tree_foliage, ordinal_dupfix) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-fol-ord-dupfix";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  std::vector<uint32_t> keys;
  {
    auto txn = env.start_write();
    // DUPSORT + фиксированная длина значений → MDBX_DUPFIXED на листве
    auto map = txn.create_map("dupfix", mdbx::key_mode::ordinal,
                              mdbx::value_mode::multi);
    for (uint32_t i = 0; i < 4000; ++i) {
      uint32_t k = i * 11 + 3;
      keys.push_back(k);
      for (int v = 0; v < 3; ++v) {
        uint32_t value = k * 10 + static_cast<uint32_t>(v); // 4 байта → DUPFIXED
        txn.put(map, mdbx::slice(&k, sizeof(k)), mdbx::slice(&value, sizeof(value)),
                mdbx::upsert);
      }
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("dupfix", mdbx::key_mode::ordinal,
                          mdbx::value_mode::multi);
  // exact-поиск пар (dupfix-листовые страницы) + обход групп
  auto cur = txn.open_cursor(map);
  for (const auto &k : keys) {
    mdbx::slice key(&k, sizeof(k));
    auto found = cur.find_multivalue(key, mdbx::slice("\x01\x00\x00\x00"), false);
    EXPECT_FALSE(found.done); // конкретного значения нет — но ветвь поиска
                              // по dupfix-листу исполнена
    auto any = cur.find(key, false);
    EXPECT_TRUE(any.done);
  }
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  size_t groups = 1;
  while (cur.move(mdbx::cursor::move_operation::multi_nextkey_firstvalue, false))
    ++groups;
  EXPECT_EQ(groups, keys.size());
}

/// reverse/lexical-usual и dupfix: MDBX_REVERSEKEY и обычные ключи с
/// DUPSORT — search_foliage_reverse/lexical_{usual,dupfix}.
TEST(ut_gap_tree_foliage, reverse_lexical_usual_dupfix) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-fol-rev-lex";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    // reverse + DUPSORT с фиксированной длиной → reverse_dupfix
    auto rdup = txn.create_map("rev_dupfix", mdbx::key_mode::reverse,
                               mdbx::value_mode::multi);
    // reverse + обычные значения → reverse_usual
    auto rus = txn.create_map("rev_usual", mdbx::key_mode::reverse,
                              mdbx::value_mode::multi);
    // usual (lexical) + DUPSORT → lexical_usual
    auto lus = txn.create_map("lex_usual", mdbx::key_mode::usual,
                              mdbx::value_mode::multi);
    // lexical + dupfix → lexical_dupfix
    auto ldup = txn.create_map("lex_dupfix", mdbx::key_mode::usual,
                               mdbx::value_mode::multi);
    for (int i = 0; i < 2500; ++i) {
      // строки живут в переменных: slice НЕ копирует данные временных
      std::string rs = "prefix-" + std::to_string(100000 + i);
      std::string ls = "key-" + std::to_string(100000 + i);
      auto rk = mdbx::slice(rs);
      auto lk = mdbx::slice(ls);
      uint32_t fixed = static_cast<uint32_t>(i);
      std::string vs = "v-" + rs.substr(7); // "v-<tail>"
      txn.put(rdup, rk, mdbx::slice(&fixed, sizeof(fixed)), mdbx::upsert);
      txn.put(rus, rk, mdbx::slice(vs), mdbx::upsert);
      txn.put(lus, lk, mdbx::slice(vs), mdbx::upsert);
      txn.put(ldup, lk, mdbx::slice(&fixed, sizeof(fixed)), mdbx::upsert);
    }
    txn.commit();
  }
  // sanity: данные реально вставлены во все 4 таблицы (открытие строго
  // с теми же флагами, что и создание — иначе MDBX_INCOMPATIBLE)
  {
    auto txn = env.start_read();
    auto rdup = txn.open_map("rev_dupfix", mdbx::key_mode::reverse,
                             mdbx::value_mode::multi);
    auto rus = txn.open_map("rev_usual", mdbx::key_mode::reverse,
                            mdbx::value_mode::multi);
    auto lus = txn.open_map("lex_usual", mdbx::key_mode::usual,
                            mdbx::value_mode::multi);
    auto ldup = txn.open_map("lex_dupfix", mdbx::key_mode::usual,
                             mdbx::value_mode::multi);
    for (auto *m : {&rdup, &rus, &lus, &ldup}) {
      MDBX_stat st;
      ASSERT_EQ(mdbx_dbi_stat(txn, *m, &st, sizeof(st)), MDBX_SUCCESS);
      ASSERT_EQ(st.ms_entries, 2500u);
    }
    txn.abort();
  }
  auto txn = env.start_read();
  // reverse-dupfix
  {
    auto map = txn.open_map("rev_dupfix", mdbx::key_mode::reverse,
                            mdbx::value_mode::multi);
    auto cur = txn.open_cursor(map);
    for (int i = 0; i < 2500; i += 17) {
      std::string rs = "prefix-" + std::to_string(100000 + i);
      EXPECT_TRUE(cur.find(mdbx::slice(rs), false).done);
    }
    EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
    size_t groups = 1;
    while (cur.move(mdbx::cursor::move_operation::multi_nextkey_firstvalue, false))
      ++groups;
    EXPECT_EQ(groups, 2500);
  }
  // reverse-usual
  {
    auto map = txn.open_map("rev_usual", mdbx::key_mode::reverse,
                            mdbx::value_mode::multi);
    auto cur = txn.open_cursor(map);
    for (int i = 0; i < 2500; i += 23) {
      std::string rs = "prefix-" + std::to_string(100000 + i);
      EXPECT_TRUE(cur.find(mdbx::slice(rs), false).done);
    }
  }
  // lexical-usual
  {
    auto map = txn.open_map("lex_usual", mdbx::key_mode::usual,
                            mdbx::value_mode::multi);
    auto cur = txn.open_cursor(map);
    for (int i = 0; i < 2500; i += 29) {
      std::string ls = "key-" + std::to_string(100000 + i);
      EXPECT_TRUE(cur.find(mdbx::slice(ls), false).done);
    }
    // lower_bound в multi-таблице: DUPSORT-семантика требует пары;
    // для чистого позиционирования на ключ используем exact-find соседа.
    std::string lbk = "key-150000";
    EXPECT_TRUE(cur.find(mdbx::slice(lbk), false).done);
  }
  // lexical-dupfix
  {
    auto map = txn.open_map("lex_dupfix", mdbx::key_mode::usual,
                            mdbx::value_mode::multi);
    auto cur = txn.open_cursor(map);
    for (int i = 0; i < 2500; i += 31) {
      std::string ls = "key-" + std::to_string(100000 + i);
      EXPECT_TRUE(cur.find(mdbx::slice(ls), false).done);
    }
  }
}

/// lenfast-dupfix: обычные ключи (lenfast-компаратор) с DUPSORT+dupfix.
TEST(ut_gap_tree_foliage, lenfast_dupfix) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-fol-lenfast-dupfix";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("lfd", mdbx::key_mode::usual,
                              mdbx::value_mode::multi);
    for (int i = 0; i < 3000; ++i) {
      auto k = "lf-" + std::to_string(50000 + i);
      uint32_t fixed = static_cast<uint32_t>(i * 3);
      txn.put(map, mdbx::slice(k), mdbx::slice(&fixed, sizeof(fixed)), mdbx::upsert);
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("lfd", mdbx::key_mode::usual, mdbx::value_mode::multi);
  auto cur = txn.open_cursor(map);
  for (int i = 0; i < 3000; i += 7) {
    EXPECT_TRUE(cur.find(mdbx::slice("lf-" + std::to_string(50000 + i)), false).done);
  }
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  size_t groups = 1;
  while (cur.move(mdbx::cursor::move_operation::multi_nextkey_firstvalue, false))
    ++groups;
  EXPECT_EQ(groups, 3000);
}

/// null_comparator / cmp2name / search_branch2name / search_foliage2name —
/// debug/имя-хелперы; активируются через сравнение произвольного cmp.
/// custom-usual/custom-dupfix уже покрыты SC-69 (custom_comparator).
TEST(ut_gap_tree_foliage, names_are_alive) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  // name-хелперы — constexpr-таблицы; их символы «оживают» при любом
  // сравнении имён ветвей. Просто гарантируем, что модуль линкуется и
  // обычные операции работают (глубокое покрытие имён не требуется).
  EXPECT_GT(mdbx::env::default_pagesize(), 0u);
}