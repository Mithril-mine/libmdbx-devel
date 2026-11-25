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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/317
//
// Three defects of mdbx_env_open() on POSIX:
// 1. a read-write open spun forever (under rthc_lock(), freezing the whole process) while another process
//    held the data file in without-lck mode; now it returns MDBX_BUSY after about two seconds;
// 2. two threads opening the same path deadlocked when the first open failed: the second one waited for
//    the lock file (blocking, under rthc_lock()) while the first needed rthc_lock() to release it; the
//    shared-lock wait is bounded the same way now;
// 3. a first opener with MDBX_RDONLY|MDBX_NORDAHEAD stored both bits in lck->envmode, and a later
//    MDBX_ACCEDE writer inherited MDBX_RDONLY, so its first write transaction failed with EACCES.
// 4. a duplicate open of the same database in one process (via the same or another pathname) is
//    refused with MDBX_BUSY before the lck-file is opened, so the refusal does not damage the
//    already-opened env (with classic fcntl-locks closing any lck descriptor would release all
//    the process's locks on the file); a hardlinked datafile via another pathname is refused as
//    well. With MDBX_DBG_LEGACY_MULTIOPEN the duplicate opens cooperate.
//
// POSIX-only: the cases use fork(), file permissions and alarm() to bound a hung open. Every case fails
// within a bounded time on the unfixed code, so a regression fails the test instead of hanging the CI cell.

#include "../env-guard.h++"
#include "../test-env.h++"
#include <gtest/gtest.h>

#include <cerrno>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

/* The watchdog budgets below scale through the shared mdbx_tst_scaled_wait_seconds()
 * (tests/ut/test-env.h++), whose Valgrind detection is a runtime function -
 * no valgrind.h macros leak into this translation unit. */

