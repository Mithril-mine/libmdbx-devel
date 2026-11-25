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
/// software or the use or dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"
#include "../test-env.h++"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* The exit-code contract of mdbx_chk under a break signal (the
 * chk-interrupt-exit-code fix, the suite test of
 * https://github.com/Mithril-mine/libmdbx-devel/issues/553): a SIGINT delivered
 * while mdbx_env_chk() traverses the database must make the tool exit with
 * EXIT_INTERRUPTED (5, i.e. EXIT_FAILURE + 4 of src/tools/chk.c). Before the
 * fix an interruption surfaced as the negative MDBX_EINTR, which fell through
 * the rc-mapping to EXIT_FAILURE_MDBX (3), and EXIT_INTERRUPTED (5) was
 * reachable only through the usage/argument errors (chk.c reports those with
 * the same code) - so the test pairs the interrupted run with the clean run
 * of the very same database: exit 0 and the "No error is detected" verdict
 * there, no interruption mark. The "interrupted by signal" resolution line
 * in the interrupted run's output pins the signal path itself.
 *
 * Mechanics: the test fork+execs the mdbx_chk binary (the path comes via the
 * MDBX_TOOL_CHK environment variable, the tool binary next to this test
 * executable as the fallback) with `-vvv` on a bulk database, reads the
 * child's combined output until the "Traversal B-Trees" line appears - the
 * tool flushes the traversal scope line right before the chk_tree() stage
 * begins - and delivers SIGINT at that point. The verbosity matters: inside
 * the tree scope (adjustment -1) the "interrupted by signal" resolution line
 * passes the silence-cutoff only from verbosity 3 on. The bulk database
 * (16'000'000 records, about 300MB on 4KiB pages) is sized for a
 * multi-second traversal on the slowest CI configurations: the walk is
 * record-count-bound (~60ns per record), so the wall time holds across
 * page sizes (tests/AGENTS.md, "Cross-platform correctness").
 *
 * UNIX-only (fork/execv/SIGINT): registered under
 * `if(UNIX AND MDBX_BUILD_TOOLS)` in tests/CMakeLists.txt. */

namespace {

const char *const kDbName = "chk-exit.mdbx";
constexpr unsigned long kBulkRecords = 16000000;
constexpr char kTraversalMarker[] = "Traversal B-Trees";
constexpr char kInterruptedMark[] = "interrupted by signal";
constexpr char kNoErrorMark[] = "No error is detected";
/* EXIT_INTERRUPTED of src/tools/chk.c: EXIT_FAILURE + 4, i.e. 5 as long as
 * the C runtime defines EXIT_FAILURE as 1. */
constexpr int kExitInterrupted = 5;

/* The 10..300-seconds native deadlines scale through the shared
 * mdbx_tst_scaled_wait_seconds() (tests/ut/test-env.h++, provided to every gtest
 * translation unit through the shared PCH), where the child's bulk-database
 * open/walk takes tens of seconds under Valgrind. */
double monotonic_seconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

/* Full path to the mdbx_chk binary: the MDBX_TOOL_CHK environment variable
 * (set by the registration in tests/CMakeLists.txt), otherwise the tool
 * binary next to this test executable (the manual-run layout). */
std::string chk_tool_path() {
  if (const char *from_env = std::getenv("MDBX_TOOL_CHK"); from_env && *from_env)
    return from_env;
  std::vector<char> buf(32768);
  const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
  if (n > 0 && size_t(n) < buf.size()) {
    std::string exe(buf.data(), size_t(n));
    const size_t slash = exe.find_last_of('/');
    if (slash != std::string::npos)
      return exe.substr(0, slash + 1) + "mdbx_chk";
  }
  return "mdbx_chk";
}

uint64_t to_be64(uint64_t v) {
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i)
    result = (result << 8) | ((v >> (i * 8)) & UINT64_C(0xff));
  return result;
}

