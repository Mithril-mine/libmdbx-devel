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

/// The lock-events smoke of the durability program (#564, the #562
/// follow-up): two writers contend for the write-transaction lock while a
/// third writer acquires it, holds it briefly and dies by SIGKILL - the
/// incident shape of #562, whose own log was flush-time interleaved and
/// useless as evidence. Here every participant polls the ipclock_* probe
/// sites right after each lock-lifecycle operation and emits one atomic line
/// per poll into the shared stderr event log, so the log carries the TRUE
/// call-time interleaving of the lock lifecycle.
///
/// The probe registry is per-process, so the poll runs in the process that
/// performed the operation - the forked children do exactly that. Each poll
/// line reports the per-site seen counters and the last captured value:
/// for the lock site 0 = acquired, MDBX_RESULT_TRUE = acquired with the
/// dead-owner recovery, MDBX_BUSY = a try-lock found it busy, otherwise the
/// error code; for the unlock site 0 = unlocked, otherwise the error code;
/// the forkswallow site counts the swallowed failures of a forked child's
/// unlock (never fired by this scenario: the children open their own
/// environments, it is the diagnostic for the real incidents).
///
/// On Linux the test runs against whatever MDBX_LOCKING the build resolved
/// to (POSIX-2008 by default): the writers proceed past the killed holder
/// and at least one of them observes the recovered (MDBX_RESULT_TRUE)
/// acquisition. On the modes without dead-owner recovery the strand is
/// captured the same way - the log's silence after the writers' last unlock
/// is the strand signature, and their watchdog deaths are the expected
/// outcome (the #565 limitation, see dead-owner.c++). The value here is the
/// instrumentation plumbing itself, exercised nightly on the lock-2001 cell
/// too.

#include "../../src/essentials.h" /* the resolved MDBX_LOCKING; requires the NO_PCH setup */

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"
#include "../test-env.h++"

#if defined(MDBX_PROBES)
#include "../probe-ctl.h++"
#endif /* MDBX_PROBES */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

namespace {

constexpr unsigned kWatchdogSeconds = 5;        /* the writers' strand watchdog, the dead-owner pattern */
constexpr unsigned kVictimWatchdogSeconds = 10; /* the victim's own bound (readiness wait included) */
constexpr unsigned kHoldMillis = 20;            /* the victim holds the write lock before the SIGKILL */
constexpr unsigned kGraceMillis = 200;          /* the parent's grace before the stop flag */
constexpr unsigned kWarmupTxns = 3;             /* the pre-kill contended transactions per writer */
constexpr unsigned kTxnsHardCap = 5000;         /* belt-and-braces, the alarm is the primary bound */
constexpr unsigned kReadyDeadlineSeconds = 8;
constexpr unsigned kCollectDeadlineSeconds = 10;

const char *const kDb = "lock-events.mdbx";
const char *const kCoordDir = "lock-events-coord";
const char *const kReadyA = "ready-writer-a";
const char *const kReadyB = "ready-writer-b";
const char *const kHolding = "victim-holding";
const char *const kStop = "stop";
const char *const kStatusA = "status-writer-a";
const char *const kStatusB = "status-writer-b";

/* --- the coordination files ----------------------------------------------- */

void write_all(int fd, const char *data, size_t len) {
  while (len) {
    const ssize_t written = write(fd, data, len);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return; /* the event log and the statuses are best-effort */
    }
    data += written;
    len -= static_cast<size_t>(written);
  }
}

void coord_path(char *path, size_t size, const char *name) { snprintf(path, size, "%s/%s", kCoordDir, name); }

void coord_touch(const char *name) {
  char path[128];
  coord_path(path, sizeof path, name);
  const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd >= 0)
    close(fd);
}

bool coord_exists(const char *name) {
  char path[128];
  coord_path(path, sizeof path, name);
  struct stat st;
  return stat(path, &st) == 0;
}

