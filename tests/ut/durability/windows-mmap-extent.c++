/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but
/// not limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/// The reproducer for #549 (windows-readonly-mmap-extent-fixed-at-open): on
/// native Windows the RDONLY mapping extent is pinned to the file size at the
/// moment of the environment open. The section is created by NtCreateSection()
/// with MaximumSize = the open-time size and the view is mapped with
/// ViewSize = 0, i.e. the whole section - so map->limit collapses to the
/// open-time size. The POSIX counterpart maps the full geometry upper limit
/// and picks the growth up in place (within map->limit, no un-map), so the
/// scenario below is Windows-only by construction.
///
/// When the writer grows the database well beyond the reader's open-time
/// extent, a fresh reader's snapshot needs pages beyond that extent:
/// txn_setup_primal() calls dxb_resize() for the read transaction without the
/// un-map permission (readers may not un-map the mapping), and the Windows
/// osal_mresize() falls into its "cannot resize without un-map" tail which
/// returns exactly MDBX_EPERM. The other MDBX_UNABLE_EXTEND_MAPSIZE
/// alternatives of that path (the address-space probe, the contended
/// exclusive remap-lock) do not apply to this scenario.
///
/// The writer and the reader run in two processes: libmdbx refuses a second
/// environment handle for the same environment within one process with
/// MDBX_BUSY (the in-process neighbor check in lck_setup(), unless the
/// legacy MDBX_DBG_LEGACY_MULTIOPEN debug mode is enabled), regardless of
/// the open order and the flags - so no same-process arrangement (neither
/// the co-opened reader nor the reader-opened-first reorder) can reach the
/// defect point. The reader is this test binary re-invoked for the
/// DISABLED_reader_role test with a marker environment variable, the
/// CreateProcessA + named-events pattern of issue_gh0336.c++: the parent
/// commits the known initial data, spawns the reader (which opens its RDONLY
/// environment over the small file and signals ready), commits the growth,
/// signals go, and verifies the child's exit code.
///
/// The assertion is the fixed behavior: the reader's fresh transaction begins
/// and returns the grown data. With the defect present mdbx_txn_begin() fails
/// with MDBX_EPERM - i.e. the test is red on Windows while the defect exists
/// (this is the confirmation instrument for #549) and turns green once the
/// RDONLY extent follows the file growth. Non-Windows platforms skip: there
/// the reader's extent covers the growth from the start and the scenario
/// cannot reproduce anything.

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const char *const kDb = "windows-mmap-extent.mdbx";

/* The writer's geometry is fixed explicitly so the scenario does not depend
 * on the default upper limit: the upper stays well above the growth, hence
 * the reader's dxb_resize() is driven purely by `now` growing beyond the
 * reader's open-time extent. */
MDBX_MAYBE_UNUSED constexpr size_t kUpperBytes = size_t(64) * 1024 * 1024;

/* The growth applied after the reader's open: several MB of bulk data, the
 * record count and the value size are fixed (not page-derived), so the
 * runtime does not vary with the page size (see tests/AGENTS.md). */
constexpr size_t kGrowthBytes = size_t(8) * 1024 * 1024;
constexpr size_t kValueBytes = 16 * 1024;
MDBX_MAYBE_UNUSED constexpr size_t kGrowthRecords = kGrowthBytes / kValueBytes;

MDBX_MAYBE_UNUSED void put_record(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const void *value, size_t value_size) {
  MDBX_val kkey;
  kkey.iov_base = const_cast<void *>(static_cast<const void *>(key));
  kkey.iov_len = strlen(key);
  MDBX_val vval;
  vval.iov_base = const_cast<void *>(static_cast<const void *>(value));
  vval.iov_len = value_size;
  ASSERT_EQ(mdbx_put(txn, dbi, &kkey, &vval, MDBX_UPSERT), MDBX_SUCCESS) << "put `" << key << "` failed";
}

