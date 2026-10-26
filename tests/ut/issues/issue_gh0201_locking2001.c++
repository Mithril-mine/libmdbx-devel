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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "../../../src/internals.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <pthread.h>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// LOCKING=2001 (POSIX-1 process-shared NON-robust mutex) regression coverage.
//
// The 2001 scheme protects the reader table registration (rdt_lock) and the
// write-transaction serialization (wrt_lock) with process-shared pthread
// mutexes that are NOT robust (unlike the POSIX-2008 scheme). When a process
// dies while holding such a mutex, the mutex stays permanently locked: a later
// trylock returns EBUSY forever (and a blocking lock hangs), so the whole
// database becomes unusable — new writers/readers fail in seemingly random
// places of the stochastic multi-process swarm. This file:
//
//   B1/B2/B3 — live-process coordination: writers are serialized by wrt_lock,
//              readers are not blocked by rdt_lock, and a small multi-process
//              swarm cooperates cleanly. These must pass on every POSIX scheme.
//   C1       — owner death: a process is SIGKILLed while holding a write (or
//              read-registration) lock; a subsequent MDBX_TXN_TRY open must
//              NOT be stuck busy. This is where non-robust 2001 fails (EBUSY
//              forever) while robust 2008 recovers via EOWNERDEAD.
//   C2       — diagnostic: scan the opaque pthread_mutex_t bytes with memcmp()
//              for the killed owner's pid. Finding it proves the mutex still
//              belongs to the dead process; NOT finding it (no owner-like slot
//              on this platform) is also information, not a failure.
//
// Portability: fork()+pipe()+waitpid() are POSIX-only, so the file is only
// registered on !WIN32. The C scenarios assert the *recovery* behavior: on 2001
// they are expected to fail until the scheme learns to recover the owner; on
// 2008 they assert the EOWNERDEAD path keeps working.

