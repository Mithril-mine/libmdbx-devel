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

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"
#include "tool_runner.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Issue #217: the Windows datafile-shrink throttle (added for #170,
// MDBX_opt_shrink_retry_period) must rate-limit only *doomed* attempts —
// those skipped because another process keeps the datafile mapped.
//
//  1. single_process_shrink_not_throttled: with no other process and no pinned
//     reader in this process, a shrink-warranted commit must shrink the file
//     immediately, even when consecutive shrink-warranted commits would
//     normally fall inside the retry period.
//
//  2. foreign_read_process_skips_shrink: a live read transaction in ANOTHER
//     process (a child spawned via tool_runner) puts a foreign pid into the
//     shared reader table, so the parent's shrink-warranted commit must be
//     deferred (the file must stay large) while the child holds the reader,
//     and must shrink once the child is gone.
//
// Windows-only: on POSIX dxb_shrink_attempt_allowed() is a no-op and the
// shrink is never gated by exclusivity (see src/lck-windows.c / src/dxb.c).

#if defined(_WIN32) || defined(_WIN64)

namespace {

const char *const kTestPathFp = "issue217-foreign.mdbx";

void env_remove(const char *path) {
  try {
    mdbx::env::remove(mdbx::path(path));
  } catch (const mdbx::error &) {
    /* not required to exist beforehand */
  }
}

bool commit_put(MDBX_env *env, MDBX_dbi dbi, const char *key) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn) != MDBX_SUCCESS)
    return false;
  MDBX_val k = {const_cast<char *>(key), std::strlen(key)}, v = {const_cast<char *>("v"), 1};
  if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

uint64_t dxb_fsize(MDBX_env *env) {
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  if (mdbx_env_info_ex(env, nullptr, &info, sizeof info) != MDBX_SUCCESS)
    return 0;
  return info.mi_dxb_fsize;
}

/* Open a fresh env on `path`, create the default table and force an implicit
 * shrink to become warranted: the physical file is explicitly grown far beyond
 * the data extent. The file size afterwards is > 4 MiB. */
MDBX_env *open_with_shrink_warranted(const char *path, uint32_t flags, MDBX_dbi *dbi) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  mdbx_env_set_maxdbs(env, 4);
  mdbx_env_set_geometry(env, 256 << 10, -1, 64 << 20, 64 << 10, 64 << 10, -1);
  if (mdbx_env_open(env, path, (MDBX_env_flags_t)(MDBX_NOSUBDIR | flags), 0644) != MDBX_SUCCESS)
    goto fail;
  {
    MDBX_txn *txn = nullptr;
    if (mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn) != MDBX_SUCCESS)
      goto fail;
    if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, dbi) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      goto fail;
    }
    if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
      goto fail;
  }
  if (!commit_put(env, *dbi, "seed"))
    goto fail;
  if (mdbx_env_set_geometry(env, -1, 8 << 20, -1, -1, -1, -1) != MDBX_SUCCESS)
    goto fail;
  return env;
fail:
  mdbx_env_close(env);
  return nullptr;
}

/* Wait until `env`'s file size drops below `threshold` (the shrink converged)
 * or `budget_ms` elapses. Returns true when it shrunk in time. */
bool wait_shrunk(MDBX_env *env, uint64_t threshold, int budget_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (dxb_fsize(env) < threshold)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

/* Reader-table slot scan callback: remember whether a slot with a pid other
 * than `my_pid` is present. */
struct foreign_slot_probe {
  mdbx_pid_t my_pid;
  bool foreign_found = false;
};

static int foreign_slot_probe_fn(void *ctx, int num, int slot, mdbx_pid_t pid, mdbx_tid_t thread, uint64_t txnid,
                                 uint64_t lag, size_t bytes_used, size_t bytes_retained) noexcept {
  (void)num;
  (void)slot;
  (void)txnid;
  (void)lag;
  (void)thread;
  (void)bytes_used;
  (void)bytes_retained;
  foreign_slot_probe *probe = static_cast<foreign_slot_probe *>(ctx);
  if (pid && pid != probe->my_pid)
    probe->foreign_found = true;
  return 0;
}

/* Wait until a foreign-process reader slot appears in the shared reader table
 * (i.e. the spawned child has opened the database and begun its read
 * transaction), or `budget_ms` elapses. Returns true when the foreign slot is
 * present.
 *
 * A fixed sleep is NOT enough here: the child must be scheduled, perform the
 * full gtest startup, open its environment and begin the read transaction.
 * On loaded CI runners this takes far longer than the previous 400 ms, and
 * when the parent's shrink-warranted commit runs before the child registered
 * its slot, the shrink is NOT deferred — the file shrinks and the test fails
 * spuriously. */
bool wait_foreign_reader_slot(MDBX_env *env, mdbx_pid_t my_pid, int budget_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    foreign_slot_probe probe = {my_pid, false};
    mdbx_reader_list(env, foreign_slot_probe_fn, &probe);
    if (probe.foreign_found)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

/* Child mode: open the foreign path read-only and hold a read transaction,
 * refreshing it occasionally, for `MDBX_TEST_GH217_STOP` ms (default 3000). */
int foreign_reader_child() {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return 2;
  if (mdbx_env_open(env, kTestPathFp, MDBX_NOSUBDIR | MDBX_RDONLY, 0) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return 2;
  }
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return 2;
  }
  const char *const stop = std::getenv("MDBX_TEST_GH217_STOP");
  unsigned ms = stop ? (unsigned)std::strtoul(stop, nullptr, 10) : 3000;
  const unsigned until_ms = ms;
  unsigned waited = 0;
  while (waited < until_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    waited += 20;
    /* re-issue the read txn to keep it fresh (a reader slot stays occupied). */
    mdbx_txn_abort(txn);
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS) {
      mdbx_env_close(env);
      return 2;
    }
  }
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  return 0;
}

} // namespace