namespace {

constexpr const char *const kDbPath = "issue317.mdbx";
constexpr char kValue[] = "a value written by the sibling opener";
constexpr int kOpenIterations = 3000;

/* A child-friendly check: no gtest across fork(), bail out with a distinct status. */
#define MUST_SILENT(x)                                                                                                 \
  do {                                                                                                                 \
    if ((x))                                                                                                           \
      _exit(3);                                                                                                        \
  } while (0)

void make_db(const char *path) {
  MDBX_env *env = nullptr;
  MUST_SILENT(mdbx_env_create(&env));
  MUST_SILENT(mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  MUST_SILENT(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  MUST_SILENT(mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  MDBX_val key = {const_cast<char *>("key"), 3}, value = {const_cast<char *>("value"), 5};
  MUST_SILENT(mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT));
  MUST_SILENT(mdbx_txn_commit(txn));
  MUST_SILENT(mdbx_env_close(env));
}

/* A child that keeps the environment open in the without-lck mode (MDBX_RDONLY|MDBX_EXCLUSIVE on read-only
 * files), i.e. holds the data file exclusively while nobody holds the lock file. */
[[noreturn]] void without_lck_holder(int ack_fd, int go_fd) {
  MDBX_env *env = nullptr;
  MUST_SILENT(mdbx_env_create(&env));
  const int rc = mdbx_env_open(env, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE), 0);
  if (rc != MDBX_SUCCESS)
    _exit(8);
  char b;
  if (write(ack_fd, "r", 1) != 1 || read(go_fd, &b, 1) != 1)
    _exit(3);
  mdbx_env_close(env);
  _exit(0);
}

/* The opener child: alarm() bounds a hung open, the exit status carries the outcome.
 * The budget scales under Valgrind like the watchdogs above (the ~2s bounded
 * BUSY wait alone becomes minutes under the memcheck sweep's slowdown). */
[[noreturn]] void bounded_opener(int expected_rc) {
  alarm(mdbx_tst_running_on_valgrind() ? 15 * 42 : 15);
  MDBX_env *env = nullptr;
  MUST_SILENT(mdbx_env_create(&env));
  const int rc = mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644);
  mdbx_env_close(env);
  _exit(rc == expected_rc ? 0 : 9);
}

std::atomic<int> g_done[2];
const char *g_junk_path = nullptr;

void opener_thread(const int n) {
  for (int i = 0; i < kOpenIterations; ++i) {
    MDBX_env *env = nullptr;
    if (mdbx_env_create(&env))
      _exit(3);
    (void)mdbx_env_open(env, g_junk_path, MDBX_NOSUBDIR, 0644);
    mdbx_env_close(env);
    g_done[n].store(i + 1, std::memory_order_relaxed);
  }
}

/* Best-effort dump of the threads' state of the given process, to turn a stall
 * occurrence into a diagnosable event: per-thread backtraces via `sample` on
 * macOS, the current syscall and the kernel wait-channel of each task via
 * /proc elsewhere. */
void dump_threads_state(const pid_t target, const char *why) {
  char cmd[512];
  char sample_path[] = "issue317-sample-XXXXXX";
#if defined(__APPLE__)
  /* `sample <pid> <seconds> -file <path>` provides per-thread backtraces on every macOS. */
  const int fd = ::mkstemp(sample_path);
  if (fd >= 0)
    ::close(fd);
  std::snprintf(cmd, sizeof(cmd), "sample %d 2 -file '%s' >/dev/null 2>&1", (int)target, sample_path);
  if (::system(cmd) == 0) {
    FILE *f = std::fopen(sample_path, "r");
    if (f) {
      fprintf(stderr, "---- threads sample (%s) ----\n", why);
      char line[512];
      for (int lines = 0; lines < 200 && std::fgets(line, sizeof(line), f); ++lines)
        fputs(line, stderr);
      std::fclose(f);
    }
  } else
    fprintf(stderr, "the `sample` of pid %d failed (%s)\n", (int)target, why);
#else
  char taskdir[64];
  fprintf(stderr, "---- threads syscall/wchan (%s) ----\n", why);
  std::snprintf(taskdir, sizeof(taskdir), "/proc/%d/task", (int)target);
  std::snprintf(cmd, sizeof(cmd), "%s/*", taskdir);
  glob_t gl;
  if (::glob(cmd, 0, nullptr, &gl) == 0) {
    for (size_t i = 0; i < gl.gl_pathc; ++i) {
      char sub[128], buf[256];
      FILE *f;
      std::snprintf(sub, sizeof(sub), "%s/syscall", gl.gl_pathv[i]);
      if ((f = std::fopen(sub, "r")) != nullptr) {
        if (std::fgets(buf, sizeof(buf), f)) {
          buf[strcspn(buf, "\n")] = 0;
          fprintf(stderr, "tid %s syscall: %s\n", gl.gl_pathv[i] + strlen(taskdir) + 1, buf);
        }
        std::fclose(f);
      }
      std::snprintf(sub, sizeof(sub), "%s/wchan", gl.gl_pathv[i]);
      if ((f = std::fopen(sub, "r")) != nullptr) {
        if (std::fgets(buf, sizeof(buf), f)) {
          buf[strcspn(buf, "\n")] = 0;
          fprintf(stderr, "tid %s wchan: %s\n", gl.gl_pathv[i] + strlen(taskdir) + 1, buf);
        }
        std::fclose(f);
      }
    }
    globfree(&gl);
  }
#endif
  ::unlink(sample_path);
}

/* The scenario, in a child process without other threads; returns its exit status. */
int two_threads_scenario() {
  g_done[0].store(0);
  g_done[1].store(0);
  std::thread a(opener_thread, 0);
  std::thread b(opener_thread, 1);
  /* The total budget is wall-clock and calibrated for the reference x86-64
   * setup: on the systematically slower runners (the arm cells, see the
   * 2026-11-24 nightly: one thread starved the other at 1079 of 3000
   * iterations, while the parent's own watchdog expired at its unscaled
   * 75 s) the iteration target is reached later in wall time, so it scales
   * by the same MDBX_TEST_TIMEOUT_SCALE factor the CTest TIMEOUTs use -
   * the two mechanisms are one scheme. The stall limit stays unscaled: it
   * is diagnostics-only and must fire early on a slow runner too. */
  /* The stall limit stays diagnostics-only: it must fire early on a slow
   * runner too, so it keeps its native budget (the pre-scaled
   * bounded_child_wait() applies the Valgrind factor on its own). */
  const int stall_limit = mdbx_tst_running_on_valgrind() ? 5 * 42 : 5;
  const int total_limit = mdbx_tst_scaled_wait_seconds(60);
  int last0 = -1, last1 = -1, stalled = 0;
  bool reported_stall = false;
  for (int second = 0;; ++second) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (g_done[0].load() >= kOpenIterations && g_done[1].load() >= kOpenIterations) {
      if (reported_stall)
        fprintf(stderr, "the progress resumed after the stall, done %d/%d of %d\n", g_done[0].load(), g_done[1].load(),
                kOpenIterations);
      break;
    }
    if (g_done[0].load() == last0 && g_done[1].load() == last1) {
      ++stalled;
      if (stalled == stall_limit && !reported_stall) {
        /* Capture the diagnostics at the first stall, but let the scenario
         * continue: a slow/loaded runner may legitimately pause a thread for
         * seconds, while a real deadlock hits the total limit below. */
        reported_stall = true;
        dump_threads_state(::getpid(), "stall detected");
      }
    } else {
      stalled = 0;
      last0 = g_done[0].load();
      last1 = g_done[1].load();
    }
    if (second >= total_limit) {
      fprintf(stderr, "deadlocked: done %d/%d of %d after %u s\n", g_done[0].load(), g_done[1].load(), kOpenIterations,
              second);
      dump_threads_state(::getpid(), "the total limit reached");
      _exit(6);
    }
  }
  a.join();
  b.join();
  _exit(0);
}

