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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/329
// After fork() the child inherited each open environment's own process-local
// mutexes, env->dbi_lock and env->remap_lock, in whatever state other threads of
// the parent left them, and the write-lock owner recorded in
// env->basal_txn->owner. If another thread was opening or closing a table handle
// or resizing the database at that moment, the child hung after a successful
// mdbx_env_resurrect_after_fork(), mdbx_env_close() in the child aborted, or it
// failed with MDBX_BUSY because of the stale owner. Reinitializing the mutexes
// in the child is not enough: the table-handle registry or the mapping that
// another thread was changing would stay half-updated. So the library now holds
// both mutexes of each environment over fork(), as it does for its global ones,
// reinitializes them in the child and resets the owner.
//
// Four modes; in each, one parent thread repeats an operation while the main
// thread forks, and each child does one step and leaves with _exit():
//   dbi       parent: open a closed table + close;  child: resurrect, the same
//   remap     parent: mdbx_env_set_geometry();       child: resurrect, the same
//   close     parent: mdbx_env_set_geometry();       child: resurrect, close
//   dbiclose  parent: open a closed table + close;  child: close (no resurrect)
// A child that has not finished within 15 seconds counts as hung and is killed;
// a child must exit with status 0. The defect is timing-dependent, but without
// the fix most modes fail within the first few forks.
//
// fork_while_a_logger_under_remap_lock_joins_a_thread: the fork handler must not
// wait for one of these mutexes while it holds rthc_mutex. A shrink by
// mdbx_env_set_geometry() logs a NOTICE under remap_lock; the logger callback
// joins a thread that used libmdbx, whose exit needs rthc_mutex, while a third
// thread forks. Run in a child process, which counts as deadlocked after 15
// seconds. POSIX only.

namespace {

enum mode { DBI, REMAP, CLOSE, DBICLOSE };
const char *const mode_name[] = {"dbi", "remap", "close", "dbiclose"};

std::string db_name(mode m) { return std::string("issue329-") + mode_name[m] + ".mdbx"; }

/* Deletes the database before the test and on every exit path. */
struct files_guard {
  const std::string path;
  explicit files_guard(const std::string &path) : path(path) { mdbx_env_delete(path.c_str(), MDBX_ENV_JUST_DELETE); }
  ~files_guard() { mdbx_env_delete(path.c_str(), MDBX_ENV_JUST_DELETE); }
};

struct env_guard {
  MDBX_env *env = nullptr;
  ~env_guard() {
    if (env)
      mdbx_env_close(env);
  }
};

int dbi_open_close(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS) {
    MDBX_dbi dbi = 0;
    rc = mdbx_dbi_open(txn, "t", MDBX_DB_DEFAULTS, &dbi);
    mdbx_txn_abort(txn);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_dbi_close(env, dbi);
  }
  return rc;
}

int operation(mode m, MDBX_env *env, unsigned i) {
  if (m == DBI || m == DBICLOSE)
    return dbi_open_close(env);
  /* 4 MiB or 8 MiB, upper bound 64 MiB: a resize under remap_lock */
  return mdbx_env_set_geometry(env, -1, (i & 1) ? 8 << 20 : 4 << 20, 64 << 20, -1, -1, -1);
}

/* The child's step; returns its exit status. */
int child_step(mode m, MDBX_env *env) {
  if (m == DBICLOSE)
    return mdbx_env_close(env) == MDBX_SUCCESS ? 0 : 1;
  if (mdbx_env_resurrect_after_fork(env) != MDBX_SUCCESS)
    return 2;
  const int rc = (m == CLOSE) ? mdbx_env_close(env) : operation(m, env, 1);
  return rc == MDBX_SUCCESS ? 0 : 3;
}

/* Waits up to 15 seconds for the child. Returns true and its wait status when it exits, or kills it and returns
 * false when it hangs. */
bool child_exits(pid_t pid, int &status) {
  for (int i = 0; i < 15000; ++i) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid)
      return true;
    if (got < 0 && errno != EINTR)
      break;
    usleep(1000);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return false;
}

