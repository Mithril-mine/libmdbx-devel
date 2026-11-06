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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/331 (direct I/O part)
// osal_openfile() opened a new copy destination with O_CREAT|O_EXCL|O_DIRECT. A
// filesystem without direct I/O (e.g. ramfs) creates the file and then fails the
// open with EINVAL; the retry without O_DIRECT kept O_EXCL and failed with EEXIST
// on the file the first call created. mdbx_env_copy() returned MDBX_EEXIST for a
// path that did not exist, and the empty file blocked every later attempt. Now
// O_DIRECT is set with fcntl(F_SETFL) after the open, and a failure there is
// ignored (the writes then go through the page cache).
//
// The fs_without_direct_io FAULT site, right after the open() of the file, makes
// the library behave as on such a filesystem: an open() with O_DIRECT fails with
// EINVAL after creating the file, and fcntl(F_SETFL, O_DIRECT) is skipped as if
// it failed. Both copy modes must then create a valid copy. Requires
// MDBX_PROBES=ON and MDBX_PROBES=1. The site must be reached on every POSIX
// platform; the test skips on Windows, where the POSIX open is not used.

namespace {

const char *const kSrc = "issue331d-src.mdbx";
const char *const kCopy = "issue331d-copy.mdbx";

/* Deletes both databases before the test and on every exit path. */
struct files_guard {
  files_guard() { remove_all(); }
  ~files_guard() {
    char reply[256];
    mdbx_probe_ctl("fault fs_without_direct_io none", reply, sizeof reply);
    remove_all();
  }
  static void remove_all() {
    mdbx_env_delete(kSrc, MDBX_ENV_JUST_DELETE);
    mdbx_env_delete(kCopy, MDBX_ENV_JUST_DELETE);
  }
};

struct env_guard {
  MDBX_env *env = nullptr;
  ~env_guard() {
    if (env)
      mdbx_env_close(env);
  }
};

/* Returns the hits of the FAULT site, or 0 when it was never reached. */
size_t site_hits() {
  char reply[512];
  if (mdbx_probe_ctl("query fs_without_direct_io", reply, sizeof reply) != 0)
    return 0;
  const char *const site = strstr(reply, "site ");
  char name[64];
  unsigned kind, armed;
  size_t seen, hits;
  return (site && sscanf(site, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, &hits) == 5) ? hits : 0;
}

/* The site is in the POSIX osal_openfile(), so it must be reached everywhere but on Windows. */
bool site_expected() {
#if defined(_WIN32) || defined(_WIN64)
  return false;
#else
  return true;
#endif
}

/* Copies the source to a new path while O_DIRECT is rejected, then reads the record back. */
void copy_and_verify(MDBX_env *src, MDBX_copy_flags_t flags, const char *what) {
  char reply[512];
  mdbx_env_delete(kCopy, MDBX_ENV_JUST_DELETE);
  ASSERT_EQ(0, mdbx_probe_ctl("reset fs_without_direct_io", reply, sizeof reply)) << reply;
  ASSERT_EQ(0, mdbx_probe_ctl("fault fs_without_direct_io 22", reply, sizeof reply)) << reply;
  const int rc = mdbx_env_copy(src, kCopy, flags);
  mdbx_probe_ctl("fault fs_without_direct_io none", reply, sizeof reply);
  if (site_hits() == 0) {
    ASSERT_FALSE(site_expected()) << "the fs_without_direct_io site was not reached (" << what << ")";
    GTEST_SKIP() << "the POSIX open of the copy is not used here (" << what << ")";
  }
  ASSERT_EQ(rc, MDBX_SUCCESS) << what << ": " << mdbx_strerror(rc);

  env_guard copy;
  ASSERT_EQ(mdbx_env_create(&copy.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(copy.env, kCopy, MDBX_NOSUBDIR | MDBX_RDONLY, 0644), MDBX_SUCCESS) << what;
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(copy.env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS) << what;
  MDBX_dbi dbi = 0;
  MDBX_val key = {const_cast<char *>("key"), 3}, data = {nullptr, 0};
  int err = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  if (err == MDBX_SUCCESS)
    err = mdbx_get(txn, dbi, &key, &data);
  const std::string value =
      (err == MDBX_SUCCESS) ? std::string(static_cast<const char *>(data.iov_base), data.iov_len) : std::string();
  mdbx_txn_abort(txn);
  ASSERT_EQ(err, MDBX_SUCCESS) << what << ": " << mdbx_strerror(err);
  EXPECT_EQ(value, "value") << what;
}

} // namespace

TEST(issue_gh0331, copy_to_new_path_without_direct_io) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  files_guard files;
  env_guard src;
  ASSERT_EQ(mdbx_env_create(&src.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(src.env, kSrc, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(src.env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  MDBX_val key = {const_cast<char *>("key"), 3}, data = {const_cast<char *>("value"), 5};
  int rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  else
    mdbx_txn_abort(txn);
  ASSERT_EQ(rc, MDBX_SUCCESS) << mdbx_strerror(rc);

  copy_and_verify(src.env, MDBX_CP_DEFAULTS, "as-is copy");
  if (IsSkipped() || HasFatalFailure())
    return;
  copy_and_verify(src.env, MDBX_CP_COMPACT, "compacting copy");
}
