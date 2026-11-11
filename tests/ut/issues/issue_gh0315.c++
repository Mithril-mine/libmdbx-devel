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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/315
//
// 1. mdbx_env_set_geometry() that changes the upper bound may re-create the mapping at another address. On POSIX the
//    only veto was a reader of this process in ANOTHER thread that is not parked, so a read transaction of the calling
//    thread, a parked read transaction and the caller's own write transaction kept pointers into the old mapping: the
//    next access through a cursor or a value read before the call crashed.
// 2. mdbx_env_warmup() without a transaction took the used size from the newest meta. Another process may have grown
//    the database past this process's mapping (only a transaction start extends the mapping), so warmup touched pages
//    beyond the end of the mapping and crashed.
//
// The tests reserve the address range right after the mapping, so the mapping cannot grow in place: a resize that may
// move the mapping then has to move it, and a touch past the end of the mapping faults. A mapping that must stay in
// place reports MDBX_UNABLE_EXTEND_MAPSIZE instead. Windows checks the range after the view before it unmaps it, so
// there the geometry cases are sanity checks (the resize is refused, the mapping never moves); the warmup case
// reproduces on every platform.

namespace {

constexpr intptr_t kMiB = intptr_t(1) << 20;
constexpr intptr_t kUpper = 16 * kMiB;
constexpr int kRecords = 2000;
/* no durability is needed; the child process of the warmup case must open with the same mode */
constexpr MDBX_env_flags_t kOpenFlags = MDBX_env_flags_t(MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC);
const char kValue[] = "a value read before the resize and dereferenced after it";

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

struct cursor_guard {
  MDBX_cursor *cursor = nullptr;
  ~cursor_guard() {
    if (cursor)
      mdbx_cursor_close(cursor);
  }
};

void open_env(const char *path, intptr_t upper, MDBX_env **env) {
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(*env, -1, kMiB, upper, kMiB, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(*env, path, kOpenFlags, 0644));
}

/* Like open_env(), but creates the datafile notably larger than the data written afterwards and with a
 * fine-grained grow/shrink granularity: the shrink target is aligned up to the growth step (a coarse 1MiB step
 * would align the truncation away), and the shrink predicate requires a gap between the file size and the used
 * size, which appears when the data is deleted below a file that was grown beforehand. */
void open_env_fine(const char *path, intptr_t upper, MDBX_env **env) {
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(*env, -1, 15 * kMiB, upper, intptr_t(1) << 16, intptr_t(1) << 18, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(*env, path, kOpenFlags, 0644));
}

void seed(MDBX_env *env) {
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

/* Opens a cursor in `txn`, positions it at the first record and returns the value it points to. */
void position_first(MDBX_txn *txn, cursor_guard &cursor, MDBX_val *held) {
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, MAIN_DBI, &cursor.cursor));
  MDBX_val key;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor.cursor, &key, held, MDBX_FIRST));
  ASSERT_EQ(sizeof(kValue), held->iov_len);
}

/* Raises the upper bound while `txn` holds the cursor and the value read through it. The mapping must stay where it
 * is, and both must remain usable afterwards. */
void resize_under_live_txn(MDBX_env *env, MDBX_txn *txn, cursor_guard &cursor, const MDBX_val &held, bool park) {
  const block_after_mapping block(env);
  const void *const base = env->dxb_mmap.base;
  if (park) {
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(txn, false));
  }
  const int rc = mdbx_env_set_geometry(env, -1, -1, 4 * kUpper, -1, -1, -1);
  EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_UNABLE_EXTEND_MAPSIZE) << rc << " " << mdbx_strerror(rc);
  if (park) {
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_unpark(txn, false));
  }
  ASSERT_EQ(base, env->dxb_mmap.base) << "the mapping moved under a live transaction";
  EXPECT_EQ(0, std::memcmp(held.iov_base, kValue, sizeof(kValue)));
  MDBX_val key, value;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor.cursor, &key, &value, MDBX_NEXT));
}