/* The growth records: key `grown-NNNN`, value filled with the record's
 * pattern byte, so the read-back is verifiable. */
MDBX_MAYBE_UNUSED void make_growth_record(size_t index, char *key_buf, size_t key_buf_size, void *value_buf) {
  snprintf(key_buf, key_buf_size, "grown-%06zu", index);
  memset(value_buf, static_cast<int>(index & 0xFF) + 1, kValueBytes);
}

#if defined(_WIN32)

/* The marker env-var for the spawned reader role: the child gets a COPY of
 * the parent's environment at CreateProcess time, its presence enables the
 * DISABLED_reader_role test below and its value is the parent's pid (the
 * named events are scoped by it). */
constexpr char kReaderRoleVar[] = "MDBX_WINDOWS_MMAP_EXTENT_READER_ROLE";
constexpr char kReaderRoleFilter[] = "windows_mmap_extent.DISABLED_reader_role";

/* The choreography's watchdog: a loaded CI runner must still fit, while a
 * hung child is reaped well within the CTest TIMEOUT (test-timeouts.cmake). */
constexpr DWORD kSyncTimeoutMs = 15000;

std::string event_name(const char *what, unsigned long pid) {
  return std::string("Local\\mdbx-windows-mmap-extent-") + what + "-" + std::to_string(pid);
}

/* The spawned reader role (the issue_gh0336.c++ pattern). start() creates the
 * named events, spawns this binary for the DISABLED_reader_role test and
 * waits for the child's RDONLY environment open. finish() signals `go` (the
 * parent's growth commit is done), reaps the child and returns its exit code.
 * The destructor runs finish() on every parent exit path, so the child never
 * outlives the test and the file-guard's delete never races a still-mapped
 * process (on Windows a mapped file cannot be deleted). */
struct reader_role {
  PROCESS_INFORMATION pi{};
  HANDLE ready = nullptr, go = nullptr;

  void start() {
    const unsigned long self = GetCurrentProcessId();
    ready = CreateEventA(nullptr, TRUE, FALSE, event_name("ready", self).c_str());
    go = CreateEventA(nullptr, TRUE, FALSE, event_name("go", self).c_str());
    ASSERT_TRUE(ready && go) << "CreateEventA() failed: " << GetLastError();
    ResetEvent(ready);
    ResetEvent(go);
    char exe[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    ASSERT_TRUE(n > 0 && n < MAX_PATH) << "GetModuleFileNameA() failed: " << GetLastError();
    /* non-const: CreateProcessA takes a mutable LPSTR and may rewrite it */
    std::string cmdline =
        "\"" + std::string(exe) + "\" --gtest_filter=" + kReaderRoleFilter + " --gtest_also_run_disabled_tests";
    ASSERT_TRUE(SetEnvironmentVariableA(kReaderRoleVar, std::to_string(self).c_str()));
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    const BOOL started = CreateProcessA(exe, &cmdline[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    const DWORD start_error = GetLastError();
    SetEnvironmentVariableA(kReaderRoleVar, nullptr);
    ASSERT_TRUE(started) << "CreateProcessA() failed: " << start_error;
    const HANDLE ready_or_gone[2] = {ready, pi.hProcess};
    const DWORD got = WaitForMultipleObjects(2, ready_or_gone, FALSE, kSyncTimeoutMs);
    if (got == WAIT_OBJECT_0 + 1) {
      DWORD code = 0;
      GetExitCodeProcess(pi.hProcess, &code);
      FAIL() << "the reader ended before opening the environment, exit code " << code;
    }
    ASSERT_EQ(got, WAIT_OBJECT_0) << "the reader did not open the environment within " << kSyncTimeoutMs << " ms";
  }

  DWORD finish() {
    DWORD code = ~0u;
    if (go)
      SetEvent(go);
    if (pi.hProcess) {
      if (WaitForSingleObject(pi.hProcess, kSyncTimeoutMs) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, INFINITE);
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      pi = PROCESS_INFORMATION{};
    }
    for (HANDLE *h : {&ready, &go})
      if (*h) {
        CloseHandle(*h);
        *h = nullptr;
      }
    return code;
  }

  ~reader_role() { finish(); }
};

#endif /* _WIN32 */

} // namespace

/* The reader role: this binary re-invoked by the parent test below (the
 * marker environment variable is the guard against a standalone run, e.g. a
 * --gtest_also_run_disabled_tests corpus sweep). The test owns NO database
 * files - the parent's env_files_guard does (see tests/AGENTS.md), the child
 * only opens and closes its environment. */
TEST(windows_mmap_extent, DISABLED_reader_role) {
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only reproducer";
#else
  char arg[64];
  if (GetEnvironmentVariableA(kReaderRoleVar, arg, sizeof(arg)) == 0)
    GTEST_SKIP() << "run by windows_mmap_extent.reader_picks_up_growth_beyond_open_time_extent only";
  unsigned long parent_pid = 0;
  ASSERT_EQ(sscanf(arg, "%lu", &parent_pid), 1) << arg;
  const HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name("ready", parent_pid).c_str());
  const HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, event_name("go", parent_pid).c_str());
  ASSERT_TRUE(ready && go) << "OpenEventA() failed: " << GetLastError();