void coord_write_line(const char *name, const char *data, size_t len) {
  char path[128];
  coord_path(path, sizeof path, name);
  const int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return;
  write_all(fd, data, len);
  close(fd);
}

bool coord_read_line(const char *name, char *line, size_t size) {
  char path[128];
  coord_path(path, sizeof path, name);
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  const bool ok = fgets(line, static_cast<int>(size), file) != nullptr;
  fclose(file);
  return ok;
}

/* The RAII caretaker of the coordination directory: creates it at the
 * construction and removes the flags/statuses with the directory itself at
 * any destruction, including the ASSERT failures mid-scenario. */
struct coord_dir_guard {
  bool made = false;
  coord_dir_guard() {
    if (mkdir(kCoordDir, 0777) == 0 || errno == EEXIST)
      made = true;
  }
  coord_dir_guard(const coord_dir_guard &) = delete;
  coord_dir_guard &operator=(const coord_dir_guard &) = delete;
  ~coord_dir_guard() {
    if (!made)
      return;
    static const char *const names[] = {kReadyA, kReadyB, kHolding, kStop, kStatusA, kStatusB};
    for (const char *name : names) {
      char path[128];
      coord_path(path, sizeof path, name);
      unlink(path);
    }
    rmdir(kCoordDir);
  }
};

/* --- the probe polls into the stderr event log ----------------------------- */

#if defined(MDBX_PROBES)

/* The snapshot of this process' ipclock_* sites. */
struct lock_probe_snapshot {
  bool valid = false;
  size_t lock_seen = 0;
  long lock_value = 0; /* the last lock outcome, the engine's int rc */
  size_t unlock_seen = 0;
  long unlock_value = 0;
  size_t swallow_seen = 0;
};

/* The reply lines follow the ctl format "site <name> <kind> <armed> <seen>
 * <hits> <suppressed> <value> <file>:<line>"; the stored value is the
 * size_t image of the engine's int rc, so the low half carries the two's
 * complement on both 32/64-bit platforms. */
lock_probe_snapshot query_lock_probes() {
  lock_probe_snapshot snap;
  char reply[1024];
  if (mdbx_probe_ctl("query ipclock_*", reply, sizeof reply) != 0)
    return snap;
  snap.valid = true;
  for (char *line = strtok(reply, "\n"); line; line = strtok(nullptr, "\n")) {
    char name[64];
    unsigned kind = 0, armed = 0;
    unsigned long long seen = 0, hits = 0, suppressed = 0, raw = 0;
    if (sscanf(line, "site %63s %u %u %llu %llu %llu %llu", name, &kind, &armed, &seen, &hits, &suppressed, &raw) != 7)
      continue;
    const long value = static_cast<long>(static_cast<int32_t>(raw));
    if (strcmp(name, "ipclock_lock_result") == 0) {
      snap.lock_seen = static_cast<size_t>(seen);
      snap.lock_value = value;
    } else if (strcmp(name, "ipclock_unlock_result") == 0) {
      snap.unlock_seen = static_cast<size_t>(seen);
      snap.unlock_value = value;
    } else if (strcmp(name, "ipclock_unlock_forkswallow") == 0) {
      snap.swallow_seen = static_cast<size_t>(seen);
    }
  }
  return snap;
}

/* The single poll entry every participant calls right after each
 * lock-lifecycle operation: query this process' registry, append one line
 * to the shared stderr event log. One write() per line keeps the
 * cross-process interleave tear-free, and the poll is issued at the call
 * time of the operation - not at some later flush. */
