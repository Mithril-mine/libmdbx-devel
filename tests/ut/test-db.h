/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)
///
/// RAII helpers that keep a gtest-based unit test from leaving database files
/// behind (https://github.com/Mithril-mine/libmdbx-devel/issues/343).
///
/// Provides:
///  - class mdtest_db: deletes the database files at a path (the data file,
///    the `-lck` file, or a directory environment) when it is constructed and
///    again when it is destroyed. The destructor runs on every exit path of a
///    test: success, a failed ASSERT_*, GTEST_SKIP() or an exception. Deleting
///    at construction also removes the files a crashed or killed earlier run
///    left behind, including a `-lck` file whose data file is gone (for an
///    MDBX_NOSUBDIR file path and the default `-lck` suffix). A deletion
///    that fails is reported as a test failure (with --gtest_throw_on_failure
///    that failure would throw from a destructor and terminate the process).
///  - mdtest_env: a std::unique_ptr that closes an MDBX_env when it goes out
///    of scope. Windows cannot delete a mapped file, so the environment must
///    be closed before the mdtest_db destructor runs: declare the mdtest_env
///    after the mdtest_db, and C++ destroys it first. To close the environment
///    earlier, declare the owner without const and call env_owner.reset(),
///    never mdbx_env_close(env) directly: the owner would close it a second time.
///
/// Usage:
///   const mdtest_db testdb("issue123.mdbx");
///   MDBX_env *env = nullptr;
///   ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
///   const mdtest_env env_owner(env);
///   ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));
///
/// A forked child that ends with _exit() runs no destructors, so it never
/// deletes the parent's files.

#pragma once

#include "test-env.h" /* first: the Windows macros must precede <windows.h> from mdbx.h */

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

class mdtest_db {
  const std::string path_;

  void erase() const {
    const int rc = mdbx_env_delete(path_.c_str(), MDBX_ENV_JUST_DELETE);
    if (rc == MDBX_RESULT_TRUE) {
      /* Nothing found: mdbx_env_delete() looks for the data file first and
       * leaves a lock file whose data file is gone. */
      const std::string lck = path_ + "-lck";
      if (std::remove(lck.c_str()) != 0) {
        const int err = errno;
        if (err != ENOENT)
          ADD_FAILURE() << "cannot delete the lock file \"" << lck << "\": errno " << err;
      }
    } else if (rc != MDBX_SUCCESS)
      ADD_FAILURE() << "cannot delete the test database \"" << path_ << "\": " << mdbx_strerror(rc);
  }

public:
  explicit mdtest_db(std::string path) : path_(std::move(path)) { erase(); }
  ~mdtest_db() { erase(); }
  mdtest_db(const mdtest_db &) = delete;
  mdtest_db &operator=(const mdtest_db &) = delete;

  const char *c_str() const noexcept { return path_.c_str(); }
};

struct mdtest_env_closer {
  void operator()(MDBX_env *env) const noexcept { mdbx_env_close(env); }
};
using mdtest_env = std::unique_ptr<MDBX_env, mdtest_env_closer>;
