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

/// SC-71 (test-coverage-gaps.md): gc-put/gc-get dense-пути —
/// gc_dense_hist/gc_dense_solve/dense_adjust_chunk/amount и
/// consume_stack/consume_remaining активируются, когда в repnl есть
/// ПОСЛЕДОВАТЕЛЬНОСТИ смежных страниц, а ready4reuse непуст. Для этого
/// нужны ОДНОВРЕМЕННО: (1) большая txn, создающая длинные retired-списки
/// (крупные записи → много страниц), (2) массовое удаление, освобождающее
/// смежные span'ы, (3) длинная серия GC-транзакций (loop-счётчик 1/3k).

namespace {

MDBX_dbi open_main(MDBX_txn *txn) {
  MDBX_dbi dbi;
  // DBI-хэндлы кэшируются по txn: повторный open с теми же флагами —
  // no-op; в read-txn CREATE запрещён, поэтому пробуем по флагам txn.
  int rc = mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi);
  if (rc != MDBX_SUCCESS &&
      (mdbx_txn_flags(txn) & MDBX_TXN_READWRITE) == 0) {
    rc = mdbx_dbi_open(txn, "main", MDBX_DB_DEFAULTS, &dbi);
  }
  EXPECT_EQ(rc, MDBX_SUCCESS) << "rc=" << rc;
  return dbi;
}

void put_range(MDBX_env *env, uint32_t base, uint32_t count, size_t val_size, unsigned seed) {
  MDBX_txn *txn;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = open_main(txn);
  std::mt19937 rng(seed);
  std::vector<char> val(val_size);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t k = base + i;
    for (size_t j = 0; j < val_size; ++j)
      val[j] = static_cast<char>('a' + (i + j + seed) % 26);
    MDBX_val key{&k, sizeof(k)};
    MDBX_val v{val.data(), val.size()};
    ASSERT_EQ(mdbx_put(txn, dbi, &key, &v, MDBX_UPSERT), MDBX_SUCCESS);
    if ((i & 1023) == 1023) {
      // периодический коммит: кластеры retired-страниц, но с дольшими
      // непрерывными span'ами внутри одной большой txn
      ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
      ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
      dbi = open_main(txn);
    }
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}

void del_span(MDBX_env *env, uint32_t base, uint32_t count) {
  // Удаляем ПОДРЯД (смежные ключи → смежные страницы → длинные
  // последовательности в repnl — активация dense-алгоритма)
  MDBX_txn *txn;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = open_main(txn);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t k = base + i;
    MDBX_val key{&k, sizeof(k)};
    ASSERT_EQ(mdbx_del(txn, dbi, &key, nullptr), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}

size_t count_entries(MDBX_env *env) {
  MDBX_txn *txn;
  EXPECT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = open_main(txn);
  MDBX_stat stat;
  EXPECT_EQ(mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)), MDBX_SUCCESS);
  mdbx_txn_abort(txn);
  return static_cast<size_t>(stat.ms_entries);
}

} // namespace

/// Плотная упаковка GC: большие span'ы смежных освобождённых страниц.
TEST(ut_gc_dense_packing, span_reclaim_with_dense_packing) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "test-gap-gc-dense";
  mdbx_env_delete(testdb.c_str(), MDBX_ENV_JUST_DELETE);
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, testdb.c_str(), MDBX_NOSTICKYTHREADS, 0644), MDBX_SUCCESS);

  // 1. 3 кластера по 2000 записей с крупными значениями (по ~2КБ →
  //    ~500 страниц на кластер, страницы физически смежные).
  put_range(env, 0, 2000, 2048, 1);
  put_range(env, 100000, 2000, 2048, 2);
  put_range(env, 200000, 2000, 2048, 3);
  EXPECT_EQ(count_entries(env), 6000u);

  // 2. Массовое удаление ПОДРЯД — освобождает смежные span'ы страниц
  //    (длинные последовательности в repnl → gc_dense_hist/solve).
  del_span(env, 0, 2000);
  del_span(env, 100000, 2000);
  EXPECT_EQ(count_entries(env), 2000u);

  // 3. Серия GC-циклов: вставка/удаление по-крупному — каждая txn
  //    резервирует GC-куски из длинных span'ов (loop 1/3k → dense_adjust).
  for (int cycle = 0; cycle < 4; ++cycle) {
    put_range(env, 300000 + cycle * 10000u, 800, 2048, static_cast<unsigned>(10 + cycle));
    del_span(env, 300000 + cycle * 10000u, 800);
  }

  // 4. Целостность: третий кластер жив целиком.
  EXPECT_EQ(count_entries(env), 2000u);
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi = open_main(txn);
    MDBX_cursor *cur;
    ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cur), MDBX_SUCCESS);
    size_t n = 0;
    MDBX_val key, val;
    int rc = mdbx_cursor_get(cur, &key, &val, MDBX_FIRST);
    while (rc == MDBX_SUCCESS) {
      ++n;
      rc = mdbx_cursor_get(cur, &key, &val, MDBX_NEXT);
    }
    EXPECT_EQ(n, 2000u);
    mdbx_cursor_close(cur);
    mdbx_txn_abort(txn);
  }
  mdbx_env_close(env);
}

