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

#include <cstdio>
#include <cstdlib>
#include <string>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/336
// On Windows, mdbx_env_delete() opened the files for the "unused" check with
// DELETE access but without data access, then locked them with LockFileEx(),
// which needs data access: MDBX_ENV_ENSURE_UNUSED and MDBX_ENV_WAIT_FOR_UNUSED
// failed with ERROR_ACCESS_DENIED on a database nobody used, and while another
// process used it WAIT_FOR_UNUSED failed at once with a sharing violation
// instead of waiting. The files are opened with read access now.
//
// Five cases: ENSURE_UNUSED and WAIT_FOR_UNUSED on an unused database must
// delete both files; WAIT_FOR_UNUSED while another process keeps the
// environment open for 1 s must return only after that process started closing
// it, and delete both files; ENSURE_UNUSED while another process uses it must
// return MDBX_BUSY and keep the files; ENSURE_UNUSED on an unused environment in
// a directory must delete the directory too. The other process is this binary,
// run into the disabled holder test. Windows only.

namespace {

const char *const kDb = "issue336.mdbx";
const std::string kLck = std::string(kDb) + MDBX_LOCK_SUFFIX;
const char *const kDir = "issue336-dir";
const char *const kHolderVar = "MDBX_ISSUE336_HOLDER";

/* Deletes the databases before the test and on every exit path. */
struct files_guard {
  files_guard() { remove_all(); }
  ~files_guard() { remove_all(); }
  static void remove_all() {
    mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);
    mdbx_env_delete(kDir, MDBX_ENV_JUST_DELETE);
  }
};

bool exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

void create_db(const char *path = kDb, MDBX_env_flags_t flags = MDBX_NOSUBDIR) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  int rc = mdbx_env_open(env, path, flags, 0644);
  MDBX_txn *txn = nullptr;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
  ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);
  ASSERT_TRUE(exists(path));
}

std::string event_name(const char *what, DWORD pid) {
  return std::string("Local\\mdbx-issue336-") + what + "-" + std::to_string(pid);
}

/* Another process that opens the environment, signals "ready", keeps it open until "stop" is signalled or `hold_ms`
 * have passed, then signals "closing" and closes it. */
struct holder {
  PROCESS_INFORMATION pi{};
  HANDLE ready = nullptr, stop = nullptr, closing = nullptr;

  void start(unsigned hold_ms) {
    const DWORD self = GetCurrentProcessId();
    ready = CreateEventA(nullptr, TRUE, FALSE, event_name("ready", self).c_str());
    stop = CreateEventA(nullptr, TRUE, FALSE, event_name("stop", self).c_str());
    closing = CreateEventA(nullptr, TRUE, FALSE, event_name("closing", self).c_str());
    ASSERT_TRUE(ready && stop && closing) << "CreateEventA() failed: " << GetLastError();
    for (HANDLE h : {ready, stop, closing})
      ResetEvent(h);
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    ASSERT_TRUE(n > 0 && n < MAX_PATH) << "GetModuleFileNameA() failed: " << GetLastError();
    std::string cmdline =
        "\"" + std::string(exe) + "\" --gtest_filter=issue_gh0336.DISABLED_holder --gtest_also_run_disabled_tests";
    const std::string arg = std::to_string(self) + " " + std::to_string(hold_ms);
    ASSERT_TRUE(SetEnvironmentVariableA(kHolderVar, arg.c_str()));
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    const BOOL started = CreateProcessA(exe, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    const DWORD start_error = GetLastError();
    SetEnvironmentVariableA(kHolderVar, nullptr);
    ASSERT_TRUE(started) << "CreateProcessA() failed: " << start_error;
    const HANDLE ready_or_gone[2] = {ready, pi.hProcess};
    const DWORD got = WaitForMultipleObjects(2, ready_or_gone, FALSE, 10000);
    if (got == WAIT_OBJECT_0 + 1) {
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      FAIL() << "the holder ended before opening the environment, exit code " << code;
    }
    ASSERT_EQ(got, WAIT_OBJECT_0) << "the holder did not open the environment within 10 s";
  }

  /* Lets the holder close the environment and waits for it; returns its exit code. */
  DWORD finish() {
    DWORD code = ~0u;
    if (stop)
      SetEvent(stop);
    if (pi.hProcess) {
      if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, INFINITE);
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      pi = PROCESS_INFORMATION{};
    }
    for (HANDLE *h : {&ready, &stop, &closing})
      if (*h) {
        CloseHandle(*h);
        *h = nullptr;
      }
    return code;
  }