/* Waits up to the given seconds for the child, scaled under Valgrind in line
 * with the ctest timeouts' scaling. Returns true and its wait status when it
 * exits, or false when it hangs (the caller kills it). */
bool bounded_child_wait(pid_t pid, int &status, unsigned seconds) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(mdbx_tst_running_on_valgrind() ? seconds * 42 : seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid)
      return true;
    if (got < 0 && errno != EINTR)
      break;
    usleep(1000);
  }
  return false;
}

} // namespace

/* Case 1: a read-write open against a without-lck holder must return MDBX_BUSY in a bounded time
 * instead of spinning forever. */
TEST(issue_gh0317, open_bounded_against_without_lck_holder) {
  const env_files_guard guard({kDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  make_db(kDbPath);
  char lck_path[512];
  std::snprintf(lck_path, sizeof(lck_path), "%s-lck", kDbPath);
  ASSERT_EQ(0, ::chmod(kDbPath, 0444));
  ASSERT_EQ(0, ::chmod(lck_path, 0444));

  int go[2], ack[2];
  ASSERT_NE(-1, pipe(go));
  ASSERT_NE(-1, pipe(ack));
  fflush(nullptr);
  const pid_t holder = fork();
  ASSERT_GE(holder, 0);
  if (holder == 0) {
    ::close(go[1]);
    ::close(ack[0]);
    without_lck_holder(ack[1], go[0]);
  }
  ::close(go[0]);
  ::close(ack[1]);

  char b;
  ASSERT_EQ(1, read(ack[0], &b, 1)) << "the without-lck holder failed to start";
  ASSERT_EQ(0, ::chmod(kDbPath, 0644));
  ASSERT_EQ(0, ::chmod(lck_path, 0644));

  const pid_t opener = fork();
  ASSERT_GE(opener, 0);
  if (opener == 0) {
    ::close(go[0]);
    ::close(go[1]);
    ::close(ack[0]);
    bounded_opener(MDBX_BUSY);
  }
  int status = 0;
  ASSERT_EQ(opener, waitpid(opener, &status, 0));
  EXPECT_FALSE(WIFSIGNALED(status)) << "the open spun forever and was killed by signal " << WTERMSIG(status);
  if (!WIFSIGNALED(status)) {
    EXPECT_EQ(0, WEXITSTATUS(status)) << "mdbx_env_open() returned neither the expected MDBX_BUSY nor hung";
  }

  ASSERT_EQ(1, write(go[1], "g", 1));
  ::close(go[1]);
  ::close(ack[0]);
  ASSERT_EQ(holder, waitpid(holder, &status, 0));
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "the without-lck holder failed";
}

/* Case 2: two threads opening the same non-database path must make progress instead of deadlocking
 * (the second one waits for the lock file while the first needs rthc_lock() to release it).
 *
 * The scenario runs in a forked child: a detected stall must not terminate the whole test binary
 * (joinable std::threads left behind by a failing gtest would std::terminate it), and a truly hung
 * child is bounded by the parent's watchdog, which samples the child before killing it. */
TEST(issue_gh0317, two_threads_open_close_progress) {
  const env_files_guard guard({kDbPath});
  /* The Valgrind-instrumented run wedges before making any progress: the
   * two churning threads and the forked children end up in the sampled
   * futex_do_wait/fcntl_setlk stacks for hours (the 2026-11-24 sweep: the
   * internal watchdog fired at 52.5 minutes with no iterations done),
   * which is the same valgrind-internal wedge family as the WRITEMAP
   * fork-kill scenarios of the durability corpus. */
  if (mdbx_tst_running_on_valgrind()) {
    GTEST_SKIP() << "SKIPPED under Valgrind: the open/close churn wedges the instrumented process";
  }
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  {
    int fd = ::open(kDbPath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    ASSERT_NE(-1, fd);
    static char junk[1 << 16];
    memset(junk, 0x5A, sizeof(junk));
    ASSERT_EQ(sizeof(junk), (size_t)write(fd, junk, sizeof(junk)));
    ASSERT_EQ(0, ::close(fd));
  }
  g_junk_path = kDbPath;

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_GE(pid, 0) << "fork() failed";
  if (pid == 0)
    _exit(two_threads_scenario());
  int status = 0;
  /* The watchdog must outlive the scenario's own scaled budget (60 s), the
   * same MDBX_TEST_TIMEOUT_SCALE factor applies (see the scenario above);
   * bounded_child_wait() adds the Valgrind factor on its own. */
  if (!bounded_child_wait(pid, status, 75u * mdbx_tst_timeout_scale())) {
    dump_threads_state(pid, "the child hung, sampled before kill");
    ::kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    FAIL() << "deadlock: the scenario made no progress within the watchdog budget";
  }
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "the scenario failed, wait status 0x" << std::hex << status;
}

/* Case 3: a first opener with MDBX_RDONLY|MDBX_NORDAHEAD must not make a later MDBX_ACCEDE writer
 * inherit MDBX_RDONLY. */
TEST(issue_gh0317, accede_writer_is_not_readonly) {
  const env_files_guard guard({kDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  make_db(kDbPath);
  int go[2], ack[2];
  ASSERT_NE(-1, pipe(go));
  ASSERT_NE(-1, pipe(ack));
  fflush(nullptr);
  const pid_t reader = fork();
  ASSERT_GE(reader, 0);
  if (reader == 0) {
    ::close(go[1]);
    ::close(ack[0]);
    MDBX_env *env = nullptr;
    MUST_SILENT(mdbx_env_create(&env));
    MUST_SILENT(mdbx_env_open(env, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_NORDAHEAD), 0));
    char b;
    if (write(ack[1], "r", 1) != 1 || read(go[0], &b, 1) != 1)
      _exit(3);
    mdbx_env_close(env);
    _exit(0);
  }
  ::close(go[0]);
  ::close(ack[1]);

  char b;
  ASSERT_EQ(1, read(ack[0], &b, 1)) << "the reader failed to start";

  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_ACCEDE), 0644));
  unsigned flags = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_flags(env, &flags));
  EXPECT_EQ(0u, flags & unsigned(MDBX_RDONLY)) << "the ACCEDE writer inherited MDBX_RDONLY from the first opener";
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn))
      << "the ACCEDE writer cannot start a write transaction";

  ASSERT_EQ(1, write(go[1], "g", 1));
  ::close(go[1]);
  ::close(ack[0]);
  int status = 0;
  ASSERT_EQ(reader, waitpid(reader, &status, 0));
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "the reader failed";
}