/// Чередование крупных и мелких записей → фрагментированные chunk-запросы,
/// dense_adjust_chunk binary-search путь (chunk > maxgc_large1page).
TEST(ut_gc_dense_packing, mixed_sizes_force_chunk_adjustment) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "test-gap-gc-mixed";
  mdbx_env_delete(testdb.c_str(), MDBX_ENV_JUST_DELETE);
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, testdb.c_str(), MDBX_NOSTICKYTHREADS, 0644), MDBX_SUCCESS);

  std::mt19937 rng(77);
  std::vector<size_t> sizes = {64, 4096, 512, 16384, 128, 8192};
  // 1. Вставка с чередующимися размерами (крупные → overflow-страницы)
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi = open_main(txn);
    for (uint32_t i = 0; i < 4000; ++i) {
      const size_t sz = sizes[i % sizes.size()];
      std::vector<char> val(sz);
      for (size_t j = 0; j < sz; ++j)
        val[j] = static_cast<char>('a' + (i + j) % 26);
      uint32_t k = i * 3 + 1;
      MDBX_val key{&k, sizeof(k)};
      MDBX_val v{val.data(), val.size()};
      ASSERT_EQ(mdbx_put(txn, dbi, &key, &v, MDBX_UPSERT), MDBX_SUCCESS);
      if ((i & 511) == 511) {
        ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
        ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
        dbi = open_main(txn);
      }
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  // 2. Удаление каждой третьей — дыры разных размеров вокруг overflow-страниц
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi = open_main(txn);
    for (uint32_t i = 0; i < 4000; i += 3) {
      uint32_t k = i * 3 + 1;
      MDBX_val key{&k, sizeof(k)};
      ASSERT_EQ(mdbx_del(txn, dbi, &key, nullptr), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  // 3. GC-циклы с изменчивым fill — активируют dense_adjust_chunk/amount
  for (int cycle = 0; cycle < 3; ++cycle) {
    for (uint32_t i = 0; i < 1500; ++i) {
      const size_t sz = sizes[(i + cycle) % sizes.size()];
      std::vector<char> val(sz);
      for (size_t j = 0; j < sz; ++j)
        val[j] = static_cast<char>('A' + (i + j + cycle) % 26);
      uint32_t k = 500000u + cycle * 100000u + i * 5 + 2;
      MDBX_txn *txn;
      ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
      MDBX_dbi dbi = open_main(txn);
      MDBX_val key{&k, sizeof(k)};
      MDBX_val v{val.data(), val.size()};
      ASSERT_EQ(mdbx_put(txn, dbi, &key, &v, MDBX_UPSERT), MDBX_SUCCESS);
      ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
    }
  }
  // 4. Целостность
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi = open_main(txn);
    MDBX_stat stat;
    ASSERT_EQ(mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)), MDBX_SUCCESS);
    // 4000 - 1334 (удалённые) + 4500 (3 цикла по 1500) = 7166
    EXPECT_EQ(stat.ms_entries, 2666u + 4500u);
    mdbx_txn_abort(txn);
  }
  mdbx_env_close(env);
  (void)rng;
}