namespace {

const char *const kTestPath = "issue_gh0201_locking2001.mdbx";

static void cleanup(const char *path) {
  std::string lck = std::string(path) + MDBX_LOCK_SUFFIX;
  std::remove(path);
  std::remove(lck.c_str());
}

static MDBX_env *open_env(const char *path) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  if (mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

static MDBX_dbi open_main_dbi(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return 0;
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS || mdbx_txn_commit(txn) != MDBX_SUCCESS)
    return 0;
  return dbi;
}

/* Ожидание SIGSTOP-ребёнка или сигнала «held» по pipe. Возвращает true, если
 * ребёнок готов, false по таймауту. */
static bool wait_ready(const int fd, const unsigned timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char c = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    ssize_t n = read(fd, &c, 1);
    if (n == 1)
      return true;
    if (n < 0 && errno != EINTR)
      return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// B1/B2/B3: live-process coordination helpers
// ---------------------------------------------------------------------------

/* Ребёнок: открывает env, берёт WR-txn и держит её, пока отец не скомандует
 * release через второй pipe. Сообщает «held» через первый pipe. */
static void child_hold_writelock(const char *path, const int held_fd, const int release_fd) {
  MDBX_env *env = open_env(path);
  if (!env)
    _exit(2);
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    _exit(3);
  const char held = 'H';
  (void)write(held_fd, &held, 1);
  close(held_fd);
  char release = 0;
  while (read(release_fd, &release, 1) != 1)
    /* wait */;
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  _exit(0);
}

/* Ребёнок: держит RO-txn (регистрация в таблице читателей), после release —
 * завершается. */
static void child_hold_readlock(const char *path, const int held_fd, const int release_fd) {
  MDBX_env *env = open_env(path);
  if (!env)
    _exit(2);
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    _exit(3);
  const char held = 'H';
  (void)write(held_fd, &held, 1);
  close(held_fd);
  char release = 0;
  while (read(release_fd, &release, 1) != 1)
    /* wait */;
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  _exit(0);
}

/* Ребёнок-«рой»: пишет/читает N операций, завершается успехом. */
static void child_swarm(const char *path, const bool writer, const unsigned nops) {
  MDBX_env *env = open_env(path);
  if (!env)
    _exit(2);
  MDBX_dbi dbi = open_main_dbi(env);
  if (!dbi)
    _exit(4);
  char kbuf[32];
  for (unsigned i = 0; i < nops; ++i) {
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(env, nullptr, writer ? MDBX_TXN_READWRITE : MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
      _exit(5);
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%06u", i % 1000);
    MDBX_val k{kbuf, size_t(kn)}, v{(void *)"v", 1};
    if (writer) {
      if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS)
        _exit(6);
    } else {
      MDBX_val got;
      int rc = mdbx_get(txn, dbi, &k, &got);
      if (rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND)
        _exit(7);
    }
    if (writer) {
      if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
        _exit(8);
    } else {
      mdbx_txn_abort(txn);
    }
  }
  mdbx_env_close(env);
  _exit(0);
}

// ---------------------------------------------------------------------------
// A/C2: diagnostic probe of the opaque pthread_mutex_t
// ---------------------------------------------------------------------------

enum class owner_probe { zero, expected_pid, other_pid, no_owner_slot };

/* Сканирует opaque байты pthread_mutex_t в поисках слота, memcmp-равного pid
 * (по 4-байтовому значению; по 8-байтовому при sizeof(pid)==8). Возвращает:
 *  - expected_pid если найден слот равный ожидаемому pid;
 *  - other_pid  если найден некий другой ненулевой pid (чужой владелец);
 *  - zero       если все пригодные слоты нулевые (mutex свободен/недержан);
 *  - no_owner_slot если поле-владелец не найдено (платформозависимая
 *    диагностика; это тоже информация). */
static owner_probe probe_opaque_owner(const void *bytes, const size_t size, const mdbx_pid_t expected_pid) {
  bool saw_nonzero = false;
  const union {
    mdbx_pid_t pid;
    unsigned char raw[sizeof(mdbx_pid_t)];
  } ep = {expected_pid};

  for (size_t off = 0; off + sizeof(mdbx_pid_t) <= size; off += sizeof(mdbx_pid_t)) {
    const unsigned char *const slot = (const unsigned char *)bytes + off;
    if (!memcmp(slot, ep.raw, sizeof(ep.raw)))
      return owner_probe::expected_pid;
    /* Ненулевой ли весь слот. */
    bool all_zero = true;
    for (size_t i = 0; i < sizeof(ep.raw); ++i)
      if (slot[i]) {
        all_zero = false;
        break;
      }
    if (!all_zero)
      saw_nonzero = true;
  }
  if (saw_nonzero)
    return owner_probe::other_pid;
  return owner_probe::zero; /* все глянутые слоты нулевые */
}

} // namespace

TEST(issue_gh0201_locking2001, B1_wrt_serializes_writers) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kTestPath);
  MDBX_env *env = open_env(kTestPath);
  ASSERT_NE(nullptr, env);
  const MDBX_dbi dbi = open_main_dbi(env);
  ASSERT_NE(0u, dbi);

  int held_fd[2], release_fd[2];
  ASSERT_EQ(0, pipe(held_fd));
  ASSERT_EQ(0, pipe(release_fd));
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    close(held_fd[0]);
    close(release_fd[1]);
    child_hold_writelock(kTestPath, held_fd[1], release_fd[0]);
  }
  close(held_fd[1]);
  close(release_fd[0]);

  ASSERT_TRUE(wait_ready(held_fd[0], 10000)) << "child must take the write lock in time";
  close(held_fd[0]);

  /* Пока ребёнок держит write-txn, попытка начать ещё одну WR-txn должна
   * дать MDBX_BUSY (wrt_lock сериализует писателей). */
  MDBX_txn *txn = nullptr;
  EXPECT_EQ(MDBX_BUSY, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE | MDBX_TXN_TRY, &txn));
  EXPECT_EQ(nullptr, txn);

  /* Освободить и убедиться, что писатель снова может начать txn. */
  const char release = 'R';
  ASSERT_EQ(1, (int)write(release_fd[1], &release, 1));
  close(release_fd[1]);
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child must exit cleanly";

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  mdbx_env_close(env);
  cleanup(kTestPath);
}

