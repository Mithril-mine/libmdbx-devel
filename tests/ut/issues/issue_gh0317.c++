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
#include <gtest/gtest.h>

#include <fcntl.h>
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

/* The opener child: alarm() bounds a hung open, the exit status carries the outcome. */
[[noreturn]] void bounded_opener(int expected_rc) {
  alarm(15);
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
 * (the second one waits for the lock file while the first needs rthc_lock() to release it). */
TEST(issue_gh0317, two_threads_open_close_progress) {
  const env_files_guard guard({kDbPath});
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
  g_done[0].store(0);
  g_done[1].store(0);

  std::thread a(opener_thread, 0);
  std::thread b(opener_thread, 1);
  int stalled = 0, last0 = -1, last1 = -1;
  for (int second = 0;; ++second) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (g_done[0].load() >= kOpenIterations && g_done[1].load() >= kOpenIterations)
      break;
    if (g_done[0].load() == last0 && g_done[1].load() == last1) {
      if (++stalled >= 5)
        FAIL() << "deadlocked: the threads made no progress for " << stalled << " s, done " << g_done[0].load() << "/"
               << g_done[1].load() << " of " << kOpenIterations;
    } else
      stalled = 0;
    last0 = g_done[0].load();
    last1 = g_done[1].load();
    ASSERT_LT(second, 60) << "timeout";
  }
  a.join();
  b.join();
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