TEST(issue_gh0217, foreign_read_process_skips_shrink) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  if (std::getenv("MDBX_TEST_GH217_CHILD")) {
    /* child: become the foreign reader */
    ASSERT_EQ(0, foreign_reader_child());
    return;
  }
  env_remove(kTestPathFp);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_with_shrink_warranted(kTestPathFp, 0, &dbi);
  ASSERT_NE(nullptr, env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 0 /* no throttle noise */));
  const uint64_t grown = dxb_fsize(env);
  EXPECT_GT(grown, (uint64_t)(4 << 20));

  /* Spawn a foreign reader: an independent process opening the same database
   * read-only and holding a read transaction (so its pid occupies a shared
   * reader-table slot). */
  std::string child = tool_runner::executable_dir();
  child += '/';
  child += "issue_gh0217";
#ifdef _WIN32
  child += ".exe";
#endif
  std::string filter = "--gtest_filter=issue_gh0217.foreign_read_process_skips_shrink";
  std::string child_env = "MDBX_TEST_GH217_CHILD=1", stop_env = "MDBX_TEST_GH217_STOP=3000";
#ifdef _WIN32
  _putenv(child_env.c_str());
  _putenv(stop_env.c_str());
#else
  setenv("MDBX_TEST_GH217_CHILD", "1", 1);
  setenv("MDBX_TEST_GH217_STOP", "3000", 1);
#endif
  const std::vector<std::string> args = {child, filter, "--gtest_brief=1"};

  /* tool_runner::run() blocks until the child exits, so run it in a background
   * thread while the parent performs its commit with the foreign reader alive. */
  int child_rc = -1;
  std::thread child_thread([&]() { child_rc = tool_runner::run(args); });

  /* Wait until the child actually registers its reader slot in the shared
   * reader table (it spins for ~3 s once started). A fixed sleep is not
   * enough: on loaded CI runners the child's process spawn + gtest startup +
   * env open can easily exceed 400 ms, and then the shrink-warranted commit
   * below runs with NO foreign reader registered — the shrink succeeds and
   * the test fails spuriously (observed on win-msvc/win-cxx CI cells). */
  const mdbx_pid_t my_pid = GetCurrentProcessId();
  ASSERT_TRUE(wait_foreign_reader_slot(env, my_pid, 30000))
      << "the foreign reader did not register its slot in time (child startup is too slow or it died)";

  /* The parent's shrink-warranted commit while the foreign reader is alive
   * must be deferred: the file must NOT shrink. */
  ASSERT_TRUE(commit_put(env, dbi, "while-foreign"));
  EXPECT_GE(dxb_fsize(env), grown) << "a shrink must be deferred while a foreign process keeps the file mapped";

  /* Wait for the child to finish (it stops its reader and exits). */
  child_thread.join();
  EXPECT_EQ(0, child_rc) << "foreign reader child must exit cleanly";

#ifdef _WIN32
  _putenv("MDBX_TEST_GH217_CHILD=");
  _putenv("MDBX_TEST_GH217_STOP=");
#else
  unsetenv("MDBX_TEST_GH217_CHILD");
  unsetenv("MDBX_TEST_GH217_STOP");
#endif

  /* After the foreign reader is gone, a subsequent shrink-warranted commit
   * must shrink the file (the throttle, if any, is ineffective now). */
  ASSERT_TRUE(commit_put(env, dbi, "after-foreign"));
  EXPECT_TRUE(wait_shrunk(env, grown, 5000)) << "the shrink must converge once the foreign process is gone";

  mdbx_env_close(env);
  env_remove(kTestPathFp);
}

#else
TEST(issue_gh0217, windows_only) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* The Windows-only shrink-exclusivity gate does not exist on POSIX
   * (dxb_shrink_attempt_allowed() always allows), so there is nothing to test. */
  GTEST_SKIP() << "shrink-attempt throttling is Windows-only";
}
#endif /* Windows */