void log_lock_events(const char *role, const char *phase, bool *recovered_seen) {
  const lock_probe_snapshot snap = query_lock_probes();
  if (!snap.valid)
    return;
  if (recovered_seen && snap.lock_value == MDBX_RESULT_TRUE)
    *recovered_seen = true;
  char line[192];
  const int n = snprintf(line, sizeof line, "lock-events|pid=%ld|%s|%s|lock=%zu/%ld|unlock=%zu/%ld|swallow=%zu\n",
                         static_cast<long>(getpid()), role, phase, snap.lock_seen, snap.lock_value, snap.unlock_seen,
                         snap.unlock_value, snap.swallow_seen);
  if (n > 0 && static_cast<size_t>(n) < sizeof line)
    write_all(STDERR_FILENO, line, static_cast<size_t>(n));
}

#else /* !MDBX_PROBES */

inline void log_lock_events(const char *, const char *, bool *) {}

#endif /* MDBX_PROBES */

/* --- the forked participants ----------------------------------------------- */

/* The forked writer: contends for the write lock in a bounded loop, polling
 * the probes into the event log after every operation. The stop flag appears
 * only after the victim's death, so the closing transaction below is
 * post-mortem by construction. The summary goes through the status file
 * "writer-status|<role>|<txns>|<recovered>". */
void run_writer(const char *role, const char *ready_flag, const char *status_flag) {
  alarm(kWatchdogSeconds);
#if defined(MDBX_PROBES)
  setenv("MDBX_PROBES", "1", 1); /* per-process probe activation, must precede the first site hit */
#endif                           /* MDBX_PROBES */
  unsigned txns = 0;
  bool recovered_seen = false;

  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    _exit(101);
  if (mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS)
    _exit(102);
  log_lock_events(role, "open", nullptr);

  for (unsigned i = 0; i < kWarmupTxns; ++i) {
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
      _exit(103);
    log_lock_events(role, "warmup-begin", &recovered_seen);
    if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(104);
    log_lock_events(role, "warmup-commit", nullptr);
    usleep(1000u);
  }
  coord_touch(ready_flag);

  char phase[32];
  while (!coord_exists(kStop)) {
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
      _exit(105);
    snprintf(phase, sizeof phase, "begin#%u", txns);
    log_lock_events(role, phase, &recovered_seen);
    if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(106);
    snprintf(phase, sizeof phase, "commit#%u", txns);
    log_lock_events(role, phase, nullptr);
    ++txns;
    if (txns >= kTxnsHardCap)
      break;
    usleep(1000u);
  }

  /* The closing transaction: post-mortem by construction. */
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    _exit(107);
  log_lock_events(role, "closing-begin", &recovered_seen);
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
    _exit(108);
  log_lock_events(role, "closing-commit", nullptr);
  ++txns;
  log_lock_events(role, "close", nullptr);
  if (mdbx_env_close(env) != MDBX_SUCCESS)
    _exit(109);

  char status[96];
  const int n = snprintf(status, sizeof status, "writer-status|%s|%u|%d\n", role, txns, recovered_seen ? 1 : 0);
  if (n > 0)
    coord_write_line(status_flag, status, static_cast<size_t>(n));
  _exit(0);
}

/* The forked victim: waits for both writers' readiness, acquires the write
 * lock amid the contention, holds it briefly, marks the hold and dies by
 * SIGKILL - the write lock is left owned by a dead process while the writers
 * keep contending. */
void run_victim() {
  alarm(kVictimWatchdogSeconds);
#if defined(MDBX_PROBES)
  setenv("MDBX_PROBES", "1", 1); /* per-process probe activation, must precede the first site hit */
#endif                           /* MDBX_PROBES */
  bool ready = false;
  for (unsigned spin = 0; spin < kReadyDeadlineSeconds * 1000; ++spin) {
    if (coord_exists(kReadyA) && coord_exists(kReadyB)) {
      ready = true;
      break;
    }
    usleep(1000u);
  }
  if (!ready)
    _exit(120); /* the writers never became ready */

  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    _exit(121);
  if (mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS)
    _exit(122);
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    _exit(123);
  log_lock_events("victim", "holding", nullptr);
  usleep(kHoldMillis * 1000u);
  coord_touch(kHolding);
  log_lock_events("victim", "dying", nullptr);
  raise(SIGKILL);
  _exit(124); /* unreachable */
}

