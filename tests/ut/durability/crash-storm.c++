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

/// The crash-storm scenario of the durability test program (#564, cluster
/// D2): the parent fills a known baseline and closes the environment, then
/// forks N writers; each child opens its own environment and commits its own
/// key-range in a loop. The first half of the children arm the probe-bus
/// kill-rule `kill commit_meta_update_begin <c+1>` after a counter reset
/// (mirroring the crash-windows arming pattern) at their own deterministic
/// offset, and therefore die by SIGKILL inside their (c+1)-th commit - while
/// the others keep committing to the end. The parent waits all the children
/// and verifies the durability contract end-to-end:
///  - the recovery must succeed (the environment reopens cleanly);
///  - every completing child has all its key-ranges present, byte-exactly;
///  - every dying child has an exact prefix of its key-ranges present: the
///    commits that returned from mdbx_txn_commit() before the kill, and
///    nothing beyond (the mid-commit transaction is absent);
///  - the recovered tree passes a full scan (no unexpected, missing or
///    corrupted records), and a follow-up write transaction works.
///
/// Why the prefix boundary is exact, and not merely "a prefix": the armed
/// site precedes the meta-page update in every transport (the WRITEMAP
/// invalidate path and the pwrite path alike), so the killed commit's meta
/// update never starts and no weak meta exists for it - recovery picks the
/// last completed meta. Commits are serialized by the write lock, the
/// children write disjoint key ranges, and a commit that returned from
/// mdbx_txn_commit() is durable under the durable defaults - so the
/// scheduler interleaving cannot change the surviving set. The exactness is
/// pinned for both transports by the single-writer crash-windows matrix
/// (kill at `commit_meta_update_begin` -> the transaction is absent).
///
/// The storm requires the dead-owner recovery for the surviving writers (a
/// killed child leaves the write-transaction lock dead-owned): all the
/// auto-selected locking modes provide it; the legacy POSIX-2001 mode
/// strands by the documented #565 limitation - there the per-child watchdog
/// (alarm) turns a stranded sibling into a bounded, reported failure
/// instead of a hung test.
///
/// The rounds (default 2) rebuild the environment each time, so the nightly
/// accumulates the interleavings. Knobs: CRASH_STORM_WRITERS (default 6,
/// clamped to 4..8), CRASH_STORM_ROUNDS (default 2). POSIX-only (fork),
/// like the crash-windows corpus; requires the probe-bus build (MDBX_PROBES).

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

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(MDBX_PROBES)

