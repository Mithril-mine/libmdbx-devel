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

#if !defined(_WIN32)

TEST(issue_gh0332, failed_reader_lock_does_not_block_other_threads) {
  GTEST_SKIP() << "Windows only: src/lck-windows.c";
}

#else /* _WIN32 */

#include <cstdio>
#include <cstring>
#include <string>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/332
// On Windows, when the file lock in lck_rdt_lock() failed, the function returned
// the error but kept env->lck_event_cs entered, so every other thread of the
// process that started a read transaction (or took the readers-table lock
// otherwise) blocked forever.
//
// While the flock_ex_error FAULT site is armed (until "fault ... none"), every
// LockFileEx() request fails as an I/O error on the lock file would
// (ERROR_IO_DEVICE); it is armed around one read-transaction begin only. The
// scenario runs in a child process (this binary, the disabled child test),
// because without the fix a thread of it blocks forever: the parent kills the
// child after a timeout and deletes the database files in every case. The child
// reports its own exit codes, so a crash, an empty test filter or a lost fault
// path cannot pass. Requires MDBX_PROBES=ON and MDBX_PROBES=1; skips on other
// platforms.

namespace {

const char *const kDb = "issue332.mdbx";
const char *const kChildVar = "MDBX_ISSUE332_CHILD";
constexpr DWORD kChildTimeoutMs = 30000;

/* Exit codes of the child; none of them can come from a crash, abort() or gtest itself. */
constexpr UINT kChildPassed = 0x3320, kChildHung = 0x3321, kSiteNotReached = 0x3322;

/* Returns the hits of the FAULT site, or 0 when it was never reached. */
size_t site_hits() {
  char reply[512];
  if (mdbx_probe_ctl("query flock_ex_error", reply, sizeof reply) != 0)
    return 0;
  const char *const site = strstr(reply, "site ");
  char name[64];
  unsigned kind, armed;
  size_t seen, hits;
  return (site && sscanf(site, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, &hits) == 5) ? hits : 0;
}

DWORD WINAPI other_reader(LPVOID arg) {
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(static_cast<MDBX_env *>(arg), nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS)
    mdbx_txn_abort(txn);
  return static_cast<DWORD>(rc);
}

[[noreturn]] void child_exit(UINT code) {
  fflush(nullptr);
  TerminateProcess(GetCurrentProcess(), code);
  ExitProcess(code);
}

} // namespace

TEST(issue_gh0332, failed_reader_lock_does_not_block_other_threads) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  char reply[256];
  if (mdbx_probe_ctl("sync", reply, sizeof reply) != 0)
    GTEST_SKIP() << "the probe bus is not active: " << reply;
  mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);

  char exe[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
  ASSERT_TRUE(n > 0 && n < MAX_PATH) << "GetModuleFileNameA() failed: " << GetLastError();
  std::string cmdline =
      "\"" + std::string(exe) + "\" --gtest_filter=issue_gh0332.DISABLED_child --gtest_also_run_disabled_tests";
  ASSERT_TRUE(SetEnvironmentVariableA(kChildVar, "1"));
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  /* the handles are inherited, so the child's output goes to the same place as ours */
  const BOOL started = CreateProcessA(exe, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
  const DWORD start_error = GetLastError();
  SetEnvironmentVariableA(kChildVar, nullptr);
  ASSERT_TRUE(started) << "CreateProcessA() failed: " << start_error;

  const bool hung = WaitForSingleObject(pi.hProcess, kChildTimeoutMs) == WAIT_TIMEOUT;
  if (hung) {
    TerminateProcess(pi.hProcess, kChildHung);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);

  if (hung)
    ADD_FAILURE() << "the child did not finish within " << kChildTimeoutMs / 1000 << " s";
  else if (code == kChildHung)
    ADD_FAILURE() << "another thread blocked in its read transaction: lck_event_cs was not released";
  else if (code == kSiteNotReached)
    ADD_FAILURE() << "the read transaction did not reach the flock_ex_error site: the test lost its coverage";
  else
    EXPECT_EQ(code, kChildPassed) << "the child failed (exit code 0x" << std::hex << code << "), see its output above";
}

/* The child: a reader-table lock fails in this thread, then another thread must start a read transaction. */
TEST(issue_gh0332, DISABLED_child) {
  char flag[8];
  if (GetEnvironmentVariableA(kChildVar, flag, sizeof(flag)) == 0)
    GTEST_SKIP() << "run by issue_gh0332.failed_reader_lock_does_not_block_other_threads only";

  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  char reply[512];
  ASSERT_EQ(0, mdbx_probe_ctl("reset flock_ex_error", reply, sizeof reply)) << reply;
  ASSERT_EQ(0, mdbx_probe_ctl("fault flock_ex_error 1117" /* ERROR_IO_DEVICE */, reply, sizeof reply)) << reply;
  MDBX_txn *txn = nullptr;
  int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn); /* takes the readers-table lock for its slot */
  mdbx_probe_ctl("fault flock_ex_error none", reply, sizeof reply);
  if (site_hits() == 0)
    child_exit(kSiteNotReached);
  if (rc == MDBX_SUCCESS)
    mdbx_txn_abort(txn);
  EXPECT_NE(rc, MDBX_SUCCESS) << "the injected lock failure did not reach mdbx_txn_begin()";

  HANDLE thread = CreateThread(nullptr, 0, other_reader, env, 0, nullptr);
  ASSERT_NE(thread, nullptr);
  if (WaitForSingleObject(thread, 10000) == WAIT_TIMEOUT) {
    fprintf(stderr, "another thread is still blocked in mdbx_txn_begin() after 10 s\n");
    child_exit(kChildHung); /* the blocked thread cannot be stopped, so the environment is not closed */
  }
  DWORD other_rc = 0;
  GetExitCodeThread(thread, &other_rc);
  CloseHandle(thread);
  EXPECT_EQ(static_cast<int>(other_rc), MDBX_SUCCESS) << mdbx_strerror(static_cast<int>(other_rc));

  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  EXPECT_EQ(rc, MDBX_SUCCESS) << "this thread again: " << mdbx_strerror(rc);
  if (rc == MDBX_SUCCESS)
    mdbx_txn_abort(txn);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
  if (!HasFailure())
    child_exit(kChildPassed);
}

#endif /* _WIN32 */
