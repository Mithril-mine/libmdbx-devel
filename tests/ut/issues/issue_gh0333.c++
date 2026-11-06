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

TEST(issue_gh0333, failed_suspension_resumes_each_thread_once) { GTEST_SKIP() << "Windows only: src/lck-windows.c"; }

#else /* _WIN32 */

#include <cstdio>
#include <cstring>
#include <string>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/333
// On Windows, when suspending the process's threads before a remap failed
// part-way, osal_suspend_threads_before_remap() resumed the threads already
// suspended and closed their handles, but kept their count; dxb_resize() then
// resumed and closed the same handles a second time, which could close a handle
// that meanwhile belonged to something else.
//
// Three threads keep a reader slot each. The suspend_open_thread_error FAULT
// site makes OpenThread() fail once a thread is already suspended, during one
// mdbx_env_set_geometry() with a new upper bound (a remap with the readers
// suspended). The child process (this binary, the disabled child test) enables
// the strict handle checks of Windows, so the second CloseHandle() of a closed
// handle raises an exception instead of failing silently. The parent kills a
// child that hangs and deletes the database files on every path; the child
// reports its own exit codes. Requires MDBX_PROBES=ON and MDBX_PROBES=1; skips on
// other platforms.

namespace {

const char *const kDb = "issue333.mdbx";
const char *const kChildVar = "MDBX_ISSUE333_CHILD";
constexpr DWORD kChildTimeoutMs = 30000;
constexpr int kReaders = 3;

/* Exit codes of the child; none of them can come from a crash, abort() or gtest itself. */
constexpr UINT kChildPassed = 0x3330, kSiteNotReached = 0x3332, kNoStrictHandleChecks = 0x3333;

/* Returns the hits of the FAULT site, or 0 when it was never reached. */
size_t site_hits() {
  char reply[512];
  if (mdbx_probe_ctl("query suspend_open_thread_error", reply, sizeof reply) != 0)
    return 0;
  const char *const site = strstr(reply, "site ");
  char name[64];
  unsigned kind, armed;
  size_t seen, hits;
  return (site && sscanf(site, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, &hits) == 5) ? hits : 0;
}

/* ProcessStrictHandleCheckPolicy with RaiseExceptionOnInvalidHandleReference and
 * HandleExceptionsPermanentlyEnabled; loaded at run time, so that no SDK version is needed. */
bool enable_strict_handle_checks() {
  typedef BOOL(WINAPI * set_policy_t)(int policy, PVOID buffer, SIZE_T length);
  const set_policy_t set_policy = reinterpret_cast<set_policy_t>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessMitigationPolicy")));
  DWORD flags = 1 | 2;
  return set_policy && set_policy(3 /* ProcessStrictHandleCheckPolicy */, &flags, sizeof(flags));
}

struct reader_ctx {
  MDBX_env *env;
  HANDLE ready, stop;
};

/* Keeps a reader slot (a reset read transaction) until `stop` is set. */
DWORD WINAPI reader(LPVOID arg) {
  const reader_ctx *const ctx = static_cast<const reader_ctx *>(arg);
  MDBX_txn *txn = nullptr;
  const int rc = mdbx_txn_begin(ctx->env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS)
    mdbx_txn_reset(txn);
  SetEvent(ctx->ready);
  WaitForSingleObject(ctx->stop, INFINITE);
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

TEST(issue_gh0333, failed_suspension_resumes_each_thread_once) {
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
      "\"" + std::string(exe) + "\" --gtest_filter=issue_gh0333.DISABLED_child --gtest_also_run_disabled_tests";
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
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);

  if (hung)
    ADD_FAILURE() << "the child did not finish within " << kChildTimeoutMs / 1000 << " s";
  else if (code == kSiteNotReached)
    ADD_FAILURE() << "the remap did not reach the suspend_open_thread_error site: the test lost its coverage";
  else if (code == kNoStrictHandleChecks)
    ADD_FAILURE() << "SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy) failed in the child";
  else
    EXPECT_EQ(code, kChildPassed) << "the child failed (exit code 0x" << std::hex << code
                                  << "; 0xc0000008 = a closed handle was used again), see its output above";
}

/* The child: a remap fails while suspending the reader threads, with strict handle checks on. */
TEST(issue_gh0333, DISABLED_child) {
  char flag[8];
  if (GetEnvironmentVariableA(kChildVar, flag, sizeof(flag)) == 0)
    GTEST_SKIP() << "run by issue_gh0333.failed_suspension_resumes_each_thread_once only";
  if (!enable_strict_handle_checks())
    child_exit(kNoStrictHandleChecks);

  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(env, -1, 1 << 20, 64 << 20, -1, -1, -1), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  reader_ctx ctx[kReaders];
  HANDLE threads[kReaders], ready[kReaders];
  const HANDLE stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(stop, nullptr);
  for (int i = 0; i < kReaders; ++i) {
    ready[i] = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    ctx[i] = {env, ready[i], stop};
    threads[i] = CreateThread(nullptr, 0, reader, &ctx[i], 0, nullptr);
    ASSERT_NE(threads[i], nullptr);
  }
  ASSERT_EQ(WaitForMultipleObjects(kReaders, ready, TRUE, 10000), WAIT_OBJECT_0);

  char reply[512];
  ASSERT_EQ(0, mdbx_probe_ctl("reset suspend_open_thread_error", reply, sizeof reply)) << reply;
  ASSERT_EQ(0, mdbx_probe_ctl("fault suspend_open_thread_error 87" /* ERROR_INVALID_PARAMETER */, reply, sizeof reply))
      << reply;
  const int rc = mdbx_env_set_geometry(env, -1, -1, 128 << 20, -1, -1, -1); /* a new upper bound: remap */
  mdbx_probe_ctl("fault suspend_open_thread_error none", reply, sizeof reply);
  if (site_hits() < 2)
    child_exit(kSiteNotReached);
  EXPECT_NE(rc, MDBX_SUCCESS) << "the injected OpenThread() failure did not reach mdbx_env_set_geometry()";

  SetEvent(stop);
  EXPECT_EQ(WaitForMultipleObjects(kReaders, threads, TRUE, 10000), WAIT_OBJECT_0);
  for (int i = 0; i < kReaders; ++i) {
    DWORD reader_rc = 0;
    GetExitCodeThread(threads[i], &reader_rc);
    EXPECT_EQ(static_cast<int>(reader_rc), MDBX_SUCCESS) << "reader " << i;
    CloseHandle(threads[i]);
    CloseHandle(ready[i]);
  }
  CloseHandle(stop);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
  if (!HasFailure())
    child_exit(kChildPassed);
}

#endif /* _WIN32 */