TEST(issue_gh0201_locking2001, B2_rdt_does_not_block_readers) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kTestPath);
  MDBX_env *env = open_env(kTestPath);
  ASSERT_NE(nullptr, env);
  const MDBX_dbi dbi = open_main_dbi(env);
  ASSERT_NE(0u, dbi);

  int held_fd[2], release_fd[2];
  ASSERT_EQ(0, pipe(held_fd));
  ASSERT_EQ(0, pipe(release_fd));
  const pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    close(held_fd[0]);
    close(release_fd[1]);
    child_hold_readlock(kTestPath, held_fd[1], release_fd[0]);
  }
  close(held_fd[1]);
  close(release_fd[0]);

  ASSERT_TRUE(wait_ready(held_fd[0], 10000)) << "child must register as reader in time";
  close(held_fd[0]);

  /* Пока ребёнок — активный читатель, ещё один читатель не должен
   * блокироваться rdt_lock: read-only begin не берёт wrt_lock вовсе, и
   * должен пройти сразу (без TRY — флаг TRY применим только к писателям). */
  MDBX_txn *ro = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &ro));
  if (ro) {
    EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(ro));
  }

  /* И писатель на фоне читателя не должен падать в MDBX_BUSY из-за rdt. */
  MDBX_txn *wr = nullptr;
  const int wr_rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE | MDBX_TXN_TRY, &wr);
  EXPECT_TRUE(wr_rc == MDBX_SUCCESS || wr_rc == MDBX_BUSY)
      << "writer must either succeed (mvcc) or be busy due to the reader, but not fail: " << wr_rc;
  if (wr) {
    EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(wr));
  }

  const char release = 'R';
  ASSERT_EQ(1, (int)write(release_fd[1], &release, 1));
  close(release_fd[1]);
  int status = 0;
  ASSERT_EQ(pid, waitpid(pid, &status, 0));
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child must exit cleanly";
  mdbx_env_close(env);
  cleanup(kTestPath);
}

TEST(issue_gh0201_locking2001, B3_process_swarm) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kTestPath);
  MDBX_env *env = open_env(kTestPath);
  ASSERT_NE(nullptr, env);
  const MDBX_dbi dbi = open_main_dbi(env);
  ASSERT_NE(0u, dbi);
  mdbx_env_close(env);
  env = nullptr;

  const int nprocs = 4; /* 2 writers + 2 readers */
  pid_t pids[nprocs];
  for (int i = 0; i < nprocs; ++i) {
    pids[i] = fork();
    ASSERT_GE(pids[i], 0);
    if (pids[i] == 0)
      child_swarm(kTestPath, (i % 2) == 0 /* writer */, 2000);
  }
  for (int i = 0; i < nprocs; ++i) {
    int status = 0;
    ASSERT_EQ(pids[i], waitpid(pids[i], &status, 0));
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "swarm child #" << i << " must exit cleanly, got status " << status;
  }
  cleanup(kTestPath);
}

#if MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX2008
/* Обна-механика для C1: ребёнок держит WR-txn (или регистрацию читателя),
 * родитель убивает его, затем проверяет MDBX_TXN_TRY recovery.
 * Возвращает: rc самого recovery (MDBX_SUCCESS при успехе), или
 * MDBX_EBADSIGN(-30785)-подобную инфраструктурную ошибку <0, если установка
 * провалилась (pipe/fork/готовность). */
