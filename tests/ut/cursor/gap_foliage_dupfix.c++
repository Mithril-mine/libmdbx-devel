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

/// SC-81 (цель ≥90%): search_foliage_lexical/ordinal_dupfix —
/// inner-cursor dupfix-листва с lexical/ordinal-компараторами.
/// Требуется: DUPSORT + value_mode::multi_samelength (DUPFIXED) +
/// nested B+tree значений (сотни на ключ) + inner-cursor навигация.
/// SC-74 ставил value_mode::multi (переменные значения) — dupfix-ветвь
/// не активировалась. Здесь — фиксированная длина значений.

namespace {
constexpr uint32_t KEYS = 8;
constexpr uint32_t VALS_PER_KEY = 2000;
} // namespace

/// lexical_dupfix: usual-ключи + DUPSORT + фиксированные значения.
TEST(ut_gap_foliage_dupfix, lexical_dupfix) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-fld-lex";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("lex-dupfix", mdbx::key_mode::usual,
                              mdbx::value_mode::multi_samelength);
    for (uint32_t k = 0; k < KEYS; ++k) {
      std::string key = "lexd-" + std::to_string(100000 + k);
      for (uint32_t v = 0; v < VALS_PER_KEY; ++v) {
        uint32_t value = k * VALS_PER_KEY + v; // фиксированные 4 байта
        txn.put(map, mdbx::slice(key), mdbx::slice(&value, sizeof(value)),
                mdbx::upsert);
      }
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("lex-dupfix", mdbx::key_mode::usual,
                          mdbx::value_mode::multi_samelength);
  auto cur = txn.open_cursor(map);
  // inner-cursor: outer find → dupsort_setup(N_TREE) → inner seek
  for (uint32_t k = 0; k < KEYS; ++k) {
    std::string key = "lexd-" + std::to_string(100000 + k);
    EXPECT_TRUE(cur.find(mdbx::slice(key), false).done);
    auto fv = cur.move(mdbx::cursor::move_operation::multi_currentkey_firstvalue, false);
    EXPECT_TRUE(fv.done);
    size_t n = 1;
    while (cur.move(mdbx::cursor::move_operation::multi_currentkey_nextvalue, false))
      ++n;
    EXPECT_EQ(n, VALS_PER_KEY);
    // точная пара внутри dupfix-листвы (SET_KEY/GET_BOTH на inner)
    uint32_t target = k * VALS_PER_KEY + 1234;
    auto found = cur.find_multivalue(mdbx::slice(key),
                                     mdbx::slice(&target, sizeof(target)), false);
    EXPECT_TRUE(found.done);
    EXPECT_EQ(found.value, mdbx::slice(&target, sizeof(target)));
  }
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::first, true).done);
  size_t groups = 1;
  while (cur.move(mdbx::cursor::move_operation::multi_nextkey_firstvalue, false))
    ++groups;
  EXPECT_EQ(groups, KEYS);
}

/// ordinal_dupfix: INTEGERKEY + DUPSORT + фиксированные значения.
TEST(ut_gap_foliage_dupfix, ordinal_dupfix) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-gap-fld-ord";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(),
                        mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("ord-dupfix", mdbx::key_mode::ordinal,
                              mdbx::value_mode::multi_samelength);
    for (uint32_t k = 0; k < KEYS; ++k) {
      for (uint32_t v = 0; v < VALS_PER_KEY; ++v) {
        uint32_t value = k * VALS_PER_KEY + v;
        txn.put(map, mdbx::slice(&k, sizeof(k)), mdbx::slice(&value, sizeof(value)),
                mdbx::upsert);
      }
    }
    txn.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("ord-dupfix", mdbx::key_mode::ordinal,
                          mdbx::value_mode::multi_samelength);
  auto cur = txn.open_cursor(map);
  for (uint32_t k = 0; k < KEYS; ++k) {
    mdbx::slice key(&k, sizeof(k));
    EXPECT_TRUE(cur.find(key, false).done);
    EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::multi_currentkey_firstvalue, false).done);
    size_t n = 1;
    while (cur.move(mdbx::cursor::move_operation::multi_currentkey_nextvalue, false))
      ++n;
    EXPECT_EQ(n, VALS_PER_KEY);
    uint32_t target = k * VALS_PER_KEY + 432; // внутри [0, VALS_PER_KEY)
    EXPECT_TRUE(cur.find_multivalue(key, mdbx::slice(&target, sizeof(target)), false).done);
  }
  EXPECT_TRUE(cur.move(mdbx::cursor::move_operation::last, true).done);
  size_t groups = 1;
  while (cur.move(mdbx::cursor::move_operation::multi_nextkey_firstvalue, false))
    ++groups;
  EXPECT_EQ(groups, KEYS);
}