void run_mode(mode m) {
  const std::string path = db_name(m);
  files_guard files(path);
  env_guard db;
  ASSERT_EQ(mdbx_env_create(&db.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(db.env, 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(db.env, -1, 4 << 20, 64 << 20, -1, -1, -1), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(db.env, path.c_str(), MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(db.env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  int rc = mdbx_dbi_open(txn, "t", MDBX_CREATE, &dbi);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
  ASSERT_EQ(mdbx_dbi_close(db.env, dbi), MDBX_SUCCESS);

  std::atomic<bool> stop{false};
  std::atomic<int> churn_rc{MDBX_SUCCESS};
  std::thread churn([&] {
    for (unsigned i = 0; !stop.load(); ++i) {
      const int err = operation(m, db.env, i);
      if (err != MDBX_SUCCESS) {
        churn_rc.store(err);
        break;
      }
      std::this_thread::yield(); /* lets a child's wait for the same lock win now and then */
    }
  });

  int forks = 0, failure_status = 0, fork_errno = 0;
  bool hung = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (; forks < 100 && std::chrono::steady_clock::now() < deadline; ++forks) {
    fflush(nullptr);
    pid_t pid = -1;
    /* a transient EAGAIN is possible on a loaded CI runner: retry a few times */
    for (unsigned tries = 0;; ++tries) {
      pid = fork();
      if (pid >= 0 || errno != EAGAIN || tries >= 4)
        break;
      usleep(1000 << tries);
    }
    fork_errno = (pid < 0) ? errno : 0;
    if (pid < 0)
      break;
    if (pid == 0)
      _exit(child_step(m, db.env));
    int status = 0;
    if (!child_exits(pid, status)) {
      hung = true;
      break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      failure_status = status;
      break;
    }
    usleep(1000);
  }
  stop.store(true);
  churn.join();

  EXPECT_EQ(fork_errno, 0) << mode_name[m] << ": fork " << forks + 1 << " failed: " << strerror(fork_errno);
  EXPECT_FALSE(hung) << mode_name[m] << ": fork " << forks + 1 << ": the child hung";
  EXPECT_EQ(failure_status, 0) << mode_name[m] << ": fork " << forks + 1 << ": the child failed, wait status 0x"
                               << std::hex << failure_status;
  EXPECT_GT(forks, 0) << mode_name[m];
  EXPECT_EQ(churn_rc.load(), MDBX_SUCCESS) << mode_name[m] << ": " << mdbx_strerror(churn_rc.load());
}

} // namespace

TEST(issue_gh0329, fork_while_another_thread_holds_env_locks) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  for (const mode m : {DBI, REMAP, CLOSE, DBICLOSE}) {
    run_mode(m);
    if (HasFailure())
      return;
  }
}

namespace {

const char *const kJoinDb = "issue329-join.mdbx";

/* State of the scenario in the child process, shared with the logger and the atfork handler. */
std::atomic<bool> logger_armed{false}, fork_now{false}, fork_started{false}, reader_may_exit{false};
std::thread *reader_to_join = nullptr;

/* Prepare handlers run in registration order, so libmdbx's handler runs first:
 * this one then marks the fork as started while libmdbx's handler is in its
 * retry loop (its locks are contended in this scenario). */
void note_fork_started() { fork_started.store(true); }

bool wait_for(const std::atomic<bool> &flag) {
  for (int i = 0; i < 5000 && !flag.load(); ++i)
    usleep(1000);
  return flag.load();
}

/* Runs under remap_lock (and debug_lock): lets another thread fork, then joins a thread whose exit
 * (rthc_thread_dtor) needs rthc_mutex. A fork handler that waits for remap_lock while holding rthc_mutex deadlocks
 * with it. */
void joining_logger(MDBX_log_level_t, const char *, int, const char *, va_list) MDBX_CXX17_NOEXCEPT {
  if (!logger_armed.exchange(false))
    return;
  fork_now.store(true);
  /* no overlap with the fork: return without joining, the reader stays joinable and the scenario fails */
  if (!wait_for(fork_started))
    return;
  usleep(100 * 1000); /* the fork is now in libmdbx's prepare handler */
  reader_may_exit.store(true);
  reader_to_join->join();
}

/* The scenario, in a child process without other threads; returns its exit status. */
int join_scenario() {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS ||
      mdbx_env_set_geometry(env, -1, 8 << 20, 64 << 20, -1, -1, -1) != MDBX_SUCCESS ||
      mdbx_env_open(env, kJoinDb, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644) != MDBX_SUCCESS)
    return 10;
  /* a thread that used libmdbx: its exit runs rthc_thread_dtor(), which takes rthc_mutex */
  std::atomic<bool> reader_done{false}, reader_ok{false};
  std::thread reader([&] {
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) == MDBX_SUCCESS)
      reader_ok.store(mdbx_txn_abort(txn) == MDBX_SUCCESS);
    reader_done.store(true);
    while (!reader_may_exit.load())
      usleep(1000);
  });
  if (!wait_for(reader_done) || !reader_ok.load()) {
    reader_may_exit.store(true);
    reader.join();
    return 11;
  }
  reader_to_join = &reader;
  if (pthread_atfork(note_fork_started, nullptr, nullptr) != 0 ||
      mdbx_setup_debug(MDBX_LOG_NOTICE, MDBX_DBG_DONTCHANGE, joining_logger) < 0) {
    reader_may_exit.store(true);
    reader.join();
    return 12;
  }

  std::atomic<int> forked_status{-1};
  std::thread forker([&] {
    if (!wait_for(fork_now))
      return;
    const pid_t pid = fork();
    if (pid == 0)
      _exit(0);
    int status = -1;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    forked_status.store(status);
  });
  logger_armed.store(true);
  /* shrinking 8 MiB to 4 MiB logs "resize-MADV_..." at NOTICE level under remap_lock, from this thread */
  const int shrink_rc = mdbx_env_set_geometry(env, -1, 4 << 20, 64 << 20, -1, -1, -1);
  logger_armed.store(false);
  fork_now.store(true); /* the forker leaves even when the logger did not run */
  forker.join();
  if (reader.joinable()) { /* the logger did not run, or the fork did not start in time */
    reader_may_exit.store(true);
    reader.join();
    return (shrink_rc != MDBX_SUCCESS) ? 15 : 13;
  }
  if (shrink_rc != MDBX_SUCCESS)
    return 15;
  mdbx_env_close(env);
  return (forked_status.load() == 0) ? 0 : 14;
}

} // namespace

TEST(issue_gh0329, fork_while_a_logger_under_remap_lock_joins_a_thread) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  files_guard files(kJoinDb);
  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_GE(pid, 0) << "fork() failed";
  if (pid == 0)
    _exit(join_scenario());
  int status = 0;
  const bool exited = child_exits(pid, status);
  ASSERT_TRUE(exited) << "deadlock: a fork() waited for remap_lock while holding rthc_mutex, which the exit of a "
                         "thread joined by a logger needs";
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "the scenario failed, wait status 0x" << std::hex << status;
}
