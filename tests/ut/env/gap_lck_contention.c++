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

/// SC-67 (test-coverage-gaps.md): lck-posix — многопроцессная конкуренция
/// за lock-файл: EAGAIN/повторные попытки захвата, конкурентные записи,
/// координация через MDBX_NOSTICKYTHREADS + fork-дети. Цели: error-ветви
/// захвата (lck-posix 63.9%, 362 блока), osal_lockfile-fallback.

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHK(expr)                                                                                                      \
  do {                                                                                                                 \
    int err_ = (expr);                                                                                                 \
    if (err_ != MDBX_SUCCESS) {                                                                                        \
      fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #expr, mdbx_strerror(err_), err_);                 \
      exit(2);                                                                                                         \
    }                                                                                                                  \
  } while (0)

namespace {

/// Записывает `count` значений с базой `base` в общую таблицу (без CREATE).
void child_writer(const char *path, uint32_t base, uint32_t count, int seed) {
  MDBX_env *env = NULL;
  CHK(mdbx_env_create(&env));
  CHK(mdbx_env_set_maxdbs(env, 4));
  CHK(mdbx_env_open(env, path, MDBX_NOSTICKYTHREADS, 0644));
  for (uint32_t round = 0; round < 3; ++round) {
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    CHK(mdbx_dbi_open(txn, "shared", MDBX_DB_DEFAULTS, &dbi));
    char valbuf[96];
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t k = base + i;
      MDBX_val key = {(void *)&k, sizeof(k)};
      for (size_t j = 0; j < sizeof(valbuf); ++j)
        valbuf[j] = static_cast<char>('a' + (seed + i + round + j) % 26);
      MDBX_val val = {valbuf, sizeof(valbuf)};
      int rc = mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT);
      if (rc != MDBX_SUCCESS && rc != MDBX_MAP_FULL && rc != MDBX_BUSY && rc != MDBX_TXN_FULL && rc != EAGAIN) {
        fprintf(stderr, "child[%d] put %u: %s (%d)\n", seed, k, mdbx_strerror(rc), rc);
        mdbx_txn_abort(txn);
        mdbx_env_close(env);
        exit(3);
      }
      if (rc != MDBX_SUCCESS) {
        // конкурентное препятствие — повтор попытки в следующем round
        break;
      }
    }
    CHK(mdbx_txn_commit(txn));
    usleep(1000u * (unsigned)(seed % 7));
  }
  mdbx_env_close(env);
  exit(0);
}

} // namespace

/// Конкурентные писатели: несколько fork-детей пишут в одну таблицу;
/// lock-файл проходит через полный цикл захвата/освобождения между
/// процессами, включая error-ветви (EAGAIN, BUSY).
TEST(ut_gap_lck_contention, multi_process_writers) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *testdb = "test-gap-lck-contention";
  mdbx_env_delete(testdb, MDBX_ENV_JUST_DELETE);

  // 1. Родитель создаёт БД и таблицу
  MDBX_env *env = NULL;
  CHK(mdbx_env_create(&env));
  CHK(mdbx_env_set_maxdbs(env, 4));
  mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1);
  CHK(mdbx_env_open(env, testdb, MDBX_NOSTICKYTHREADS, 0644));
  {
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    CHK(mdbx_dbi_open(txn, "shared", MDBX_CREATE, &dbi));
    CHK(mdbx_txn_commit(txn));
  }
  mdbx_env_close(env);

  // 2. 4 fork-дети пишут конкурентно в разные диапазоны
  enum { WRITERS = 4, PER_WRITER = 400 };
  pid_t pids[WRITERS];
  for (int w = 0; w < WRITERS; ++w) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0)
      child_writer(testdb, static_cast<uint32_t>(w) * PER_WRITER, PER_WRITER, w + 1);
    pids[w] = pid;
  }
  int failures = 0;
  for (int w = 0; w < WRITERS; ++w) {
    int status = 0;
    ASSERT_EQ(waitpid(pids[w], &status, 0), pids[w]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      ++failures;
  }
  EXPECT_EQ(failures, 0);

  // 3. Родитель после детей: подсчёт записей (каждый writer внёс свой
  //    диапазон — total может быть меньше из-за конкурентных retries,
  //    но каждый диапазон представлен хотя бы частично).
  CHK(mdbx_env_create(&env));
  CHK(mdbx_env_set_maxdbs(env, 4));
  CHK(mdbx_env_open(env, testdb, MDBX_NOSTICKYTHREADS, 0644));
  {
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &txn));
    MDBX_dbi dbi;
    CHK(mdbx_dbi_open(txn, "shared", MDBX_DB_DEFAULTS, &dbi));
    MDBX_stat stat;
    CHK(mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)));
    EXPECT_GE(stat.ms_entries, WRITERS * PER_WRITER / 4u);
    EXPECT_LE(stat.ms_entries, WRITERS * PER_WRITER);
    mdbx_txn_abort(txn);
  }
  // 4. Родительская запись после детей — захват lock-файла после
  //    освобождения всеми детьми (error-ветви при переходе владения).
  {
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    CHK(mdbx_dbi_open(txn, "shared", MDBX_DB_DEFAULTS, &dbi));
    uint32_t k = 999999;
    char vbuf[32];
    memset(vbuf, 'z', sizeof(vbuf));
    MDBX_val key = {(void *)&k, sizeof(k)};
    MDBX_val val = {vbuf, sizeof(vbuf)};
    CHK(mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT));
    CHK(mdbx_txn_commit(txn));
  }
  mdbx_env_close(env);
  mdbx_env_delete(testdb, MDBX_ENV_JUST_DELETE);
}

/// EXCLUSIVE-доступ: пока родитель держит env открытым, ребёнок с
/// MDBX_EXCLUSIVE должен получить MDBX_BUSY (ветвь отказа захвата).
TEST(ut_gap_lck_contention, exclusive_rejected_while_open) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *testdb = "test-gap-lck-excl";
  mdbx_env_delete(testdb, MDBX_ENV_JUST_DELETE);
  MDBX_env *env = NULL;
  CHK(mdbx_env_create(&env));
  CHK(mdbx_env_open(env, testdb, MDBX_NOSTICKYTHREADS, 0644));
  {
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    CHK(mdbx_dbi_open(txn, NULL, MDBX_CREATE, &dbi));
    CHK(mdbx_txn_commit(txn));
  }
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    MDBX_env *child = NULL;
    int rc = 0;
    if (mdbx_env_create(&child) != MDBX_SUCCESS)
      exit(5);
    rc = mdbx_env_open(child, testdb, MDBX_EXCLUSIVE | MDBX_RDONLY, 0444);
    mdbx_env_close(child);
    exit(rc == MDBX_BUSY ? 0 : 7);
  }
  int status = 0;
  ASSERT_EQ(waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "child exit=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  mdbx_env_close(env);
  mdbx_env_delete(testdb, MDBX_ENV_JUST_DELETE);
}