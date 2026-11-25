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

/// The crash-window matrix of the durability test program (#564): a forked
/// victim process commits transactions with the probe-bus kill-rule armed, so
/// it dies (SIGKILL, exactly as a real writer's crash) at the chosen point of
/// the commit pipeline; the parent reopens the database and verifies the
/// durability contract end-to-end:
///  - the recovery must succeed (the environment reopens cleanly);
///  - every transaction committed before the crash is fully present;
///  - the transaction that was mid-commit at the crash is present if and only
///    if its meta-page update had completed before the kill (a written-but-
///    uncommitted meta update is accepted as weak by the boot-id check);
///  - transactions committed after the mid-commit one are absent;
///  - the recovered tree stays usable for further writes.
///
/// Windows: the fork-based victim is POSIX-only, so the suite is registered
/// for non-Windows targets only (the multi-actor Windows coverage stays with
/// the mdbx_test framework's --inject-writefault smoke scenarios).

/// The white-box include of the internals is required to learn the build's
/// resolved MDBX_LOCKING (the same as dead-owner.c++ does), hence the
/// NO_PCH registration.
#include "../../../src/essentials.h"

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"
#include "../test-env.h++"

#if defined(MDBX_PROBES)
#include "../probe-ctl.h++"
#endif /* MDBX_PROBES */

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(MDBX_PROBES)

namespace {

constexpr unsigned kBaselineTxns = 3;
constexpr unsigned kVictimTxns = 2;
constexpr size_t kVictimValueSize = 1000;
constexpr unsigned kVictimRecords = 16;

const char *const kDb = "crash-window.mdbx";

struct child_summary {
  pid_t pid = -1;
  bool signaled = false;
  int termsig = 0;
  int exit_code = 0;
};

/* The forked victim: opens the environment, commits the baseline, then arms
 * the kill-rule and commits the victim transactions - the process must die by
 * SIGKILL inside one of them. Reaching the end alive means the kill-rule did
 * not fire and the caller must fail the test. */
void run_victim(bool writemap, const char *kill_rule, unsigned kill_at) {
  setenv("MDBX_PROBES", "1", 1); /* per-process probe activation, must precede the first site hit */

  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    _exit(101);
  MDBX_env_flags_t flags = MDBX_NOSUBDIR;
  if (writemap)
    flags = MDBX_NOSUBDIR | MDBX_WRITEMAP;
  if (mdbx_env_open(env, kDb, flags, 0644) != MDBX_SUCCESS)
    _exit(102);
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    _exit(103);
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS)
    _exit(104);
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
    _exit(105);

  char key[32], value[256];
  for (unsigned i = 0; i < kBaselineTxns; ++i) {
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
      _exit(106);
    std::snprintf(key, sizeof key, "base%u", i);
    std::snprintf(value, sizeof value, "baseline-%u", i);
    MDBX_val k{key, std::strlen(key)}, v{value, std::strlen(value)};
    if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS)
      _exit(107);
    if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(108);
  }

  /* Zero the commit-step counters (the baseline commits evaluated the sites
   * too) and arm the kill at the kill_at-th evaluation after the reset. */
  char reply[256];
  if (mdbx_probe_ctl("reset commit_*", reply, sizeof reply) != 0)
    _exit(109);
  char cmd[128];
  std::snprintf(cmd, sizeof cmd, "kill %s %u", kill_rule, kill_at);
  if (mdbx_probe_ctl(cmd, reply, sizeof reply) != 0)
    _exit(110);
  std::snprintf(cmd, sizeof cmd, "query %s", kill_rule);
  mdbx_probe_ctl(cmd, reply, sizeof reply);

  std::vector<char> big(kVictimValueSize, 'x');
  for (unsigned i = 0; i < kVictimTxns; ++i) {
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
      _exit(111);
    for (unsigned j = 0; j < kVictimRecords; ++j) {
      std::snprintf(key, sizeof key, "victim%u-%02u", i, j);
      MDBX_val k{key, std::strlen(key)}, v{big.data(), big.size()};
      if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS)
        _exit(112);
    }
    /* The kill-rule must terminate the process somewhere within this call
     * (at the armed commit-step site). */
    if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(113);
  }

  /* Survived: the kill-rule never fired. */
  mdbx_env_close(env);
  _exit(42);
}