/* The in-process sibling (the same database opened twice in one process) is refused with MDBX_BUSY,
 * and the refusal does not damage the already-opened env. */
TEST(issue_gh0317, sibling_open_refused_preserves_holder) {
  const env_files_guard guard({kDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  make_db(kDbPath);
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR), 0644));
  {
    MDBX_env *dup = nullptr;
    env_handle_guard dup_owner(&dup);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&dup));
    ASSERT_EQ(MDBX_BUSY, mdbx_env_open(dup, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR), 0644));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(dup));
    /* the dup_handle_guard still owns the handle: clear it after the close,
     * or the guard would close the dangling pointer once again (UAF) */
    dup = nullptr;
  }
  /* the holder survived the refusal and remains usable */
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_val k = {const_cast<char *>("kept"), 4}, v = {const_cast<char *>(kValue), sizeof(kValue)};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, 1, &k, &v, MDBX_UPSERT));
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

/* Without-lck readers coexist: a without-lck (e.g. on a read-only or inaccessible lock file) reader
 * does not conflict with anybody, and a subsequent reader - even one that gets the lock file -
 * opens as well (the without-lck mode is unsynchronized by its documented semantics). */
TEST(issue_gh0317, lckless_readers_coexist) {
  const env_files_guard guard({kDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* The without-lck mode is induced here by making the lock file unwritable
   * (chmod 0444), which root bypasses: under root the "lckless" reader
   * opens the lock file normally, and then the subsequent reader is refused
   * with MDBX_BUSY as an in-process sibling instead of coexisting. Skip
   * rather than test nothing meaningfully. */
  if (::geteuid() == 0)
    GTEST_SKIP() << "the without-lck mode cannot be induced via file permissions under root";
  make_db(kDbPath);
  char lck_path[512];
  std::snprintf(lck_path, sizeof(lck_path), "%s-lck", kDbPath);
  ASSERT_EQ(0, ::chmod(lck_path, 0444));

  /* the first reader runs in the without-lck mode */
  MDBX_env *first = nullptr;
  env_handle_guard first_owner(&first);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&first));
  EXPECT_EQ(MDBX_SUCCESS,
            mdbx_env_open(first, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE), 0))
      << "the without-lck reader failed to open";
  ASSERT_EQ(0, ::chmod(lck_path, 0644));
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(first, nullptr, MDBX_TXN_RDONLY, &txn));
    MDBX_val key = {const_cast<char *>("key"), 3}, value;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, 1, &key, &value));
    txn_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }

  /* the second reader gets the lock file, but must not be refused by the without-lck sibling */
  MDBX_env *second = nullptr;
  env_handle_guard second_owner(&second);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&second));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_open(second, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_RDONLY), 0))
      << "a reader was refused by a without-lck sibling";
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(second, nullptr, MDBX_TXN_RDONLY, &txn));
    MDBX_val key = {const_cast<char *>("key"), 3}, value;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, 1, &key, &value));
    txn_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }
  ASSERT_EQ(0, ::chmod(lck_path, 0644));
}