static int kill_owner_and_trylock(const char *path, const bool writer) {
  MDBX_env *env = open_env(path);
  if (!env)
    return -2;
  const MDBX_dbi dbi = open_main_dbi(env);
  if (!dbi) {
    mdbx_env_close(env);
    return -3;
  }

  int held_fd[2];
  if (pipe(held_fd) != 0) {
    mdbx_env_close(env);
    return -4;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(held_fd[0]);
    close(held_fd[1]);
    mdbx_env_close(env);
    return -5;
  }
  if (pid == 0) {
    close(held_fd[0]);
    MDBX_env *ce = open_env(path);
    if (!ce)
      _exit(2);
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(ce, nullptr, writer ? MDBX_TXN_READWRITE : MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
      _exit(3);
    const char held = 'H';
    (void)write(held_fd[1], &held, 1);
    close(held_fd[1]);
    /* Держим блокировку вечно: остановиться и ждать SIGKILL от родителя. */
    for (;;)
      pause();
  }
  close(held_fd[1]);
  if (!wait_ready(held_fd[0], 10000)) {
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    close(held_fd[0]);
    mdbx_env_close(env);
    return -6;
  }
  close(held_fd[0]);

  /* Зафиксировать "печать" владельца до kill — для C2 диагностики. */
  const mdbx_pid_t killed_pid = (mdbx_pid_t)pid;
  owner_probe probe_before = owner_probe::no_owner_slot;
  if (env->lck_mmap.lck) {
    const void *mutex_bytes = (const void *)&env->lck_mmap.lck->wrt_lock;
    probe_before = probe_opaque_owner(mutex_bytes, sizeof(env->lck_mmap.lck->wrt_lock), killed_pid);
  }

  /* SIGKILL владельца. */
  kill(pid, SIGKILL);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    mdbx_env_close(env);
    return -7;
  }

  /* После гибели владельца попытка получить WR-lock не должна застревать в
   * BUSY: либо сразу проходит (recovery сработал), либо — на non-robust 2001
   * без фикса — навсегда EBUSY, что мы и фиксируем здесь как дефект. */
  int rc = -1;
  for (int attempt = 0; attempt < 40; ++attempt) {
    MDBX_txn *txn = nullptr;
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE | MDBX_TXN_TRY, &txn);
    if (rc == MDBX_SUCCESS) {
      EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
      break;
    }
    if (rc != MDBX_BUSY)
      break;
    /* Без фикса на 2001 здесь будет вечный BUSY; короткая пауза и retry. */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  /* C2 диагностика после восстановления/попытки. */
  owner_probe probe_after = owner_probe::no_owner_slot;
  if (env->lck_mmap.lck) {
    const void *mutex_bytes = (const void *)&env->lck_mmap.lck->wrt_lock;
    probe_after = probe_opaque_owner(mutex_bytes, sizeof(env->lck_mmap.lck->wrt_lock), killed_pid);
  }
  SCOPED_TRACE("C2 probe: before=" + std::to_string((int)probe_before) + " after=" + std::to_string((int)probe_after) +
               " killed_pid=" + std::to_string(killed_pid));
  /* Диагностика всегда печатается в stderr (не только gtest-trace) — это
   * ключевая информация при разборе 2001. */
  std::fprintf(stderr, "issue_gh0201 C2 probe: before=%d after=%d killed_pid=%u\n", (int)probe_before, (int)probe_after,
               (unsigned)killed_pid);

  mdbx_env_close(env);
  return rc;
}

TEST(issue_gh0201_locking2001, C1_kill_owner_wrt_recovered) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kTestPath);
  const int rc = kill_owner_and_trylock(kTestPath, /* writer */ true);
  EXPECT_EQ(MDBX_SUCCESS, rc) << "a killed writer must not leave the DB permanently write-locked; got " << rc;
  cleanup(kTestPath);
}

TEST(issue_gh0201_locking2001, C1_kill_owner_rdt_recovered) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kTestPath);
  const int rc = kill_owner_and_trylock(kTestPath, /* reader-registration */ false);
  EXPECT_EQ(MDBX_SUCCESS, rc) << "a killed reader must not leave the reader-table lock permanently busy; got " << rc;
  cleanup(kTestPath);
}
#else
TEST(issue_gh0201_locking2001, C_recovery_is_scheme_specific) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* SYSV / POSIX1988: semaphores survive owner death differently; the kill-recovery
   * scenarios above are scheme-specific (2001/2008). */
  GTEST_SKIP() << "C1 kill-owner recovery is POSIX2001/2008-specific; skipping on this Locking scheme";
}
#endif /* MDBX_LOCKING */