/* Creates the bulk database: a single write transaction of kBulkRecords
 * sequentially appended records (big-endian counters as the keys, empty
 * values), so the creation is fast and the resulting tree is a wide fan.
 * Fills ASSERT failures on every error and leaves the cleanup to the
 * test's env_files_guard. */
void make_bulk_db() {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  env_handle_guard env_guard{&env};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbName, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  txn_handle_guard txn_guard{&txn};
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  uint64_t key_be = 0;
  for (unsigned long i = 0; i < kBulkRecords; ++i) {
    uint64_t be = to_be64(++key_be);
    MDBX_val key = {&be, sizeof(be)};
    MDBX_val value = {nullptr, 0};
    const int rc = mdbx_put(txn, dbi, &key, &value, MDBX_APPEND);
    if (rc != MDBX_SUCCESS) {
      ADD_FAILURE() << "mdbx_put() #" << i << " failed with " << rc;
      return;
    }
  }
  txn_guard.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  env_guard.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
}

/* Reads the child's combined output until `marker` appears or the deadline
 * passes. Returns true when the marker was seen (the collected output keeps
 * growing), false on EOF/timeout/error. */
bool wait_output_marker(int fd, const char *marker, double deadline_seconds, std::string &collected) {
  const double deadline = monotonic_seconds() + deadline_seconds;
  for (;;) {
    if (collected.find(marker) != std::string::npos)
      return true;
    double left = deadline - monotonic_seconds();
    if (left <= 0)
      return false;
    struct pollfd pfd = {(decltype(pollfd::fd))fd, POLLIN, 0};
    const int ready = poll(&pfd, 1, int(left * 1000));
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (ready == 0)
      continue;
    char chunk[4096];
    const ssize_t got = read(fd, chunk, sizeof(chunk));
    if (got < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (got == 0)
      return collected.find(marker) != std::string::npos;
    collected.append(chunk, size_t(got));
  }
}

/* Drains the rest of the child's output until EOF or the deadline, so the
 * child can never block on a full pipe. Returns true on EOF. */
bool drain_output(int fd, double deadline_seconds, std::string &collected) {
  const double deadline = monotonic_seconds() + deadline_seconds;
  for (;;) {
    double left = deadline - monotonic_seconds();
    if (left <= 0)
      return false;
    struct pollfd pfd = {(decltype(pollfd::fd))fd, POLLIN, 0};
    const int ready = poll(&pfd, 1, int(left * 1000));
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (ready == 0)
      continue;
    char chunk[4096];
    const ssize_t got = read(fd, chunk, sizeof(chunk));
    if (got < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (got == 0)
      return true;
    collected.append(chunk, size_t(got));
  }
}

/* EINTR-safe bounded waitpid, the issue_gh0328 pattern with a deadline. */
bool reap_child(pid_t pid, int &status, double deadline_seconds) {
  const double deadline = monotonic_seconds() + deadline_seconds;
  for (;;) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid)
      return true;
    if (got < 0 && errno != EINTR)
      return false;
    if (monotonic_seconds() >= deadline)
      return false;
    usleep(2000);
  }
}

struct chk_run {
  bool exited = false;
  int exit_code = -1;
  int termsig = 0;
  bool marker_seen = false;
  double seconds = 0;
  std::string output;
};

/* Runs `mdbx_chk -vvv kDbName` as a child; when `interrupt` is set, delivers
 * SIGINT as soon as the traversal begins (the marker line is flushed right
 * before chk_tree()). Collects the combined output, reaps the child on every
 * path (SIGKILL fallback on the deadlines). */
