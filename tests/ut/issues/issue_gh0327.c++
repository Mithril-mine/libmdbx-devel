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

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/327
// copy_asis() copies the data with copy_file_range() on Linux, which writes at
// an explicit offset and does not move the destination's file position. When a
// later copy_file_range() call failed with an error that switches to the
// portable fallback, the fallback wrote with write() at the file position, just
// after the meta pages: the copied data was overwritten and the rest landed too
// early, while mdbx_env_copy() reported success.
//
// The copy_file_range_midway_errno FAULT site makes the first copy_file_range()
// copy 64 KiB only and the next one fail with the injected errno. Every record
// of the copy must then be intact. Requires MDBX_PROBES=ON and MDBX_PROBES=1;
// skips where copy_file_range() is not used (non-Linux, some kernels).

namespace {

const char *const kSrc = "issue327-src.mdbx";
const char *const kCopy = "issue327-copy.mdbx";
constexpr int kRecords = 5000;
constexpr size_t kValueLen = 1000;

void value_of(int i, char *v) {
  for (size_t j = 0; j < kValueLen; ++j)
    v[j] = static_cast<char>('a' + (i + j) % 26);
}

/* Deletes both databases before the test and on every exit path. */
struct files_guard {
  files_guard() { remove_all(); }
  ~files_guard() { remove_all(); }
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
  if (mdbx_probe_ctl("query copy_file_range_midway_errno", reply, sizeof reply) != 0)
    return 0;
  const char *const site = strstr(reply, "site ");
  char name[64];
  unsigned kind, armed;
  size_t seen, hits;
  return (site && sscanf(site, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, &hits) == 5) ? hits : 0;
}

/* Copies the source while copy_file_range() fails midway with `err`, then
 * checks every record of the copy. */
void copy_and_verify(MDBX_env *src, int err, const char *what) {
  char reply[512], cmd[128];
  mdbx_env_delete(kCopy, MDBX_ENV_JUST_DELETE);
  ASSERT_EQ(0, mdbx_probe_ctl("reset copy_file_range_midway_errno", reply, sizeof reply)) << reply;
  snprintf(cmd, sizeof cmd, "fault copy_file_range_midway_errno %d", err);
  ASSERT_EQ(0, mdbx_probe_ctl(cmd, reply, sizeof reply)) << reply;
  const int rc = mdbx_env_copy(src, kCopy, MDBX_CP_DEFAULTS);
  mdbx_probe_ctl("fault copy_file_range_midway_errno none", reply, sizeof reply);
  ASSERT_EQ(rc, MDBX_SUCCESS) << what;
  if (site_hits() < 2)
    GTEST_SKIP() << "copy_file_range() was not used for the copy (" << what << ")";

  env_guard copy;
  ASSERT_EQ(mdbx_env_create(&copy.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(copy.env, kCopy, MDBX_NOSUBDIR | MDBX_RDONLY, 0644), MDBX_SUCCESS) << what;
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(copy.env, nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS) << what;
  MDBX_dbi dbi = 0;
  int bad = 0, first_bad = -1, first_err = MDBX_SUCCESS;
  int err_dbi = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  char v[kValueLen];
  for (int i = 0; err_dbi == MDBX_SUCCESS && i < kRecords; ++i) {
    char k[16];
    snprintf(k, sizeof k, "k%06d", i);
    MDBX_val mk = {k, strlen(k)}, mv;
    value_of(i, v);
    const int got = mdbx_get(txn, dbi, &mk, &mv);
    if (got != MDBX_SUCCESS || mv.iov_len != kValueLen || memcmp(mv.iov_base, v, kValueLen) != 0) {
      if (bad++ == 0) {
        first_bad = i;
        first_err = got;
      }
      if (got == MDBX_CORRUPTED || got == MDBX_PAGE_NOTFOUND)
        break;
    }
  }
  mdbx_txn_abort(txn);
  EXPECT_EQ(err_dbi, MDBX_SUCCESS) << what;
  EXPECT_EQ(bad, 0) << what << ": the copy is corrupt, first bad record k" << first_bad << ": "
                    << (first_err == MDBX_SUCCESS ? "wrong value" : mdbx_strerror(first_err));
}

} // namespace

TEST(issue_gh0327, copy_survives_a_failure_after_a_partial_copy_file_range) {
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
  ASSERT_EQ(mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  char v[kValueLen];
  for (int i = 0; i < kRecords; ++i) {
    char k[16];
    snprintf(k, sizeof k, "k%06d", i);
    value_of(i, v);
    MDBX_val mk = {k, strlen(k)}, mv = {v, kValueLen};
    ASSERT_EQ(mdbx_put(txn, dbi, &mk, &mv, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  /* EXDEV and EINVAL switch only this copy to the fallback; ENOSYS marks
   * copy_file_range() as unavailable for the whole process, so it goes last. */
  copy_and_verify(src.env, EXDEV, "EXDEV");
  if (IsSkipped() || HasFatalFailure())
    return;
  copy_and_verify(src.env, EINVAL, "EINVAL");
  if (HasFatalFailure())
    return;
  copy_and_verify(src.env, ENOSYS, "ENOSYS");
}
