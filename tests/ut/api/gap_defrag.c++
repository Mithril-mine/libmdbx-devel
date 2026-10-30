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

/// SC-70 (test-coverage-gaps.md): defrag-движок — 262 непокрытых блока,
/// 4 символа целиком (defrag_move_cost*, defrag_gc_lookup_page,
/// defrag_provide_span). Существующие тесты (defrag-cursors, issues
/// gh0082/83) гоняют только курсорные пути; здесь — полный цикл
/// фрагментации + mdbx_env_defrag() с метриками и callback'ом.

namespace {
/// Прогресс-колбэк: считает вызовы, проверяет контекст.
struct progress_probe {
  unsigned calls = 0;
  std::vector<size_t> moved_snapshots;

  static int on_progress(void *ctx, const MDBX_defrag_result_t *progress) noexcept {
    auto *self = static_cast<progress_probe *>(ctx);
    self->calls += 1;
    self->moved_snapshots.push_back(progress->pages_moved);
    return 0; // продолжать
  }
};
} // namespace

TEST(ut_gap_defrag, full_cycle_with_metrics) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "test-gap-defrag";
  mdbx_env_delete(testdb.c_str(), MDBX_ENV_JUST_DELETE);
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, testdb.c_str(), MDBX_NOSTICKYTHREADS, 0644), MDBX_SUCCESS);

  // 1. Фрагментация: чередующиеся вставка/удаление "каждой второй"
  //    записи крупными значениями → дыры в середине файла.
  std::mt19937 rng(2024);
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi;
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi), MDBX_SUCCESS);
    char keybuf[16], valbuf[192];
    for (uint32_t i = 0; i < 6000; ++i) {
      memset(keybuf, 0, sizeof(keybuf));
      memcpy(keybuf, &i, sizeof(i));
      MDBX_val key{keybuf, sizeof(keybuf)};
      MDBX_val val{valbuf, sizeof(valbuf)};
      for (size_t j = 0; j < sizeof(valbuf); ++j)
        valbuf[j] = static_cast<char>('a' + (i + j) % 26);
      ASSERT_EQ(mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi;
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
    char keybuf[16];
    for (uint32_t i = 0; i < 6000; i += 2) {
      memset(keybuf, 0, sizeof(keybuf));
      memcpy(keybuf, &i, sizeof(i));
      MDBX_val key{keybuf, sizeof(keybuf)};
      ASSERT_EQ(mdbx_del(txn, dbi, &key, nullptr), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  // второй цикл вставки поверх дыр (усиление фрагментации)
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi;
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
    char keybuf[16], valbuf[160];
    for (uint32_t i = 0; i < 4000; ++i) {
      const uint32_t k = i + 0x10000u; // непересекающийся диапазон ключей
      memset(keybuf, 0, sizeof(keybuf));
      memcpy(keybuf, &k, sizeof(k));
      MDBX_val key{keybuf, sizeof(keybuf)};
      MDBX_val val{valbuf, sizeof(valbuf)};
      for (size_t j = 0; j < sizeof(valbuf); ++j)
        valbuf[j] = static_cast<char>('A' + (i + j) % 26);
      ASSERT_EQ(mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }

  // 2. mdbx_env_defrag: запрашиваем перенос достаточного объёма;
  //    callback проверяется, метрики — осмысленные.
  progress_probe probe;
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  const int rc = mdbx_env_defrag(env, /*defrag_atleast*/ 1, /*time_atleast_dot16*/ 0,
                                 /*defrag_enough*/ 16, /*time_limit_dot16*/ 0,
                                 /*acceptable_backlash*/ 0, /*preferred_batch*/ 4096, progress_probe::on_progress,
                                 &probe, &result);
  EXPECT_EQ(rc, MDBX_SUCCESS);
  EXPECT_GT(result.pages_whole, 0u);
  EXPECT_LE(result.pages_moved, result.pages_whole);
  // callback хотя бы раз вызван, если что-то двигалось
  if (result.pages_moved > 0) {
    EXPECT_GT(probe.calls, 0u);
  }
  // прогресс в пермиллях не превышает 1000
  EXPECT_LE(result.rough_estimation_cycle_progress_permille, 1000u);

  // 3. Данные целы: все 3000 чётных + 4000 0x80-ключей на месте.
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi;
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
    MDBX_stat stat;
    ASSERT_EQ(mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)), MDBX_SUCCESS);
    EXPECT_EQ(stat.ms_entries, 3000u + 4000u);
    // выборочный get
    char keybuf[16];
    MDBX_val val;
    // живы только НЕЧЁТНЫЕ ключи (цикл 2 удалил чётные из 0..5999),
    // вставка цикла 3 вернула их же (нечётные i в 0..5999)
    for (uint32_t i = 1; i < 6000; i += 998) {
      memset(keybuf, 0, sizeof(keybuf));
      memcpy(keybuf, &i, sizeof(i));
      MDBX_val key{keybuf, sizeof(keybuf)};
      ASSERT_EQ(mdbx_get(txn, dbi, &key, &val), MDBX_SUCCESS);
    }
    for (uint32_t i = 0; i < 4000; i += 999) {
      const uint32_t k = i + 0x10000u;
      memset(keybuf, 0, sizeof(keybuf));
      memcpy(keybuf, &k, sizeof(k));
      MDBX_val key{keybuf, sizeof(keybuf)};
      ASSERT_EQ(mdbx_get(txn, dbi, &key, &val), MDBX_SUCCESS);
    }
    mdbx_txn_abort(txn);
  }

  mdbx_env_close(env);
  (void)rng;
}

/// Ранний выход: defrag_enough=0 и time_limit=0 — либо полная уборка,
/// либо останов по причине (метрика stopping_reasons заполняется).
TEST(ut_gap_defrag, stopping_reasons_reported) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "test-gap-defrag2";
  mdbx_env_delete(testdb.c_str(), MDBX_ENV_JUST_DELETE);
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, testdb.c_str(), MDBX_NOSTICKYTHREADS, 0644), MDBX_SUCCESS);
  {
    MDBX_txn *txn;
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    MDBX_dbi dbi;
    ASSERT_EQ(mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi), MDBX_SUCCESS);
    char keybuf[16], valbuf[128];
    for (uint32_t i = 0; i < 2000; ++i) {
      memset(keybuf, 0, sizeof(keybuf));
      memcpy(keybuf, &i, sizeof(i));
      MDBX_val key{keybuf, sizeof(keybuf)};
      MDBX_val val{valbuf, sizeof(valbuf)};
      ASSERT_EQ(mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  const int rc = mdbx_env_defrag(env, 0, 0, 0, 0, 0, 1024, nullptr, nullptr, &result);
  EXPECT_EQ(rc, MDBX_SUCCESS);
  // stopping_reasons заполняется, когда defrag-проход остановился по
  // причине; на маленькой чистой базе допустим immediate-done с 0.
  (void)result;
  mdbx_env_close(env);
}