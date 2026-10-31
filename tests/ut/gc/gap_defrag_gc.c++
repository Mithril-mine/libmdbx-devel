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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <random>
#include <vector>

/// SC-76 (цель ≥90%): связка defrag + последующие GC-нагруженные txn.
/// Ключевой факт: wr.gc.ready4reuse наполняется ТОЛЬКО в defrag
/// (rkl_push в defrag.c), поэтому dense-пути gc-put (gc_dense_hist/solve,
/// dense_adjust_amount/chunk, consume_stack/remaining) и
/// defrag_move_cost* активируются только ПОСЛЕ mdbx_env_defrag в
/// последующих GC-транзакциях. SC-70 делал defrag, но не GC после него.

namespace {

MDBX_dbi open_main(MDBX_txn *txn) {
  MDBX_dbi dbi;
  int rc = mdbx_dbi_open(txn, "main", MDBX_DB_DEFAULTS, &dbi);
  EXPECT_EQ(rc, MDBX_SUCCESS) << "dbi_open rc=" << rc << " errno=" << errno
                              << " strerror=" << mdbx_strerror(rc) << "|errno_str=" << strerror(errno);
  return dbi;
}

void fill(MDBX_env *env, uint32_t base, uint32_t count, size_t val_size) {
  MDBX_txn *txn;
  int rc_txn = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  EXPECT_EQ(rc_txn, MDBX_SUCCESS) << "txn_begin at base=" << base;
  if (rc_txn != MDBX_SUCCESS)
    return;
  MDBX_dbi dbi = open_main(txn);
  std::vector<char> val(val_size);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t k = base + i;
    for (size_t j = 0; j < val_size; ++j)
      val[j] = static_cast<char>('a' + (i + j) % 26);
    MDBX_val key{&k, sizeof(k)};
    MDBX_val v{val.data(), val.size()};
    EXPECT_EQ(mdbx_put(txn, dbi, &key, &v, MDBX_UPSERT), MDBX_SUCCESS);
  }
  EXPECT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}

void del_strided(MDBX_env *env, uint32_t base, uint32_t count, uint32_t step) {
  MDBX_txn *txn;
  EXPECT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = open_main(txn);
  for (uint32_t i = 0; i < count; i += step) {
    uint32_t k = base + i;
    MDBX_val key{&k, sizeof(k)};
    EXPECT_EQ(mdbx_del(txn, dbi, &key, nullptr), MDBX_SUCCESS);
  }
  EXPECT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}

size_t entries(MDBX_env *env) {
  MDBX_txn *txn;
  EXPECT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = open_main(txn);
  MDBX_stat stat;
  EXPECT_EQ(mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)), MDBX_SUCCESS);
  mdbx_txn_abort(txn);
  return static_cast<size_t>(stat.ms_entries);
}

} // namespace

static MDBX_dbi dbi_glob;

/// defrag → последующие GC-транзакции: dense-упаковка на ready4reuse.
TEST(ut_gap_defrag_gc, defrag_then_dense_gc_cycles) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "test-gap-dgc";
  mdbx_env_delete(testdb.c_str(), MDBX_ENV_JUST_DELETE);
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, testdb.c_str(), MDBX_NOSTICKYTHREADS, 0644),
            MDBX_SUCCESS);

  // 0. создание таблицы (CREATE единожды)
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi_glob), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  // 1. Наполнение и частичное удаление — фрагментация
  fill(env, 0, 6000, 200);
  del_strided(env, 0, 6000, 2);
  EXPECT_EQ(entries(env), 3000u);

  // 2. defrag: готовит ready4reuse (GC-записи, перемещённые в начало)
#ifndef SKIP_DEFRAG_PROBE
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  EXPECT_EQ(mdbx_env_defrag(env, 1, 0, 16, 0, 0, 4096, nullptr, nullptr, &result),
            MDBX_SUCCESS);
#endif /* SKIP_DEFRAG_PROBE */

  // 3. Последующие GC-нагруженные txn: вставки/удаления поверх
  //    ready4reuse → gc_dense_hist/solve + consume_stack/remaining
  //    + dense_adjust_amount/chunk (loop-счётчик 1/3k)
  std::mt19937 rng(99);
  for (int cycle = 0; cycle < 5; ++cycle) {
    fill(env, 100000u + static_cast<uint32_t>(cycle) * 20000u, 1200, 190);
    del_strided(env, 100000u + static_cast<uint32_t>(cycle) * 20000u, 1200, 2);
  }

  // 4. Целостность: 3000 исходных нечётных + 5 циклов × 600 выживших
  EXPECT_EQ(entries(env), 3000u + 5 * 600u);

  mdbx_env_close(env);
  (void)rng;
}

/// Двойной defrag с промежуточной нагрузкой: defrag_move_cost кэшируется
/// (defrag_move_cost vs uncached) — повторный проход по окрестностям.
TEST(ut_gap_defrag_gc, double_defrag_with_load) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "test-gap-dgc2";
  mdbx_env_delete(testdb.c_str(), MDBX_ENV_JUST_DELETE);
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, testdb.c_str(), MDBX_NOSTICKYTHREADS, 0644),
            MDBX_SUCCESS);
  // создание 'main' (CREATE единожды; далее dbi_open с DB_DEFAULTS)
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi_glob), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }

  fill(env, 0, 4000, 180);
  del_strided(env, 0, 4000, 2);

  MDBX_defrag_result_t r1;
  memset(&r1, 0, sizeof(r1));
  EXPECT_EQ(mdbx_env_defrag(env, 0, 0, 0, 0, 0, 2048, nullptr, nullptr, &r1),
            MDBX_SUCCESS);

  // нагрузка между defrag'ами: страницы в окрестностях moved-спанов
  fill(env, 200000, 1000, 170);
  del_strided(env, 200000, 1000, 3);

  MDBX_defrag_result_t r2;
  memset(&r2, 0, sizeof(r2));
  EXPECT_EQ(mdbx_env_defrag(env, 0, 0, 0, 0, 0, 2048, nullptr, nullptr, &r2),
            MDBX_SUCCESS);
  // второй проход: страницы_whole не уменьшаются некорректно
  EXPECT_GT(r2.pages_whole, 0u);

  EXPECT_EQ(entries(env), 2000u + 1000u - 334u);

  mdbx_env_close(env);
}