chk_run run_mdbx_chk(const std::string &tool, bool interrupt, double marker_deadline, double exit_deadline) {
  chk_run run;
  int fds[2];
  if (pipe(fds) != 0) {
    ADD_FAILURE() << "pipe() failed";
    return run;
  }
  std::fflush(nullptr);
  const double began = monotonic_seconds();
  const pid_t pid = fork();
  if (pid < 0) {
    ADD_FAILURE() << "fork() failed";
    close(fds[0]);
    close(fds[1]);
    return run;
  }
  if (pid == 0) {
    /* the child: stdout+stderr into the pipe, then the tool replaces the
     * image - no inherited library state to clean up (tests/AGENTS.md,
     * "Root-cause fixes over suppressions") */
    close(fds[0]);
    if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0)
      _exit(126);
    close(fds[1]);
    execl(tool.c_str(), tool.c_str(), "-vvv", kDbName, (char *)nullptr);
    _exit(127);
  }
  close(fds[1]);

  run.marker_seen = wait_output_marker(fds[0], kTraversalMarker, marker_deadline, run.output);
  if (interrupt && run.marker_seen)
    kill(pid, SIGINT);
  drain_output(fds[0], exit_deadline, run.output);
  close(fds[0]);

  int status = 0;
  if (reap_child(pid, status, exit_deadline)) {
    run.exited = WIFEXITED(status);
    run.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    run.termsig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  } else {
    /* the runaway child must not burn the test's timeout budget */
    ADD_FAILURE() << "mdbx_chk did not exit in time";
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  run.seconds = monotonic_seconds() - began;
  return run;
}

} // namespace

TEST(chk_exit, interrupted_and_clean_runs) {
  const env_files_guard guard({kDbName});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }

  const std::string tool = chk_tool_path();
  if (access(tool.c_str(), X_OK) != 0)
    GTEST_SKIP() << "the mdbx_chk binary is not available (`" << tool << "`)";

  const double began = monotonic_seconds();
  make_bulk_db();
  std::fprintf(stdout, "chk-exit: bulk db created in %.2fs\n", monotonic_seconds() - began);
  std::fflush(stdout);

  /* SIGINT while mdbx_env_chk() traverses: the tool must exit normally with
   * EXIT_INTERRUPTED (5), not the pre-fix EXIT_FAILURE_MDBX (3) and not a
   * signal death, with the "interrupted by signal" verdict in the output. */
  chk_run interrupted = run_mdbx_chk(tool, true, mdbx_tst_scaled_wait_seconds(60), mdbx_tst_scaled_wait_seconds(60));
  ASSERT_TRUE(interrupted.exited) << "mdbx_chk must exit normally (not die by a signal), status " << interrupted.termsig
                                  << ", output:\n"
                                  << interrupted.output;
  ASSERT_TRUE(interrupted.marker_seen) << "the traversal marker was not seen - the database may be too small for the "
                                          "interrupt-at-traversal handshake; output:\n"
                                       << interrupted.output;
  EXPECT_EQ(interrupted.exit_code, kExitInterrupted) << "tool output:\n" << interrupted.output;
  EXPECT_NE(interrupted.output.find(kInterruptedMark), std::string::npos) << "no interruption mark in the output:\n"
                                                                          << interrupted.output;
  EXPECT_EQ(interrupted.output.find(kNoErrorMark), std::string::npos)
      << "the interrupted run reported a completed clean check:\n"
      << interrupted.output;
  std::fprintf(stdout, "chk-exit: interrupted run %.2fs\n", interrupted.seconds);
  std::fflush(stdout);

  /* The negative control: the very same database checked to completion
   * without a signal must exit 0 - this separates the signal path above from
   * the usage/argument errors, which also exit 5. */
  chk_run clean = run_mdbx_chk(tool, false, mdbx_tst_scaled_wait_seconds(60), mdbx_tst_scaled_wait_seconds(300));
  ASSERT_TRUE(clean.exited) << "the clean run must exit normally, output:\n" << clean.output;
  EXPECT_EQ(clean.exit_code, 0) << "tool output:\n" << clean.output;
  EXPECT_NE(clean.output.find(kNoErrorMark), std::string::npos) << "no completed-check verdict in the output:\n"
                                                                << clean.output;
  EXPECT_EQ(clean.output.find(kInterruptedMark), std::string::npos) << "unexpected interruption mark in the output:\n"
                                                                    << clean.output;
  std::fprintf(stdout, "chk-exit: clean run %.2fs\n", clean.seconds);
  std::fflush(stdout);
}