  ~holder() { finish(); }
};

} // namespace

TEST(issue_gh0336, delete_modes_for_unused_databases) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  files_guard files;

  /* 1, 2: nobody uses the database */
  for (const MDBX_env_delete_mode_t mode : {MDBX_ENV_ENSURE_UNUSED, MDBX_ENV_WAIT_FOR_UNUSED}) {
    create_db();
    if (HasFatalFailure())
      return;
    const int rc = mdbx_env_delete(kDb, mode);
    EXPECT_EQ(rc, MDBX_SUCCESS) << "mode " << mode << ": " << mdbx_strerror(rc);
    EXPECT_FALSE(exists(kDb)) << "mode " << mode << ": the database is still there";
    EXPECT_FALSE(exists(kLck.c_str())) << "mode " << mode << ": the lock file is still there";
    mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);
  }

  /* 3: WAIT_FOR_UNUSED while another process keeps the environment open for 1 s */
  {
    create_db();
    if (HasFatalFailure())
      return;
    holder other;
    other.start(1000);
    if (HasFatalFailure())
      return;
    const int rc = mdbx_env_delete(kDb, MDBX_ENV_WAIT_FOR_UNUSED);
    const bool waited = WaitForSingleObject(other.closing, 0) == WAIT_OBJECT_0;
    EXPECT_EQ(other.finish(), 0u) << "the holder failed";
    EXPECT_EQ(rc, MDBX_SUCCESS) << "WAIT_FOR_UNUSED while in use: " << mdbx_strerror(rc);
    EXPECT_TRUE(waited) << "WAIT_FOR_UNUSED returned before the other process closed the environment";
    EXPECT_FALSE(exists(kDb)) << "WAIT_FOR_UNUSED: the database is still there";
    EXPECT_FALSE(exists(kLck.c_str())) << "WAIT_FOR_UNUSED: the lock file is still there";
    mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);
  }

  /* 4: ENSURE_UNUSED while another process uses the environment */
  {
    create_db();
    if (HasFatalFailure())
      return;
    holder other;
    other.start(10000);
    if (HasFatalFailure())
      return;
    const int rc = mdbx_env_delete(kDb, MDBX_ENV_ENSURE_UNUSED);
    const bool kept = exists(kDb);
    EXPECT_EQ(other.finish(), 0u) << "the holder failed";
    EXPECT_EQ(rc, MDBX_BUSY) << "ENSURE_UNUSED while in use: " << mdbx_strerror(rc);
    EXPECT_TRUE(kept) << "ENSURE_UNUSED deleted a database in use";
    mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE);
  }

  /* 5: ENSURE_UNUSED on an unused environment in a directory: the directory goes too */
  {
    CreateDirectoryA(kDir, nullptr);
    create_db(kDir, MDBX_ENV_DEFAULTS);
    if (HasFatalFailure())
      return;
    const int rc = mdbx_env_delete(kDir, MDBX_ENV_ENSURE_UNUSED);
    EXPECT_EQ(rc, MDBX_SUCCESS) << "ENSURE_UNUSED on a directory: " << mdbx_strerror(rc);
    EXPECT_FALSE(exists(kDir)) << "ENSURE_UNUSED on a directory: the directory is still there";
  }
}

/* The holder: opens the environment, signals "ready", waits for "stop" or the given time, closes. */
TEST(issue_gh0336, DISABLED_holder) {
  char arg[64];
  if (GetEnvironmentVariableA(kHolderVar, arg, sizeof(arg)) == 0)
    GTEST_SKIP() << "run by issue_gh0336.delete_modes_for_unused_databases only";
  unsigned long parent = 0, hold_ms = 0;
  ASSERT_EQ(sscanf(arg, "%lu %lu", &parent, &hold_ms), 2) << arg;
  const HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name("ready", parent).c_str());
  const HANDLE stop = OpenEventA(SYNCHRONIZE, FALSE, event_name("stop", parent).c_str());
  const HANDLE closing = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name("closing", parent).c_str());
  ASSERT_TRUE(ready && stop && closing) << "OpenEventA() failed: " << GetLastError();

  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  SetEvent(ready);
  WaitForSingleObject(stop, hold_ms);
  SetEvent(closing);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
  for (HANDLE h : {ready, stop, closing})
    CloseHandle(h);
}