namespace {

constexpr unsigned kBaselineTxns = 4;
constexpr unsigned kBaselineRecords = 4;
constexpr unsigned kKeysPerCommit = 8;
constexpr size_t kKeyBufferSize = 32;
constexpr size_t kValueSize = 500;
constexpr unsigned kWritersDefault = 6;
constexpr unsigned kWritersLow = 4;
constexpr unsigned kWritersHigh = 8;
constexpr unsigned kRoundsDefault = 2;
constexpr unsigned kRoundsHigh = 32;
constexpr unsigned kChildWatchdogSeconds = 20;

/* The wall-clock child watchdog scales through the shared
 * mdbx_tst_scaled_wait_seconds() (tests/ut/test-env.h++), whose Valgrind detection is
 * a runtime function - accurate whether or not the build carries
 * ENABLE_MEMCHECK. */

const char *const kDb = "crash-storm.mdbx";
const char *const kKillRule = "commit_meta_update_begin";

/* The storm design: the first half of the children die mid-commit (child c
 * inside its (c+1)-th commit), the second half complete all their commits
 * (one commit loop of the same length as the writers count). */
unsigned dying_children(const unsigned writers) { return (writers + 1) / 2; }

/* An env-var knob with a CI-safe default, clamped to the sane range. */
unsigned env_unsigned(const char *name, const unsigned fallback, const unsigned low, const unsigned high) {
  unsigned value = fallback;
  const char *const raw = mdtest_getenv(name);
  if (raw && raw[0]) {
    const long parsed = std::atol(raw);
    if (parsed > 0)
      value = static_cast<unsigned>(parsed);
  }
  return std::min(std::max(value, low), high);
}

void baseline_key(const unsigned txn, const unsigned slot, char *const buffer) {
  std::snprintf(buffer, kKeyBufferSize, "base%u-%02u", txn, slot);
}

void baseline_value(const unsigned txn, const unsigned slot, char *const buffer) {
  std::snprintf(buffer, kKeyBufferSize, "baseline-%u-%u", txn, slot);
}

/* The exact key of one storm record: the child/range/slot identity. */
void storm_key(const unsigned child, const unsigned range, const unsigned slot, char *const buffer) {
  std::snprintf(buffer, kKeyBufferSize, "s%ur%uk%02u", child, range, slot);
}

/* The exact value of one storm record: the same identity padded with 'x' to
 * the fixed size, so every recovered record is verifiable byte-exactly. */
void storm_value(const unsigned child, const unsigned range, const unsigned slot, char *const value) {
  const int n = std::snprintf(value, kValueSize, "v%u-%u-%02u-", child, range, slot);
  if (n >= 0 && static_cast<size_t>(n) < kValueSize)
    std::memset(value + n, 'x', kValueSize - static_cast<size_t>(n));
}

struct child_summary {
  pid_t pid = -1;
  bool dying = false;
  bool signaled = false;
  int termsig = 0;
  int exit_code = 0;
};

/* Reaps any not-yet-waited children on every exit path, so an ASSERT in the
 * parent cannot leave the storm running behind the file cleanup. The pids
 * live in a fixed-size array: every heap allocation made around the fork()
 * loop would be inherited by the forked children and drowned into the
 * memcheck reports (the fork hygiene rule), and the writers count is
 * clamped to a small compile-time bound anyway. */
struct child_reaper {
  static constexpr unsigned kPidsMax = 16; /* above the writers' clamp of 8 */
  pid_t pids[kPidsMax] = {};
  unsigned count = 0;
  void push(const pid_t pid) {
    ASSERT_LT(count, kPidsMax);
    pids[count++] = pid;
  }
  ~child_reaper() {
    for (unsigned i = 0; i < count; ++i) {
      int status = 0;
      while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
};
static_assert(kWritersHigh <= child_reaper::kPidsMax, "the reaper's fixed array must hold the clamped writers count");

/* The forked writer child: opens its own environment, arms the kill-rule at
 * its own offset (dying children only), then commits its key-ranges in a
 * loop. A dying child must die by SIGKILL inside its armed commit - reaching
 * the end alive means the kill-rule did not fire and the caller must fail
 * the test. A completing child closes the environment and exits zero. */
[[noreturn]] void run_storm_child(const bool writemap, const unsigned child, const unsigned writers) {
  setenv("MDBX_PROBES", "1", 1); /* per-process probe activation, must precede the first site hit */
  alarm(mdbx_tst_scaled_wait_seconds(
      kChildWatchdogSeconds)); /* the runaway cap: a stranded lock must not hang the test */

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

  /* Zero the commit-step counters (the setup commit evaluated the sites too)
   * and - for a dying child - arm the kill at this child's offset: it dies
   * at the (child+1)-th evaluation after the reset, i.e. inside its
   * (child+1)-th commit, with the first `child` ones durably completed. */
  char reply[256];
  if (mdbx_probe_ctl("reset commit_*", reply, sizeof reply) != 0)
    _exit(106);
  if (child < dying_children(writers)) {
    char cmd[128];
    std::snprintf(cmd, sizeof cmd, "kill %s %u", kKillRule, child + 1);
    if (mdbx_probe_ctl(cmd, reply, sizeof reply) != 0)
      _exit(107);
  }

  for (unsigned range = 0; range < writers; ++range) {
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
      _exit(108);
    for (unsigned slot = 0; slot < kKeysPerCommit; ++slot) {
      char key[kKeyBufferSize], value[kValueSize];
      storm_key(child, range, slot, key);
      storm_value(child, range, slot, value);
      MDBX_val k{key, std::strlen(key)}, v{value, kValueSize};
      if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS)
        _exit(109);
    }
    /* For a dying child the kill-rule must terminate the process somewhere
     * within this call (at the armed begin-of-meta-update site). */
    if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(110);
  }

  /* Survived all the commits: normal for a completing child, the kill-rule
   * did not fire for a dying one (the caller must fail the test). */
  if (mdbx_env_close(env) != MDBX_SUCCESS)
    _exit(111);
  _exit(child < dying_children(writers) ? 42 : 0);
}

/* The reopened-database verification: builds the exact expected content and
 * checks it airtight - the recovery must succeed, every surviving record
 * must be present byte-exactly, nothing else may be present, the full scan
 * must reach the end of the tree, and a follow-up write must work. */
void verify_aftermath(const bool writemap, const unsigned writers) {
  std::map<std::string, std::string> expected;
  char key[kKeyBufferSize], value[kValueSize];
  for (unsigned t = 0; t < kBaselineTxns; ++t)
    for (unsigned s = 0; s < kBaselineRecords; ++s) {
      baseline_key(t, s, key);
      baseline_value(t, s, value);
      expected[key] = value;
    }
  for (unsigned c = 0; c < writers; ++c) {
    /* A dying child c has exactly its first c ranges durable (it was killed
     * inside the (c+1)-th commit's meta-update, before any visible trace of
     * it); a completing child has all of them. */
    const unsigned present = (c < dying_children(writers)) ? c : writers;
    for (unsigned r = 0; r < present; ++r)
      for (unsigned slot = 0; slot < kKeysPerCommit; ++slot) {
        storm_key(c, r, slot, key);
        storm_value(c, r, slot, value);
        expected[key] = std::string(value, kValueSize);
      }
  }
  const size_t expected_total = expected.size();

  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  env_handle_guard env_guard(&env);
  MDBX_env_flags_t flags = MDBX_NOSUBDIR;
  if (writemap)
    flags = MDBX_NOSUBDIR | MDBX_WRITEMAP;
  ASSERT_EQ(mdbx_env_open(env, kDb, flags, 0644), MDBX_SUCCESS) << "the recovery must succeed after the storm";
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);

  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val k, v;
  size_t scanned = 0;
  int rc;
  while ((rc = mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT)) == MDBX_SUCCESS) {
    const std::string key(static_cast<const char *>(k.iov_base), k.iov_len);
    const auto it = expected.find(key);
    ASSERT_TRUE(it != expected.end()) << "an unexpected key survived the storm: `" << key << "`";
    ASSERT_EQ(v.iov_len, it->second.size()) << "the value length mismatch for `" << key << "`";
    ASSERT_EQ(std::memcmp(v.iov_base, it->second.data(), v.iov_len), 0) << "the value corrupted for `" << key << "`";
    expected.erase(it);
    ++scanned;
  }
  ASSERT_EQ(rc, MDBX_NOTFOUND) << "the full scan must reach the end of the recovered tree";
  mdbx_cursor_close(cursor);
  ASSERT_EQ(scanned, expected_total) << "the record count mismatch";
  ASSERT_TRUE(expected.empty()) << "missing keys, e.g. `" << expected.begin()->first << "`";
  ASSERT_EQ(mdbx_txn_abort(txn), MDBX_SUCCESS);