/* An exclusive opener is refused while a without-lck sibling holds the data file, and the sibling
 * survives the refusal. */
TEST(issue_gh0317, exclusive_refused_against_lckless_sibling) {
  const env_files_guard guard({kDbPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  make_db(kDbPath);
  char lck_path[512];
  std::snprintf(lck_path, sizeof(lck_path), "%s-lck", kDbPath);
  ASSERT_EQ(0, ::chmod(lck_path, 0444));

  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS,
            mdbx_env_open(env, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE), 0));
  {
    MDBX_env *dup = nullptr;
    env_handle_guard dup_owner(&dup);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&dup));
    ASSERT_EQ(MDBX_BUSY, mdbx_env_open(dup, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE), 0))
        << "an exclusive opener must be refused by the without-lck sibling";
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(dup));
    /* the dup_handle_guard still owns the handle: clear it after the close,
     * or the guard would close the dangling pointer once again (UAF) */
    dup = nullptr;
  }
  /* the holder survived the refusal and remains usable */
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_val key = {const_cast<char *>("key"), 3}, value;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, 1, &key, &value));
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  ASSERT_EQ(0, ::chmod(lck_path, 0644));
}

/* The same datafile exposed through another pathname (a hardlink) with its own lck-file is refused
 * (one dxb-file must not be opened twice with different lock domains). */
TEST(issue_gh0317, same_dxb_another_lck_refused) {
  const char *const copy_path = "issue317-copy.mdbx";
  const env_files_guard guard({kDbPath, copy_path});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  make_db(kDbPath);
  ASSERT_EQ(0, ::link(kDbPath, copy_path)) << "hardlink: " << strerror(errno);
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_env_flags_t(MDBX_NOSUBDIR), 0644));
  {
    MDBX_env *dup = nullptr;
    env_handle_guard dup_owner(&dup);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&dup));
    ASSERT_EQ(MDBX_BUSY, mdbx_env_open(dup, copy_path, MDBX_env_flags_t(MDBX_NOSUBDIR), 0644))
        << "the same datafile via another pathname must be refused";
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(dup));
    /* the dup_handle_guard still owns the handle: clear it after the close,
     * or the guard would close the dangling pointer once again (UAF) */
    dup = nullptr;
  }
  /* the holder survived and remains usable */
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_val key = {const_cast<char *>("key"), 3}, value;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, 1, &key, &value));
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
}
