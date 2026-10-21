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

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/107
// Four defects in opening and renaming tables: (1,2) the documented pseudo-names
// MDBX_CHK_GC/MDBX_CHK_META are dereferenced before the pointer comparison and
// crash the process on open/rename; (3) re-opening a non-empty table with
// MDBX_CREATE and different flags through an already-open handle returns success
// with the old flags instead of MDBX_INCOMPATIBLE; (4) opening a table that
// another process has dropped returns success instead of MDBX_NOTFOUND.
// The whole file is POSIX-only because the last defect needs a second process
// (the POSIX OFD-locks forbid a second in-process handle).

static void put_key(MDBX_txn *txn, MDBX_dbi dbi, const std::string &key) {
  MDBX_val key_val = {const_cast<char *>(key.data()), key.size()};
  MDBX_val val_val = {key_val.iov_base, key_val.iov_len};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key_val, &val_val, MDBX_UPSERT));
}

TEST(issue_gh0107, pseudo_names_and_recreate) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue107.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));

  /* The pseudo-name pointers must be recognized without being dereferenced. */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi = MDBX_dbi(0xdeadbeef);
    EXPECT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, (const char *)MDBX_CHK_MAIN, MDBX_DB_DEFAULTS, &dbi));
    EXPECT_EQ(MDBX_INCOMPATIBLE, mdbx_dbi_open(txn, (const char *)MDBX_CHK_GC, MDBX_DB_DEFAULTS, &dbi))
        << "the pseudo-name must not be dereferenced";
    EXPECT_EQ(MDBX_EINVAL, mdbx_dbi_open(txn, (const char *)MDBX_CHK_META, MDBX_DB_DEFAULTS, &dbi))
        << "the pseudo-name must not be dereferenced";
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }

  /* Rename to a pseudo-name must be rejected, not crash. */
  MDBX_dbi t = 0;
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_CREATE, &t));
    for (int i = 0; i < 10; ++i)
      put_key(txn, t, "key" + std::to_string(i));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    EXPECT_EQ(MDBX_EINVAL, mdbx_dbi_rename(txn, t, (const char *)MDBX_CHK_GC));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }

  /* Re-opening a non-empty table through an already-open handle with MDBX_CREATE
   * and different persistent flags must yield MDBX_INCOMPATIBLE. */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi t2 = 0xdeadbeef;
    EXPECT_EQ(MDBX_INCOMPATIBLE, mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &t2))
        << "re-creating a non-empty table with different flags must be rejected";
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
}

TEST(issue_gh0107, open_dropped_table_by_another_process) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue107b.mdbx";
  const std::string lck_path = testdb + "-lck";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  std::remove(lck_path.c_str());

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));

  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi t = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_CREATE, &t));
    for (int i = 0; i < 10; ++i)
      put_key(txn, t, "key" + std::to_string(i));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* A forked child (a genuine second process) drops the table. */
  int pipefd[2];
  ASSERT_EQ(0, pipe(pipefd));
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    close(pipefd[0]);
    MDBX_env *child_env = nullptr;
    int rc = (mdbx_env_create(&child_env) == MDBX_SUCCESS && mdbx_env_set_maxdbs(child_env, 8) == MDBX_SUCCESS)
                 ? mdbx_env_open(child_env, testdb.c_str(), MDBX_NOSUBDIR, 0644)
                 : MDBX_PROBLEM;
    if (rc == MDBX_SUCCESS) {
      MDBX_txn *txn = nullptr;
      rc = mdbx_txn_begin(child_env, nullptr, MDBX_TXN_READWRITE, &txn);
      if (rc == MDBX_SUCCESS) {
        MDBX_dbi t = 0;
        rc = mdbx_dbi_open(txn, "t", MDBX_DB_DEFAULTS, &t);
        if (rc == MDBX_SUCCESS) {
          rc = mdbx_drop(txn, t, true);
          if (rc == MDBX_SUCCESS)
            rc = mdbx_txn_commit(txn);
          else
            mdbx_txn_abort(txn);
        }
      }
    }
    mdbx_env_close(child_env);
    const ssize_t n = write(pipefd[1], &rc, sizeof(rc));
    (void)n;
    _exit(0);
  }
  close(pipefd[1]);
  int child_rc = -1;
  ssize_t n = read(pipefd[0], &child_rc, sizeof(child_rc));
  ASSERT_EQ(n, (ssize_t)sizeof(child_rc));
  close(pipefd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  ASSERT_EQ(child_rc, MDBX_SUCCESS) << "the child must drop the table cleanly";

  /* The live handle must now report the table as absent, not success. */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi t = 0xdeadbeef;
    EXPECT_EQ(MDBX_NOTFOUND, mdbx_dbi_open(txn, "t", MDBX_DB_DEFAULTS, &t))
        << "opening a table dropped by another process must yield MDBX_NOTFOUND";
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
  std::remove(lck_path.c_str());
}