/* Runs the victim in a forked child and requires a SIGKILL death. */
void expect_victim_killed(bool writemap, const char *kill_rule, unsigned kill_at) {
  child_summary child;
  child.pid = fork();
  ASSERT_GE(child.pid, 0) << "fork() failed";
  if (child.pid == 0)
    run_victim(writemap, kill_rule, kill_at); /* never returns */
  int status = 0;
  ASSERT_EQ(waitpid(child.pid, &status, 0), child.pid) << "waitpid failed";
  if (WIFSIGNALED(status)) {
    child.signaled = true;
    child.termsig = WTERMSIG(status);
  } else if (WIFEXITED(status)) {
    child.exit_code = WEXITSTATUS(status);
  }
  /* 42 = the victim survived (the kill-rule did not fire); any other exit
   * code is the victim's own failure-path ladder. */
  ASSERT_TRUE(child.signaled && child.termsig == SIGKILL)
      << "the victim must die by SIGKILL inside the armed commit, got " << (child.signaled ? "signal" : "exit") << " "
      << (child.signaled ? child.termsig : child.exit_code);
}

/* Reopens the database and verifies the durability contract: the recovery
 * must succeed, the baseline must be intact, the victim-phase keys must be
 * present iff their commit's meta-page update had completed before the kill,
 * and the recovered tree must stay writable. */
void expect_recovery_state(bool victim_first_present) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS) << "the recovery must succeed";
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);

  char key[32];
  MDBX_val k, v;
  char expect[64];
  for (unsigned i = 0; i < kBaselineTxns; ++i) {
    std::snprintf(key, sizeof key, "base%u", i);
    std::snprintf(expect, sizeof expect, "baseline-%u", i);
    k = {key, std::strlen(key)};
    ASSERT_EQ(mdbx_get(txn, dbi, &k, &v), MDBX_SUCCESS) << "baseline key " << key << " must survive";
    ASSERT_EQ(v.iov_len, std::strlen(expect)) << "baseline value corrupted";
    ASSERT_EQ(std::memcmp(v.iov_base, expect, v.iov_len), 0) << "baseline value corrupted";
  }

  for (unsigned i = 0; i < kVictimTxns; ++i) {
    std::snprintf(key, sizeof key, "victim%u-00", i);
    k = {key, std::strlen(key)};
    const int rc = mdbx_get(txn, dbi, &k, &v);
    if (i == 0)
      EXPECT_EQ(rc, victim_first_present ? MDBX_SUCCESS : MDBX_NOTFOUND)
          << "the first victim txn (killed mid-commit) is " << (victim_first_present ? "expected to be" : "not")
          << " visible after the recovery";
    else
      EXPECT_EQ(rc, MDBX_NOTFOUND) << "the second victim txn must be absent (never committed)";
  }
  ASSERT_EQ(mdbx_txn_abort(txn), MDBX_SUCCESS);

  /* The recovered tree must stay usable. */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  char postkey[] = "post-crash", postval[] = "writable";
  MDBX_val pk{postkey, 10}, pv{postval, 8};
  ASSERT_EQ(mdbx_put(txn, dbi, &pk, &pv, MDBX_UPSERT), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS) << "the close must succeed after the recovery";
}