  /* The recovered tree must stay usable for further writes. */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  char postkey[] = "storm-aftermath", postval[] = "writable-after-the-storm";
  MDBX_val pk{postkey, std::strlen(postkey)}, pv{postval, std::strlen(postval)};
  ASSERT_EQ(mdbx_put(txn, dbi, &pk, &pv, MDBX_UPSERT), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS) << "the close must succeed after the recovery";
  env_guard.release();
}

/* One storm round: the baseline, the fork and the deaths, the verification. */
void run_storm_round(const bool writemap, const unsigned writers) {
  /* Phase 1 - the parent fills the baseline and closes the environment, so
   * the forked children inherit no live env handle: each opens its own. */
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  env_handle_guard env_guard(&env);
  MDBX_env_flags_t flags = MDBX_NOSUBDIR;
  if (writemap)
    flags = MDBX_NOSUBDIR | MDBX_WRITEMAP;
  ASSERT_EQ(mdbx_env_open(env, kDb, flags, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  for (unsigned t = 0; t < kBaselineTxns; ++t) {
    ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    for (unsigned s = 0; s < kBaselineRecords; ++s) {
      char key[kKeyBufferSize], value[kKeyBufferSize];
      baseline_key(t, s, key);
      baseline_value(t, s, value);
      MDBX_val k{key, std::strlen(key)}, v{value, std::strlen(value)};
      ASSERT_EQ(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
  env_guard.release();

  /* Phase 2 - the storm: fork the writers and wait them out. */
  child_reaper reaper;
  std::vector<child_summary> children(writers);
  for (unsigned c = 0; c < writers; ++c) {
    children[c].dying = (c < dying_children(writers));
    children[c].pid = fork();
    ASSERT_GE(children[c].pid, 0) << "fork() failed";
    if (children[c].pid == 0)
      run_storm_child(writemap, c, writers); /* never returns */
    reaper.push(children[c].pid);
  }
  for (unsigned c = 0; c < writers; ++c) {
    int status = 0;
    ASSERT_EQ(waitpid(children[c].pid, &status, 0), children[c].pid) << "waitpid failed for the child " << c;
    if (WIFSIGNALED(status)) {
      children[c].signaled = true;
      children[c].termsig = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
      children[c].exit_code = WEXITSTATUS(status);
    }
    if (children[c].dying) {
      /* 42 = the dying child survived (the kill-rule did not fire); any
       * other death mode is its own failure-path ladder or the watchdog. */
      ASSERT_TRUE(children[c].signaled && children[c].termsig == SIGKILL)
          << "the dying child " << c << " must be SIGKILLed inside its armed commit " << (c + 1) << ", got "
          << (children[c].signaled ? "signal" : "exit") << " "
          << (children[c].signaled ? children[c].termsig : children[c].exit_code);
    } else {
      ASSERT_TRUE(!children[c].signaled && children[c].exit_code == 0)
          << "the completing child " << c << " must finish all its " << writers << " commits cleanly, got "
          << (children[c].signaled ? "signal" : "exit") << " "
          << (children[c].signaled ? children[c].termsig : children[c].exit_code);
    }
  }
  reaper.count = 0; /* reaped explicitly, the destructor must not wait again */

  /* Phase 3 - the end-to-end verification on the recovered database. */
  ASSERT_NO_FATAL_FAILURE(verify_aftermath(writemap, writers));
}

/* The whole storm: the rounds, each rebuilding the environment. */
void run_storm(const bool writemap) {
  /* The storm requires the dead-owner recovery for the surviving writers
   * (see the header comment): the POSIX-1988/2001 modes strand the lock on
   * the killed writer - the documented #565 limitation makes the scenario
   * unwinnable beyond its first in-commit death, and under the Windows
   * locking there is no fork at all. The skip enumerates exactly the
   * modes the scenario cannot work in: a newly added locking mode falls
   * through to the honest run-or-fail, never to a silent skip (the
   * dead-owner.c++ corpus carries the strand characterization for these
   * modes). */
#if MDBX_LOCKING == MDBX_LOCKING_POSIX1988 || MDBX_LOCKING == MDBX_LOCKING_POSIX2001 ||                                \
    MDBX_LOCKING == MDBX_LOCKING_WIN32FILES
  GTEST_SKIP() << "SKIPPED: the dead-owner recovery is unavailable with this locking mode (see #565)";
#endif
  /* The WRITEMAP storm wedges the Valgrind-instrumented process the same
   * way the crash-windows WRITEMAP scenarios do: after the dying children
   * are killed, the surviving processes spin at 100% CPU (the strace
   * histogram shows nothing but valgrind's own rt_sigprocmask/gettid
   * syscall-wrapping pairs, no futex, no I/O), while the forked writers
   * wait forever in rt_mutex_schedule. The non-WRITEMAP storm passes. */
  if (writemap && mdbx_tst_running_on_valgrind()) {
    GTEST_SKIP() << "SKIPPED under Valgrind: the WRITEMAP storm wedges the instrumented process";
  }
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  /* Activate the probe bus in the parent before its first site hit, so the
   * forked children inherit an active bus regardless of the standalone/CTest
   * launch environment. */
  setenv("MDBX_PROBES", "1", 1);
  const unsigned writers = env_unsigned("CRASH_STORM_WRITERS", kWritersDefault, kWritersLow, kWritersHigh);
  const unsigned rounds = env_unsigned("CRASH_STORM_ROUNDS", kRoundsDefault, 1, kRoundsHigh);
  for (unsigned round = 0; round < rounds; ++round) {
    if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
      GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    }
    const env_files_guard files({kDb});
    ASSERT_NO_FATAL_FAILURE(run_storm_round(writemap, writers));
  }
}

} // namespace

/* --- the storm, non-WRITEMAP transport ----------------------------------- */

TEST(crash_storm, storm) { run_storm(false); }

/* --- the storm, WRITEMAP transport ---------------------------------------- */

TEST(crash_storm, writemap_storm) { run_storm(true); }

#else /* !MDBX_PROBES: the kill-rule requires the probe machinery */

TEST(crash_storm, disabled_without_probes) { GTEST_SKIP() << "requires the probe-bus build (MDBX_PROBES)"; }

#endif /* MDBX_PROBES */
