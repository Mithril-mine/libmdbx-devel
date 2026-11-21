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

/// The dead-writer recovery matrix of the durability test program (#564,
/// cluster C): a writer dies while holding the write-transaction lock, and
/// the surviving processes must either recover or - for the modes without
/// dead-owner recovery - strand in the documented way (issue #565).
///
/// The white-box include of the internals is required to learn the build's
/// MDBX_LOCKING (the resolved constant, as opposed to the AUTO cmake value).
/// POSIX-only at the fork level, like the crash-windows corpus.

#include "../../src/essentials.h"

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"
#include "../test-env.h++"

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kAttemptWatchdogSeconds = 15;
const char *const kDb = "dead-owner.mdbx";

enum attempt_outcome {
  attempt_succeeded = 0, /* the write txn began (dead-owner recovery or free lock) */
  attempt_stranded,      /* the attempt was still waiting when the watchdog fired */
  attempt_aborted,       /* the attempt died by its own abort (glibc assert on a dead owner) */
  attempt_failed         /* the attempt returned an error */
};

/* The forked writer: begins a write transaction (thus holds the write lock)
 * and kills itself - the lock is left owned by a dead process. */
void run_victim_writer() {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    _exit(101);
  if (mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS)
    _exit(102);
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    _exit(103);
  fprintf(stderr, "dead-owner: the victim holds the write lock, dying\n");
  fflush(stderr);
  raise(SIGKILL);
  _exit(104); /* unreachable */
}

/* The forked attempter: joins the environment and tries to begin a write
 * transaction, with a watchdog so a stranded lock cannot hang the test. */
void run_lock_attempt() {
  alarm(kAttemptWatchdogSeconds);
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    _exit(111);
  if (mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS)
    _exit(112);
  MDBX_txn *txn = nullptr;
  const int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  fprintf(stderr, "dead-owner: the lock attempt returned %d\n", rc);
  fflush(stderr);
  if (rc == MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    _exit(0); /* attempt_succeeded */
  }
  _exit(113); /* attempt_failed, the code is not transported */
}

attempt_outcome run_attempt() {
  const pid_t pid = fork();
  if (pid < 0)
    return attempt_failed;
  if (pid == 0)
    run_lock_attempt(); /* never returns */
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return attempt_failed;
  if (WIFEXITED(status)) {
    switch (WEXITSTATUS(status)) {
    case 0:
      return attempt_succeeded;
    case 111:
    case 112:
      return attempt_failed;
    default:
      return attempt_failed;
    }
  }
  if (WIFSIGNALED(status)) {
    if (WTERMSIG(status) == SIGALRM)
      return attempt_stranded;
    if (WTERMSIG(status) == SIGABRT)
      return attempt_aborted;
  }
  return attempt_failed;
}

} // namespace

TEST(dead_owner, fresh_opener_recovers_after_writer_death) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  /* A fresh opener always recovers: the exclusive re-seize resets the lck
   * region once no other process holds it (the coarse repair path). */
  const pid_t victim = fork();
  ASSERT_GE(victim, 0);
  if (victim == 0)
    run_victim_writer(); /* never returns */
  int status = 0;
  ASSERT_EQ(waitpid(victim, &status, 0), victim);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "the victim must die by SIGKILL";

  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS) << "the fresh opener must recover";
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS)
      << "the fresh opener must acquire the write lock";
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}

TEST(dead_owner, surviving_opener_after_writer_death) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  /* The sibling's environment is open BEFORE the death, so the sibling hits
   * the dead-owned lock directly (no re-seize). */
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  const pid_t victim = fork();
  ASSERT_GE(victim, 0);
  if (victim == 0)
    run_victim_writer(); /* never returns; opens its own env on the same db */
  int status = 0;
  ASSERT_EQ(waitpid(victim, &status, 0), victim);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) << "the victim must die by SIGKILL";

  const attempt_outcome outcome = run_attempt();
#if MDBX_LOCKING == MDBX_LOCKING_POSIX2001
  /* The documented #565 limitation: plain (non-robust) process-shared
   * mutexes have no dead-owner recovery - the surviving processes strand:
   * the attempt either hangs inside the C-library until the watchdog, or is
   * aborted by the C-library's own assert on the dead PI-mutex owner.
   * CHARACTERIZATION ROW: flip this expectation to attempt_succeeded when
   * the dead-owner repair protocol lands. */
  EXPECT_TRUE(outcome == attempt_stranded || outcome == attempt_aborted)
      << "POSIX-2001 must strand (see #565), got outcome " << outcome;
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  /* The POSIX-1988 named semaphores cannot detect a dead owner either
   * (the Solaris/Haiku legacy mode): the attempt strands. */
  EXPECT_TRUE(outcome == attempt_stranded || outcome == attempt_aborted)
      << "POSIX-1988 must strand, got outcome " << outcome;
#else
  /* Robust mutexes (POSIX-2008) and the System V semaphores (SEM_UNDO)
   * provide the dead-owner recovery: the survivor acquires the lock. */
  EXPECT_EQ(outcome, attempt_succeeded) << "the surviving opener must recover from a dead writer, got outcome "
                                        << outcome;
#endif

  mdbx_env_close(env);
}