  /* The RDONLY open pins this process' mapping extent to the current, small
   * file size - the moment of #549 windows-readonly-mmap-extent-fixed-at-open. */
  MDBX_env *reader = nullptr;
  env_handle_guard reader_guard(&reader);
  ASSERT_EQ(mdbx_env_create(&reader), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(reader, kDb, MDBX_NOSUBDIR | MDBX_RDONLY, 0), MDBX_SUCCESS);
  ASSERT_TRUE(SetEvent(ready));

  /* The parent's growth commit; bounded by the watchdog timeout. */
  ASSERT_EQ(WaitForSingleObject(go, kSyncTimeoutMs), WAIT_OBJECT_0)
      << "the parent did not commit the growth within " << kSyncTimeoutMs << " ms";

  /* The assertion of #549: the reader begins a fresh transaction over the
   * grown database, whose required pages are beyond its open-time extent.
   * With the defect present mdbx_txn_begin() fails with MDBX_EPERM - the
   * exact code returned by the Windows osal_mresize() "cannot resize without
   * un-map" tail, since dxb_resize() is called here without the un-map
   * permission and neither the address-space probe nor the remap-lock
   * contention (the MDBX_UNABLE_EXTEND_MAPSIZE alternatives) applies.
   * With the defect absent the transaction begins and the reads below return
   * the grown data. */
  MDBX_txn *rtxn = nullptr;
  txn_handle_guard rtxn_guard(&rtxn);
  const int begin_rc = mdbx_txn_begin(reader, nullptr, MDBX_TXN_RDONLY, &rtxn);
  ASSERT_EQ(begin_rc, MDBX_SUCCESS)
      << "the RDONLY reader must pick up the database growth beyond its open-time extent (#549 "
         "windows-readonly-mmap-extent-fixed-at-open), got "
      << mdbx_strerror(begin_rc) << " (" << begin_rc << "); with the defect present the begin fails with " << MDBX_EPERM
      << " (MDBX_EPERM)";

