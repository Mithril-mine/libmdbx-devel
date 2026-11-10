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

#include "../../../src/internals.h"
#include "../env-guard.h++"
#include <gtest/gtest.h>

#include "../test-env.h"
#include "tool_runner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/334 (read-only part)
//
// In a process that opened the database with MDBX_RDONLY, osal_mresize() (POSIX) recorded the size of the database,
// clamped to the NEW upper bound, before it tried to grow the mapping. When the growth failed, the recorded size
// stayed larger than the mapping: the first read transaction failed with MDBX_UNABLE_EXTEND_MAPSIZE, the next one
// found the recorded size large enough, succeeded and read past the end of the mapping (Debug builds abort on the
// size/limit assertion of dxb_resize()).
//
// The test opens the database read-only, lets a child process (the same binary) raise the upper bound and grow the
// database to about twice the read-only mapping, and reserves the address range right after the mapping, so the
// mapping cannot grow in place (the usual case on Linux, where the mapping lies right below other mappings). Every
// read transaction must then either fail or see a mapping as large as the recorded size. On Windows the growth is
// refused before the size is recorded, so the test is a sanity check there.

namespace {

constexpr intptr_t kMiB = intptr_t(1) << 20;
constexpr intptr_t kUpper = 16 * kMiB;
constexpr int kRecords = 2000;
const char *const kPath = "issue334.mdbx";
const char *const kChildVar = "MDBX_TEST_GH334_CHILD";
const char kValue[] = "a value of the records written before the database grows";

/* Reserves the address range right after the mapping of the environment until destruction. When the reservation
 * fails, the range is already in use by something else, which blocks the growth in place as well. */
class block_after_mapping {
  void *addr = nullptr;
  size_t len = 0;

public:
  explicit block_after_mapping(const MDBX_env *env) {
    void *const want = static_cast<char *>(env->dxb_mmap.base) + env->dxb_mmap.limit;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    len = si.dwAllocationGranularity;
    addr = VirtualAlloc(want, len, MEM_RESERVE, PAGE_NOACCESS);
#else
    const long page = sysconf(_SC_PAGESIZE);
    len = (page > 0) ? size_t(page) : 4096;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    void *const got = mmap(want, len, PROT_NONE, flags, -1, 0);
    if (got == want)
      addr = got;
    else if (got != MAP_FAILED)
      munmap(got, len);
#endif
  }
  block_after_mapping(const block_after_mapping &) = delete;
  block_after_mapping &operator=(const block_after_mapping &) = delete;
  ~block_after_mapping() {
    if (addr) {
#if defined(_WIN32)
      VirtualFree(addr, 0, MEM_RELEASE);
#else
      munmap(addr, len);
#endif
    }
  }
};

/* Sets the variable that marks the child process, and clears it on every exit path. */
struct child_var_scope {
  static void set(bool on) {
#if defined(_WIN32)
    /* the Win32 environment block, which CreateProcess() passes to the child */
    SetEnvironmentVariableA(kChildVar, on ? "1" : nullptr);
#else
    if (on)
      setenv(kChildVar, "1", 1);
    else
      unsetenv(kChildVar);
#endif
  }
  child_var_scope() { set(true); }
  ~child_var_scope() { set(false); }
  child_var_scope(const child_var_scope &) = delete;
  child_var_scope &operator=(const child_var_scope &) = delete;
};

void create_database() {
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, kMiB, kUpper, kMiB, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPath, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644));
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  for (int i = 0; i < kRecords; ++i) {
    char key[16];
    const int n = std::snprintf(key, sizeof(key), "k%08d", i);
    MDBX_val k = {key, size_t(n)}, v = {const_cast<char *>(kValue), sizeof(kValue)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
  }
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

/* Starts a read transaction; when it succeeds, reads every record, which touches the whole used part of the
 * database. Either way the recorded size must not exceed the mapping. */
void read_all(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  const int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  ASSERT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_UNABLE_EXTEND_MAPSIZE) << rc << " " << mdbx_strerror(rc);
  ASSERT_LE(env->dxb_mmap.current, env->dxb_mmap.limit) << "the recorded size exceeds the mapping";
  if (rc != MDBX_SUCCESS)
    return;
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, MAIN_DBI, &cursor));
  MDBX_val key, value;
  size_t count = 0;
  for (int err = mdbx_cursor_get(cursor, &key, &value, MDBX_FIRST); err == MDBX_SUCCESS;
       err = mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT))
    count += value.iov_len;
  mdbx_cursor_close(cursor);
  EXPECT_GT(count, size_t(0));
}

} // namespace

/* Child process of read_only_size_stays_within_mapping: raises the upper bound and grows the database to about twice
 * the read-only mapping (each value takes five pages). The parent owns the database files. */
TEST(issue_gh0334, DISABLED_grow_database) {
  const char *const spawned = std::getenv(kChildVar);
  if (!spawned || !*spawned) {
    GTEST_SKIP() << "runs only as the child process of read_only_size_stays_within_mapping";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPath, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 4 * kUpper, -1, -1, -1));
  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  /* sized in pages: the element size, not the count, absorbs the page size */
  const std::vector<char> value(size_t(info.mi_dxb_pagesize) * 4, 'g');
  const int count = int(kUpper * 3 / 2 / intptr_t(value.size()));
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  for (int i = 0; i < count; ++i) {
    char key[16];
    const int n = std::snprintf(key, sizeof(key), "g%08d", i);
    MDBX_val k = {key, size_t(n)}, v = {const_cast<char *>(value.data()), value.size()};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
  }
  txn_owner.release();
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

TEST(issue_gh0334, read_only_size_stays_within_mapping) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_NO_FATAL_FAILURE(create_database());

  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0644));
  ASSERT_NO_FATAL_FAILURE(read_all(env));

  /* CTest passes the path of this executable, since executable_dir() falls back to "." where /proc/self/exe is
   * missing (e.g. FreeBSD); a direct run without it looks next to the running executable */
  const char *const self = std::getenv("MDBX_TEST_GH334_SELF");
  std::string child = tool_runner::executable_dir() + "/issue_gh0334";
#if defined(_WIN32)
  child += ".exe";
#endif
  if (self && *self)
    child = self;
  {
    const child_var_scope child_var;
    ASSERT_EQ(0, tool_runner::run({child, "--gtest_filter=issue_gh0334.DISABLED_grow_database",
                                   "--gtest_also_run_disabled_tests", "--gtest_brief=1"}))
        << "the child process failed to grow the database";
  }

  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  ASSERT_GT((info.mi_last_pgno + 1) * info.mi_dxb_pagesize, info.mi_mapsize)
      << "the database did not grow past the read-only mapping";

  const block_after_mapping block(env);
  ASSERT_NO_FATAL_FAILURE(read_all(env));
  ASSERT_NO_FATAL_FAILURE(read_all(env));
}
