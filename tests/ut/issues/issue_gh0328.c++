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

#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/328
// The locking auto-selection picked POSIX shared semaphores (MDBX_LOCKING_POSIX1988)
// on FreeBSD and the other platforms without usable process-shared mutexes instead
// of System V IPC. POSIX1988 cannot detect a dead lock owner: when a process was
// killed while it held the write lock, the writers of every other process that kept
// the database open waited forever (MDBX_TXN_TRY returned MDBX_BUSY forever). The fix
// restores System V IPC there, which recovers at once.
//
// A child process opens the database, takes the write lock and is killed with
// SIGKILL, while this process keeps the database open. This process must then get
// the write lock within 5 seconds and commit. The test runs where the locking can
// recover a dead owner (POSIX2008 robust mutexes, System V IPC), and where the
// auto-selection picked POSIX1988 outside Solaris/SVR4 and Haiku, i.e. the defect
// itself. It skips for POSIX2001 and for POSIX1988 chosen on purpose (an explicit
// build option, or the Solaris/SVR4 and Haiku defaults): neither can recover a dead
// owner by design.

namespace {

const char *const kDb = "issue328.mdbx";

/* The MDBX_LOCKING value from the build options: "AUTO=2008", "5", ... */
std::string locking_config() {
  const std::string options = mdbx_build.options ? mdbx_build.options : "";
  const auto at = options.find("LOCKING=");
  if (at == std::string::npos)
    return std::string();
  return options.substr(at + 8, options.find(' ', at) - at - 8);
}

bool posix1988_by_design() {
  struct utsname name;
  return uname(&name) == 0 && (strcmp(name.sysname, "SunOS") == 0 || strcmp(name.sysname, "Haiku") == 0);
}

/* Deletes the database files before the test and on every exit path. */
struct db_guard {
  db_guard() { mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE); }
  ~db_guard() { mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};
using env_ptr = std::unique_ptr<MDBX_env, env_closer>;

int open_env(env_ptr &env) {
  MDBX_env *raw = nullptr;
  int rc = mdbx_env_create(&raw);
  if (rc != MDBX_SUCCESS)
    return rc;
  env.reset(raw);
  return mdbx_env_open(raw, kDb, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644);
}

/* The child: opens the database, takes the write lock, reports and waits to be killed. */
[[noreturn]] void holder(int report_fd) {
  env_ptr env;
  int rc = open_env(env);
  MDBX_txn *txn = nullptr;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn);
  (void)!write(report_fd, &rc, sizeof(rc));
  if (rc != MDBX_SUCCESS)
    _exit(1);
  for (;;)
    pause();
}

/* Kills and reaps the child on every exit path. */
struct child_guard {
  pid_t pid = -1;
  int report_fd = -1;
  child_guard() {
    int fds[2];
    if (pipe(fds) != 0)
      return;
    pid = fork();
    if (pid == 0) {
      close(fds[0]);
      holder(fds[1]);
    }
    close(fds[1]);
    if (pid > 0)
      report_fd = fds[0];
    else
      close(fds[0]);
  }
  /* Kills the child; returns true when it died by SIGKILL. */
  bool kill_child() {
    if (pid <= 0)
      return false;
    kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    pid = -1;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
  }
  ~child_guard() {
    kill_child();
    if (report_fd >= 0)
      close(report_fd);
  }
};

} // namespace

TEST(issue_gh0328, killed_writer_does_not_block_other_processes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  const std::string locking = locking_config();
  const bool recovers = locking == "AUTO=2008" || locking == "2008" || locking == "AUTO=5" || locking == "5";
  const bool auto_posix1988 = locking == "AUTO=1988" && !posix1988_by_design();
  if (!recovers && !auto_posix1988) {
    GTEST_SKIP() << "LOCKING=" << locking << " cannot recover a dead lock owner by design";
  }

  db_guard files;
  env_ptr env; /* closed before the files are deleted */
  child_guard child;
  ASSERT_GT(child.pid, 0) << "fork() failed";
  ASSERT_GE(child.report_fd, 0) << "pipe() failed";

  /* this process keeps the database open while the child holds and loses the write lock */
  ASSERT_EQ(open_env(env), MDBX_SUCCESS);
  struct pollfd report = {child.report_fd, POLLIN, 0};
  ASSERT_EQ(poll(&report, 1, 10000), 1) << "the child did not report within 10 s";
  int child_rc = -1;
  ASSERT_EQ(read(child.report_fd, &child_rc, sizeof(child_rc)), static_cast<ssize_t>(sizeof(child_rc)));
  ASSERT_EQ(child_rc, MDBX_SUCCESS) << "the child could not take the write lock";
  ASSERT_TRUE(child.kill_child()) << "the child did not die by SIGKILL";

  int rc = MDBX_BUSY;
  MDBX_txn *txn = nullptr;
  for (int i = 0; i < 50 && rc == MDBX_BUSY; ++i) {
    rc = mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE | MDBX_TXN_TRY, &txn);
    if (rc == MDBX_BUSY)
      usleep(100000);
  }
  ASSERT_EQ(rc, MDBX_SUCCESS) << "the write lock of the killed process was not recovered within 5 s, locking "
                              << locking << ": " << mdbx_strerror(rc);

  MDBX_dbi dbi = 0;
  rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  if (rc == MDBX_SUCCESS) {
    MDBX_val key = {const_cast<char *>("key"), 3}, data = {const_cast<char *>("value"), 5};
    rc = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT);
  }
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  EXPECT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
}