  /* The grown data is readable through the fresh snapshot. */
  MDBX_dbi reader_dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(rtxn, nullptr, MDBX_DB_ACCEDE, &reader_dbi), MDBX_SUCCESS);
  char key_buf[32];
  const size_t last = kGrowthRecords - 1;
  snprintf(key_buf, sizeof(key_buf), "grown-%06zu", last);
  MDBX_val key;
  key.iov_base = key_buf;
  key.iov_len = strlen(key_buf);
  MDBX_val data;
  ASSERT_EQ(mdbx_get(rtxn, reader_dbi, &key, &data), MDBX_SUCCESS);
  ASSERT_EQ(data.iov_len, kValueBytes);
  const auto *bytes = static_cast<const uint8_t *>(data.iov_base);
  ASSERT_EQ(bytes[0], static_cast<uint8_t>((last & 0xFF) + 1));
  ASSERT_EQ(bytes[kValueBytes - 1], static_cast<uint8_t>((last & 0xFF) + 1));

  /* The pre-growth anchor is still readable through the same snapshot. */
  MDBX_val anchor_key;
  anchor_key.iov_base = const_cast<char *>("anchor");
  anchor_key.iov_len = strlen("anchor");
  MDBX_val anchor_data;
  ASSERT_EQ(mdbx_get(rtxn, reader_dbi, &anchor_key, &anchor_data), MDBX_SUCCESS);
  ASSERT_EQ(anchor_data.iov_len, kValueBytes);

  ASSERT_EQ(mdbx_txn_abort(rtxn), MDBX_SUCCESS);
  rtxn_guard.release();
  ASSERT_EQ(mdbx_env_close(reader), MDBX_SUCCESS);
  reader_guard.release();
  CloseHandle(ready);
  CloseHandle(go);
#endif /* _WIN32 */
}

TEST(windows_mmap_extent, reader_picks_up_growth_beyond_open_time_extent) {
  const env_files_guard files({kDb});
#if !defined(_WIN32)
  GTEST_SKIP() << "Windows-only reproducer: the RDONLY mapping extent is pinned to the open-time file size "
                  "(NtCreateSection MaximumSize), on POSIX the mapping covers the geometry upper limit from the start";
#else
  /* The writer: the known initial data, committed before the reader's open,
   * so no write transaction is active at the reader's open moment (the
   * real-world pattern of #549). */
  MDBX_env *writer = nullptr;
  env_handle_guard writer_guard(&writer);
  ASSERT_EQ(mdbx_env_create(&writer), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(writer, -1, -1, kUpperBytes, -1, -1, -1), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(writer, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  MDBX_dbi writer_dbi = 0;
  MDBX_txn *wtxn = nullptr;
  {
    txn_handle_guard wtxn_guard(&wtxn);
    ASSERT_EQ(mdbx_txn_begin(writer, nullptr, MDBX_TXN_READWRITE, &wtxn), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_dbi_open(wtxn, nullptr, MDBX_DB_DEFAULTS, &writer_dbi), MDBX_SUCCESS);
    char value_buf[kValueBytes];
    memset(value_buf, 'a', sizeof(value_buf));
    put_record(wtxn, writer_dbi, "anchor", value_buf, sizeof(value_buf));
    ASSERT_EQ(mdbx_txn_commit(wtxn), MDBX_SUCCESS);
    wtxn_guard.release();
  }

  /* The reader process over the small file; returns only after the child has
   * opened its RDONLY environment (its mapping extent is pinned by then). */
  reader_role reader;
  reader.start();
  if (HasFatalFailure())
    return;

  /* The growth from the writer side, well beyond the reader's extent. */
  {
    txn_handle_guard wtxn_guard(&wtxn);
    ASSERT_EQ(mdbx_txn_begin(writer, nullptr, MDBX_TXN_READWRITE, &wtxn), MDBX_SUCCESS);
    char key_buf[32];
    char *value_buf = new char[kValueBytes];
    for (size_t i = 0; i < kGrowthRecords; ++i) {
      make_growth_record(i, key_buf, sizeof(key_buf), value_buf);
      put_record(wtxn, writer_dbi, key_buf, value_buf, kValueBytes);
    }
    delete[] value_buf;
    ASSERT_EQ(mdbx_txn_commit(wtxn), MDBX_SUCCESS);
    wtxn_guard.release();
  }

  /* Let the child begin its fresh read transaction (the #549 defect point)
   * and take its verdict: exit code 0 iff the transaction began and returned
   * the grown data; the child's gtest output goes to the inherited stdout. */
  EXPECT_EQ(reader.finish(), 0u) << "the reader role failed";
#endif /* _WIN32 */
}