/* One crash-window scenario end-to-end. */
void run_crash_window(bool writemap, const char *kill_rule, unsigned kill_at, bool victim_first_present) {
  const env_files_guard files({kDb});
  /* The matrix requires the dead-owner recovery: the killed victim leaves
   * the write lock dead-owned and every subsequent commit/open strands -
   * the documented #565 limitation turns the rounds into watchdog timeouts
   * (as observed on the 2026-11-24 nightly, once the lock-1988/2001 cells
   * ran this corpus for the first time); under the Windows locking there
   * is no fork at all. The skip enumerates exactly the modes the scenario
   * cannot work in: a newly added locking mode falls through to the honest
   * run-or-fail, never to a silent skip (the dead-owner.c++ corpus carries
   * the strand characterization for these modes). */
#if MDBX_LOCKING == MDBX_LOCKING_POSIX1988 || MDBX_LOCKING == MDBX_LOCKING_POSIX2001 ||                                \
    MDBX_LOCKING == MDBX_LOCKING_WIN32FILES
  GTEST_SKIP() << "SKIPPED: the dead-owner recovery is unavailable with this locking mode (see #565)";
#endif
  /* The WRITEMAP fork-kill scenarios wedge the Valgrind-instrumented
   * process: after the victim's death the parent's recovery reopen spins
   * at 100% CPU for hours issuing nothing but the rt_sigprocmask/gettid
   * pairs of valgrind's own syscall-wrapping loop - 1.5M syscalls per 10
   * seconds, three quarters of it kernel time, with no client-relevant
   * syscall at all (no futex, no I/O, no mmap). The non-WRITEMAP siblings
   * pass, and enabling valgrind's gdbserver (--vgdb=yes) changes the
   * behavior entirely, so the wedge is valgrind-internal and cannot be
   * worked around here. */
  if (writemap && mdbx_tst_running_on_valgrind()) {
    GTEST_SKIP() << "SKIPPED under Valgrind: the WRITEMAP fork-kill recovery wedges the instrumented process";
  }
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  ASSERT_NO_FATAL_FAILURE(expect_victim_killed(writemap, kill_rule, kill_at));
  ASSERT_NO_FATAL_FAILURE(expect_recovery_state(victim_first_present));
}

} // namespace

/* --- the crash-window matrix, non-WRITEMAP transport -------------------- */

TEST(crash_windows, kill_before_write_pages) { run_crash_window(false, "commit_write_pages_begin", 1, false); }

TEST(crash_windows, kill_at_first_dirty_page) { run_crash_window(false, "commit_write_page", 1, false); }

TEST(crash_windows, kill_after_write_pages) { run_crash_window(false, "commit_write_pages_done", 1, false); }

TEST(crash_windows, kill_at_meta_update_begin) { run_crash_window(false, "commit_meta_update_begin", 1, false); }

TEST(crash_windows, kill_after_meta_update) { run_crash_window(false, "commit_meta_update_done", 1, true); }

/* NOTE: the non-WRITEMAP durable default has NO separate meta-sync step: the
 * data and the meta pages are written through the O_DSYNC dsync-fd, so the
 * meta pwrite is durable on return and the commit_meta_sync_* sites only
 * evaluate in the lazy-fd configurations (no O_DSYNC, SAFE_NOSYNC). The
 * msync-bracketed sync window is covered by the WRITEMAP tests below. */

/* --- the crash-window matrix, WRITEMAP transport ------------------------- */

TEST(crash_windows, writemap_kill_at_meta_update_begin) {
  run_crash_window(true, "commit_meta_update_begin", 1, false);
}

TEST(crash_windows, writemap_kill_after_meta_update) { run_crash_window(true, "commit_meta_update_done", 1, true); }

TEST(crash_windows, writemap_kill_before_meta_sync) { run_crash_window(true, "commit_meta_sync_begin", 1, true); }

TEST(crash_windows, writemap_kill_after_meta_sync) { run_crash_window(true, "commit_meta_sync_done", 1, true); }

#else /* !MDBX_PROBES: the kill-rule requires the probe machinery */

TEST(crash_windows, disabled_without_probes) { GTEST_SKIP() << "requires the probe-bus build (MDBX_PROBES)"; }

#endif /* MDBX_PROBES */
