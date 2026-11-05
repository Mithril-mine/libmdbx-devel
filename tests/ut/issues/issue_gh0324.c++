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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/324
// libmdbx registered only a child handler for fork(). When another thread held
// rthc_mutex (inside mdbx_env_open()/mdbx_env_close()) or globals.debug_lock
// (any log message) at the moment of fork(), the child inherited the mutex
// locked by a thread that does not exist there, and blocked forever in its next
// mdbx_env_open(), mdbx_env_close() or exit(). The fix holds both mutexes over
// fork() and reinitializes them in the child.
//
// One thread opens and closes an environment in a loop while the main thread
// forks; each child opens and closes its own environment and exits normally
// (exit() runs the library destructor). A child that has not finished within
// 15 seconds counts as hung and is killed; a child must also exit with status
// 0. The defect is timing-dependent, but without the fix most children hang.

namespace {

const char *const kParentDb = "issue324-parent.mdbx";

std::string child_db(pid_t pid) { return "issue324-child-" + std::to_string(pid) + ".mdbx"; }

enum step { before_open, in_open, in_close, in_delete, in_exit };
const char *const step_name[] = {"before mdbx_env_open()", "in mdbx_env_open()", "in mdbx_env_close()",
                                 "in mdbx_env_delete()", "in exit()"};

int open_close(const char *path, volatile int *where) {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (where)
    *where = in_open;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644);
  if (where)
    *where = in_close;
  if (env) {
    const int err = mdbx_env_close(env);
    if (rc == MDBX_SUCCESS)
      rc = err;
  }
  return rc;
}

/* Stops the churn thread and deletes the files on every exit path. */
struct churn_guard {
  std::atomic<bool> stop{false};
  std::atomic<int> rc{MDBX_SUCCESS};
  std::thread thread;
  volatile int *where = nullptr;

  churn_guard() {
    mdbx_env_delete(kParentDb, MDBX_ENV_JUST_DELETE);
    void *const shared = mmap(nullptr, sizeof(int), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    where = (shared == MAP_FAILED) ? nullptr : static_cast<volatile int *>(shared);
    thread = std::thread([this] {
      while (!stop.load()) {
        const int err = open_close(kParentDb, nullptr);
        if (err != MDBX_SUCCESS) {
          rc.store(err);
          break;
        }
      }
    });
  }
  ~churn_guard() {
    stop.store(true);
    if (thread.joinable())
      thread.join();
    if (where)
      munmap(const_cast<int *>(where), sizeof(int));
    mdbx_env_delete(kParentDb, MDBX_ENV_JUST_DELETE);
  }
};

/* Waits up to 15 seconds for the child. Returns true and its wait status when
 * it exits, or kills it and returns false when it hangs. */
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

} // namespace

TEST(issue_gh0324, fork_while_another_thread_opens_and_closes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  churn_guard churn;
  ASSERT_NE(churn.where, nullptr) << "mmap() failed";

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  int forks = 0;
  for (; forks < 50 && std::chrono::steady_clock::now() < deadline; ++forks) {
    *churn.where = before_open;
    fflush(nullptr);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork() failed";
    if (pid == 0) {
      const std::string db = child_db(getpid());
      const int rc = open_close(db.c_str(), churn.where);
      *churn.where = in_delete;
      mdbx_env_delete(db.c_str(), MDBX_ENV_JUST_DELETE);
      *churn.where = in_exit;
      exit(rc == MDBX_SUCCESS ? 0 : 1); /* a normal exit runs the library destructor */
    }
    int status = 0;
    const bool exited = child_exits(pid, status);
    mdbx_env_delete(child_db(pid).c_str(), MDBX_ENV_JUST_DELETE);
    ASSERT_TRUE(exited) << "fork " << forks + 1 << ": the child hung " << step_name[*churn.where];
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "fork " << forks + 1 << ": the child failed, wait status 0x" << std::hex << status;
    usleep(1000);
  }
  EXPECT_GT(forks, 0);
  EXPECT_EQ(churn.rc.load(), MDBX_SUCCESS) << "the open/close thread failed";
}