/* Without live transactions the resize may move the mapping again: a stale reader slot must not veto it. */
void resize_without_txn(MDBX_env *env) {
  const block_after_mapping block(env);
  const int rc = mdbx_env_set_geometry(env, -1, -1, 8 * kUpper, -1, -1, -1);
#if defined(_WIN32)
  /* Windows checks the reserved range before it unmaps the view and refuses the resize */
  EXPECT_EQ(MDBX_UNABLE_EXTEND_MAPSIZE, rc) << mdbx_strerror(rc);
#else
  EXPECT_EQ(MDBX_SUCCESS, rc) << mdbx_strerror(rc);
#endif
}

void resize_under_read_txn(const char *path, bool park) {
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_NO_FATAL_FAILURE(open_env(path, kUpper, &env));
  ASSERT_NO_FATAL_FAILURE(seed(env));
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    cursor_guard cursor;
    MDBX_val held;
    ASSERT_NO_FATAL_FAILURE(position_first(txn, cursor, &held));
    ASSERT_NO_FATAL_FAILURE(resize_under_live_txn(env, txn, cursor, held, park));
  }
  ASSERT_NO_FATAL_FAILURE(resize_without_txn(env));
}

MDBX_envinfo measure_env(MDBX_env *env) {
  MDBX_envinfo info;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  return info;
}

const char *const kGrowPath = "issue315-warmup.mdbx";
const char *const kChildVar = "MDBX_TEST_GH315_CHILD";

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

} // namespace

