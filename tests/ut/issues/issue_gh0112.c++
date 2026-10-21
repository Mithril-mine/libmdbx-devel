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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

#if !defined(_WIN32)
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/112
// copy2pathname() mishandled the destination in three ways:
//  1. With MDBX_CP_OVERWRITE, a copy whose file lock failed on an empty
//     destination unlinked it, even though the lock conflict meant another
//     process owned the file: a concurrent copy would lose its just-created
//     file while still reporting success.
//  3. An overwritten destination was never truncated, so content of the old
//     file survived inside the new copy.
// Items 1 and 3 are exercised here (on POSIX); item 2 (a rejected exclusive
// create leaving an orphan) needed the fd 0..2 safeguard and is platform-
// specific, item 4 (partial stub write) needs a fault.

static void remove_db(const char *path) {
  /* the environment remove API is in the C++ binding; use plain unlink */
  remove(path);
}

static FILE *open_file(const char *path, const char *mode) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  return fopen(path, mode);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

/* Fills the file with `size` bytes of 'X'. */
static bool write_blob(const char *path, size_t size) {
  FILE *f = open_file(path, "wb");
  if (!f)
    return false;
  char blob[4096];
  memset(blob, 'X', sizeof(blob));
  bool ok = true;
  for (size_t done = 0; ok && done < size; done += sizeof(blob))
    ok = fwrite(blob, 1, sizeof(blob), f) == sizeof(blob);
  return fclose(f) == 0 && ok;
}

/* Returns the longest run of 'X' bytes in the file. */
static size_t longest_blob_run(const char *path) {
  FILE *f = open_file(path, "rb");
  if (!f)
    return SIZE_MAX;
  size_t run = 0, longest = 0;
  for (int c; (c = fgetc(f)) != EOF;) {
    run = (c == 'X') ? run + 1 : 0;
    if (longest < run)
      longest = run;
  }
  fclose(f);
  return longest;
}

static long file_size(const char *path) {
  FILE *f = open_file(path, "rb");
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fclose(f);
  return size;
}

TEST(issue_gh0112, copy_does_not_delete_locked_destination) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
#if defined(_WIN32)
  GTEST_SKIP() << "POSIX flock semantics not present on Windows";
#else
  const char *const testdb = "issue112-src.mdbx";
  const char *const dest = "issue112-dest.mdbx";
  remove_db(testdb);
  remove_db(dest);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  /* A second "copy" currently owns the destination: it created an empty file,
   * exactly the state between create-and-lock and the first write. */
  const int destfd = open(dest, O_CREAT | O_RDWR | O_EXCL, 0644);
  ASSERT_GE(destfd, 0);
  ASSERT_EQ(0, flock(destfd, LOCK_EX | LOCK_NB));

  /* The copy must fail (the destination is locked by another owner) and,
   * importantly, must leave the locked file at the path: it does not belong
   * to this call. */
  EXPECT_NE(MDBX_SUCCESS, mdbx_env_copy(env, dest, MDBX_CP_OVERWRITE));
  struct stat st;
  ASSERT_EQ(0, stat(dest, &st)) << "the locked destination must survive a failed copy";

  flock(destfd, LOCK_UN);
  close(destfd);
  remove_db(dest);

  mdbx_env_close(env);
  remove_db(testdb);
#endif
}

TEST(issue_gh0112, copy_overwrite_truncates_old_content) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue112-src2.mdbx";
  const char *const dest = "issue112-dest2.mdbx";
  remove_db(testdb);
  remove_db(dest);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  /* A pre-existing destination that is larger than the copied database: the
   * tail of the old content must not survive inside the new copy. */
  ASSERT_TRUE(write_blob(dest, (size_t)4 << 20));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_copy(env, dest, MDBX_CP_OVERWRITE));
  EXPECT_LT(longest_blob_run(dest), (size_t)64) << "the old file's content survived inside the copy";

  /* The copied database must be a valid environment and must not be padded
   * with the old file's content: the destination file size must equal the
   * used size of the source (a couple of meta pages), not the old 4096-byte
   * blob or anything derived from it. */
  {
    struct stat st;
    ASSERT_EQ(0, stat(dest, &st)) << "the destination must exist after the copy";
    EXPECT_LE((size_t)st.st_size, (size_t)4096 * 64)
        << "the copied database must not be padded with the old file's content (size " << st.st_size << ")";
  }
  MDBX_env *copy_env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&copy_env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(copy_env, dest, MDBX_RDONLY | MDBX_NOSUBDIR, 0644));

  mdbx_env_close(copy_env);
  mdbx_env_close(env);
  remove_db(dest);
  remove_db(testdb);
}

TEST(issue_gh0112, rejected_copy_keeps_destination) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue112-src3.mdbx";
  const char *const dest = "issue112-dest3.mdbx";
  remove_db(testdb);
  remove_db(dest);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  /* A copy rejected by the transaction or flag checks must not damage a
   * pre-existing destination, even with MDBX_CP_OVERWRITE. */
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_val key = {(void *)"k", 1}, data = {(void *)"v", 1};
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
  ASSERT_TRUE(write_blob(dest, 4096));
  EXPECT_EQ(MDBX_BAD_TXN, mdbx_txn_copy2pathname(txn, dest, MDBX_CP_OVERWRITE));
  EXPECT_EQ(4096, file_size(dest)) << "a copy rejected for a dirty transaction damaged the destination";
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  EXPECT_EQ(MDBX_EINVAL, mdbx_txn_copy2pathname(txn, dest, MDBX_CP_OVERWRITE | MDBX_CP_THROTTLE_MVCC));
  EXPECT_EQ(4096, file_size(dest)) << "a copy rejected for invalid flags damaged the destination";
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));

  mdbx_env_close(env);
  remove_db(dest);
  remove_db(testdb);
}