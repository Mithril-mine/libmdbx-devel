/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// \file RAII guard that removes a test's database files on every exit path.
///
/// Unlike test-env.h this header includes "mdbx.h" and therefore must NOT be
/// precompiled into the shared PCH (see pch.h++ for why the libmdbx public
/// headers stay out of it). Include it after "mdbx.h" in the tests that open
/// environments, then construct the guard as the first statement of every
/// TEST() body that creates database files:
///
///     const env_files_guard guard({kSource, kCopy});
///
/// The constructor removes leftovers of an earlier run (a crashed or timed-out
/// previous test cannot clean up after itself), the destructor deletes the
/// files through mdbx_env_delete(MDBX_ENV_JUST_DELETE) — which removes the
/// `-lck` files and a directory environment as well — so a failed `ASSERT_*`
/// or `GTEST_SKIP()` leaves nothing behind. In-process cleanup cannot cover a
/// test that crashes or is killed by its timeout; the constructor-side delete
/// keeps the next run clean in that case. The holder of the guard must close
/// its environments before the destruction: on Windows a mapped file cannot
/// be deleted.

#pragma once

#ifdef _WIN32
/* Windows-header hygiene, the same set test-env.h uses before <windows.h>:
 * mdbx.h pulls <windows.h> on Windows (mdbx.h:177) and without NOMINMAX its
 * min/max macros break the mdbx++ headers parsed right after us (the
 * env-guard include often precedes "mdbx.h++" in the test). When this header
 * is included after <windows.h> was pulled without the hygiene (e.g. by
 * gtest in a NO_PCH build), the undefs below still sanitize the macros. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10 baseline, as in src/preface.h */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif /* _WIN32 */

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

/// \brief Removes the listed database files (and their `-lck` companions) when
/// constructed and again when destroyed, i.e. on every exit path of the test.
class env_files_guard {
  std::vector<std::string> paths;

  static void cleanup_one(const std::string &path, bool in_teardown) {
    const int rc = mdbx_env_delete(path.c_str(), MDBX_ENV_JUST_DELETE);
    if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
      if (in_teardown)
        ADD_FAILURE() << "env_files_guard: unable delete `" << path << "`, error " << rc << " " << mdbx_strerror(rc);
      else
        fprintf(stderr, "env_files_guard: unable delete leftover `%s`, error %d %s\n", path.c_str(), rc,
                mdbx_strerror(rc));
    }
  }

public:
  env_files_guard(std::initializer_list<const char *> names) {
    paths.reserve(names.size());
    for (const char *name : names) {
      paths.emplace_back(name);
      cleanup_one(paths.back(), false);
    }
  }

  env_files_guard(const env_files_guard &) = delete;
  env_files_guard &operator=(const env_files_guard &) = delete;

  ~env_files_guard() {
    for (const auto &path : paths)
      cleanup_one(path, true);
  }
};

/// \brief Scoped ownership of a raw `MDBX_env` handle: closes it on destruction.
///
/// Declare these handles AFTER the `env_files_guard` (destruction runs in
/// reverse order, so the environment is closed before its files are deleted —
/// on Windows a mapped file cannot be deleted). The guard reads the actual
/// handle value at destruction time, so it may be assigned later; use
/// `release()` before a successful close that is part of the test's own
/// assertions.
struct env_handle_guard {
  MDBX_env **handle;
  bool armed = true;

  explicit env_handle_guard(MDBX_env **handle) : handle(handle) {}
  ~env_handle_guard() {
    if (armed && *handle)
      mdbx_env_close(*handle);
  }
  void release() { armed = false; }
};

/// \brief Scoped ownership of a raw `MDBX_txn` handle: aborts it on destruction.
struct txn_handle_guard {
  MDBX_txn **handle;
  bool armed = true;

  explicit txn_handle_guard(MDBX_txn **handle) : handle(handle) {}
  ~txn_handle_guard() {
    if (armed && *handle)
      mdbx_txn_abort(*handle);
  }
  void release() { armed = false; }
};