/* Collects the child with a deadline; on a timeout kills it and returns false. */
bool collect_child(pid_t pid, int &status, unsigned deadline_seconds) {
  for (unsigned spin = 0; spin < deadline_seconds * 100; ++spin) {
    const pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid)
      return true;
    if (got < 0)
      return false;
    usleep(10 * 1000u);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return false;
}

/* Used only in the dead-owner recovery configurations (the POSIX-1988/2001
 * cells strand their writers instead, see the caller below), so mark it to
 * stay buildable with -Werror in those configurations. */
MDBX_MAYBE_UNUSED bool parse_writer_status(const char *status_flag, unsigned &txns, bool &recovered) {
  char line[96];
  if (!coord_read_line(status_flag, line, sizeof line))
    return false;
  int rec = 0;
  if (sscanf(line, "writer-status|%*[^|]|%u|%d", &txns, &rec) != 2)
    return false;
  recovered = rec != 0;
  return true;
}

} // namespace

TEST(lock_events, contention_capture) {
  const env_files_guard files({kDb});
  coord_dir_guard coord;
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
#if defined(MDBX_PROBES)
  setenv("MDBX_PROBES", "1", 1);
#endif /* MDBX_PROBES */

  /* The parent holds no environment while forking, so the children inherit
   * nothing to clean up (the tests/AGENTS.md hygiene rule). */
  const pid_t pid_a = fork();
  ASSERT_GE(pid_a, 0) << "fork() failed";
  if (pid_a == 0)
    run_writer("writer-a", kReadyA, kStatusA); /* never returns */
  const pid_t pid_b = fork();
  ASSERT_GE(pid_b, 0) << "fork() failed";
  if (pid_b == 0)
    run_writer("writer-b", kReadyB, kStatusB); /* never returns */
  const pid_t pid_v = fork();
  ASSERT_GE(pid_v, 0) << "fork() failed";
  if (pid_v == 0)
    run_victim(); /* never returns */

  /* Wait for the victim's hold: the coordination flag appears while the
   * writers are mid-contention, right before the SIGKILL. */
  bool holding = false;
  for (unsigned spin = 0; spin < (kReadyDeadlineSeconds + kVictimWatchdogSeconds) * 100; ++spin) {
    if (coord_exists(kHolding)) {
      holding = true;
      break;
    }
    usleep(10 * 1000u);
  }
  ASSERT_TRUE(holding) << "the victim never acquired the write lock";
  log_lock_events("parent", "victim-holding", nullptr);
  usleep(kGraceMillis * 1000u); /* the writers pile up behind the dead-owned lock */
  coord_touch(kStop);
  log_lock_events("parent", "stop-set", nullptr);

  int status_a = 0, status_b = 0, status_v = 0;
  ASSERT_TRUE(collect_child(pid_a, status_a, kCollectDeadlineSeconds)) << "writer-a was not collected";
  ASSERT_TRUE(collect_child(pid_b, status_b, kCollectDeadlineSeconds)) << "writer-b was not collected";
  ASSERT_TRUE(collect_child(pid_v, status_v, kVictimWatchdogSeconds)) << "the victim was not collected";
  log_lock_events("parent", "collected", nullptr);

  /* The victim must have died by SIGKILL exactly as a crashed writer does. */
  ASSERT_TRUE(WIFSIGNALED(status_v) && WTERMSIG(status_v) == SIGKILL)
      << "the victim must die by SIGKILL, got "
      << (WIFSIGNALED(status_v) ? WTERMSIG(status_v) : -WEXITSTATUS(status_v));

  const bool a_exited = WIFEXITED(status_a) && WEXITSTATUS(status_a) == 0;
  const bool b_exited = WIFEXITED(status_b) && WEXITSTATUS(status_b) == 0;

#if MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  /* The documented #565 limitation: no dead-owner recovery, the surviving
   * openers strand inside the C-library - the characterization row of
   * dead-owner.c++ applies mid-contention too. The event log's silence
   * after the writers' last unlock is the strand signature. */
  const bool a_stranded = WIFSIGNALED(status_a) && (WTERMSIG(status_a) == SIGALRM || WTERMSIG(status_a) == SIGABRT);
  const bool b_stranded = WIFSIGNALED(status_b) && (WTERMSIG(status_b) == SIGALRM || WTERMSIG(status_b) == SIGABRT);
  EXPECT_TRUE(a_stranded && b_stranded) << "the writers must strand on the dead-owned lock, got " << a_stranded
                                        << b_stranded << a_exited << b_exited;
  EXPECT_FALSE(a_exited || b_exited) << "the writers must not complete past the dead holder";
#else
  /* The dead-owner recovery modes (robust POSIX-2008 mutexes, System V
   * SEM_UNDO): both writers run to the closing transaction past the killed
   * holder. */
  EXPECT_TRUE(a_exited && b_exited) << "both writers must complete past the killed holder, got exits " << a_exited
                                    << b_exited;
  if (a_exited && b_exited) {
    unsigned a_txns = 0, b_txns = 0;
    bool a_recovered = false, b_recovered = false;
    EXPECT_TRUE(parse_writer_status(kStatusA, a_txns, a_recovered)) << "writer-a summary missing";
    EXPECT_TRUE(parse_writer_status(kStatusB, b_txns, b_recovered)) << "writer-b summary missing";
    EXPECT_GE(a_txns, 1u) << "writer-a must have committed past the contention";
    EXPECT_GE(b_txns, 1u) << "writer-b must have committed past the contention";
#if defined(MDBX_PROBES) && MDBX_LOCKING == MDBX_LOCKING_POSIX2008
    /* The first acquisition of the dead-owned mutex must report the
     * dead-owner recovery (MDBX_RESULT_TRUE) - only the System V SEM_UNDO
     * recovery is invisible at the probe level (no EOWNERDEAD there). */
    EXPECT_TRUE(a_recovered || b_recovered) << "the first acquisition past the kill must report the dead-owner "
                                               "recovery, got "
                                            << a_recovered << b_recovered;
#endif /* MDBX_PROBES && MDBX_LOCKING == MDBX_LOCKING_POSIX2008 */
  }
#endif /* MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX1988 */

  /* The parent's verification: the recovered state is usable for reading and
   * writing, and the parent's own lock-lifecycle ops land in the event log
   * through the same polls. */
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS) << "the fresh opener must recover";
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  log_lock_events("parent", "read-begin", nullptr);
  ASSERT_EQ(mdbx_txn_abort(txn), MDBX_SUCCESS);
  log_lock_events("parent", "read-end", nullptr);
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  log_lock_events("parent", "write-begin", nullptr);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  log_lock_events("parent", "write-end", nullptr);
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
  log_lock_events("parent", "verified", nullptr);

#if defined(MDBX_PROBES)
  /* The instrumentation plumbing self-check: the sites fired by this
   * process' own operations are registered and listable (the sites register
   * lazily on the first fire, so the listing follows the parent's own
   * transactions above). The forkswallow site is registered in whichever
   * process actually swallows a forked child's failed unlock - an accident
   * that only real incidents and a guard-bypassing reproduction hit - so it
   * is not assertable here, only the swallow counter is polled. */
  char reply[2048];
  ASSERT_EQ(mdbx_probe_ctl("list", reply, sizeof reply), 0);
  EXPECT_NE(strstr(reply, "ipclock_lock_result"), nullptr) << reply;
  EXPECT_NE(strstr(reply, "ipclock_unlock_result"), nullptr) << reply;
#endif /* MDBX_PROBES */
}