TEST(issue_gh0315, geometry_keeps_mapping_of_read_txn_in_calling_thread) {
  const env_files_guard guard({"issue315-read.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  resize_under_read_txn("issue315-read.mdbx", false);
}

TEST(issue_gh0315, geometry_keeps_mapping_of_parked_read_txn) {
  const env_files_guard guard({"issue315-parked.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  resize_under_read_txn("issue315-parked.mdbx", true);
}

TEST(issue_gh0315, geometry_keeps_mapping_of_write_txn_in_calling_thread) {
  const env_files_guard guard({"issue315-write.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_NO_FATAL_FAILURE(open_env("issue315-write.mdbx", kUpper, &env));
  ASSERT_NO_FATAL_FAILURE(seed(env));
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    {
      cursor_guard cursor;
      MDBX_val held;
      ASSERT_NO_FATAL_FAILURE(position_first(txn, cursor, &held));
      ASSERT_NO_FATAL_FAILURE(resize_under_live_txn(env, txn, cursor, held, false));
    }
    /* the write transaction stays usable */
    MDBX_val k = {const_cast<char *>("after"), 5}, v = {const_cast<char *>(kValue), sizeof(kValue)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
    txn_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }
  ASSERT_NO_FATAL_FAILURE(resize_without_txn(env));
}

/* Checks the shrink chain end-to-end: a committing transaction must truncate the oversized datafile down to
 * the used size, without moving the mapping base and without harming the data. */
TEST(issue_gh0315, commit_shrinks_file) {
  const env_files_guard guard({"issue315-shrink.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_NO_FATAL_FAILURE(open_env_fine("issue315-shrink.mdbx", kUpper, &env));
  const MDBX_envinfo grown = measure_env(env);
  ASSERT_GT(uint64_t(grown.mi_dxb_fsize), uint64_t(kUpper) * 3 / 4) << "the datafile was not created oversized";
  const void *const base = env->dxb_mmap.base;
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_val k = {const_cast<char *>("kept"), 4}, v = {const_cast<char *>(kValue), sizeof(kValue)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
    txn_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }
  const MDBX_envinfo shrunk = measure_env(env);
  EXPECT_LT(uint64_t(shrunk.mi_dxb_fsize), uint64_t(grown.mi_dxb_fsize) - 8 * kMiB)
      << "the committing transaction did not truncate the datafile";
  EXPECT_EQ(base, env->dxb_mmap.base) << "the shrink moved the mapping base";
  const MDBX_val key = {const_cast<char *>("kept"), 4};
  MDBX_val value;
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, MAIN_DBI, &key, &value));
    EXPECT_EQ(sizeof(kValue), value.iov_len);
    EXPECT_EQ(0, std::memcmp(value.iov_base, kValue, sizeof(kValue)));
  }
  {
    /* the environment reopens consistently over the truncated file */
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
    env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, "issue315-shrink.mdbx", kOpenFlags, 0644));
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, MAIN_DBI, &key, &value));
    EXPECT_EQ(sizeof(kValue), value.iov_len);
    EXPECT_EQ(0, std::memcmp(value.iov_base, kValue, sizeof(kValue)));
  }
}

/* The shrink alongside a parked reader: the committing transactions must succeed on every platform, the file
 * gets truncated (in place on POSIX even while the reader is parked; deferred until the reader is gone on
 * Windows, where the parked reader holds the remap-lock shared), and the parked reader survives on POSIX. */
TEST(issue_gh0315, commit_shrink_with_parked_reader) {
  const env_files_guard guard({"issue315-shrink-parked.mdbx"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_NO_FATAL_FAILURE(open_env_fine("issue315-shrink-parked.mdbx", kUpper, &env));
  const MDBX_envinfo grown = measure_env(env);
  ASSERT_GT(uint64_t(grown.mi_dxb_fsize), uint64_t(kUpper) * 3 / 4) << "the datafile was not created oversized";
  {
    MDBX_txn *reader = nullptr;
    txn_handle_guard reader_owner(&reader);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(reader, false));
    {
      MDBX_txn *txn = nullptr;
      txn_handle_guard txn_owner(&txn);
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
      MDBX_val k = {const_cast<char *>("kept"), 4}, v = {const_cast<char *>(kValue), sizeof(kValue)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
      txn_owner.release();
      ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
    }
#if !defined(_WIN32)
    EXPECT_TRUE(measure_env(env).mi_dxb_fsize < grown.mi_dxb_fsize - 8 * kMiB)
        << "a parked reader must not block the in-place shrink";
#endif
    /* the reader either survives, or is ousted/reset as a GC laggard by design */
    const int unpark_rc = mdbx_txn_unpark(reader, false);
    ASSERT_TRUE(unpark_rc == MDBX_SUCCESS || unpark_rc == MDBX_OUSTED) << unpark_rc;
    reader_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(reader));
  }
  /* on Windows the attempt was deferred until the reader released the remap-lock: a later commit completes it */
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_val k = {const_cast<char *>("touch"), 5}, v = {const_cast<char *>(kValue), sizeof(kValue)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
    txn_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }
  EXPECT_LT(uint64_t(measure_env(env).mi_dxb_fsize), uint64_t(grown.mi_dxb_fsize) - 8 * kMiB)
      << "the shrink did not settle";
}

/* Child process of warmup_stays_within_mapping: raises the upper bound and grows the database to about twice the
 * mapping of the parent (each value takes five pages). The parent owns the database files. */
TEST(issue_gh0315, DISABLED_grow_database) {
  const char *const spawned = std::getenv(kChildVar);
  if (!spawned || !*spawned) {
    GTEST_SKIP() << "runs only as the child process of warmup_stays_within_mapping";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kGrowPath, kOpenFlags, 0644));
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

TEST(issue_gh0315, warmup_stays_within_mapping) {
  const env_files_guard guard({kGrowPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_NO_FATAL_FAILURE(open_env(kGrowPath, kUpper, &env));
  ASSERT_NO_FATAL_FAILURE(seed(env));

  /* CTest passes the path of this executable, since executable_dir() falls back to "." where /proc/self/exe is
   * missing (e.g. FreeBSD); a direct run without it looks next to the running executable */
  const char *const self = std::getenv("MDBX_TEST_GH315_SELF");
  std::string child = tool_runner::executable_dir() + "/issue_gh0315";
#if defined(_WIN32)
  child += ".exe";
#endif
  if (self && *self)
    child = self;
  {
    const child_var_scope child_var;
    ASSERT_EQ(0, tool_runner::run({child, "--gtest_filter=issue_gh0315.DISABLED_grow_database",
                                   "--gtest_also_run_disabled_tests", "--gtest_brief=1"}))
        << "the child process failed to grow the database";
  }

  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  ASSERT_GT((info.mi_last_pgno + 1) * info.mi_dxb_pagesize, info.mi_mapsize)
      << "the database did not grow past the mapping of this process";
  const block_after_mapping block(env);
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_warmup(env, nullptr, MDBX_warmup_force, 0));
  /* the readahead edge set before touching the pages must lie within the mapping, so the test does not depend on
   * the touch past the mapping to fault */
  EXPECT_LE(uint64_t(env->lck->readahead_anchor >> 1) * env->ps, uint64_t(env->dxb_mmap.current));
}
