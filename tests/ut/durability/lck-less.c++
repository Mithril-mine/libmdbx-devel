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

/// The lck-less (without-lck) read-only guarantees of the durability program
/// (#564, cluster E1), pinned end-to-end through the real `lck_setup()` entry
/// conditions (src/lck.c:130-193) induced by the chmod technique of
/// tests/ut/issues/issue_gh0316.c++ - the lck-file is made unwritable (0444),
/// so its mandatory O_RDWR open fails with EACCES.
///
/// The pinned matrix is the #401 analysis (the rw+lck-less unreachability is
/// the current contract):
///  - EACCES/EPERM with both MDBX_RDONLY and MDBX_EXCLUSIVE set -> lck-less
///    mode (lck.c:165-169, the entry itself at lck.c:188-193);
///  - ENOFILE/EROFS with MDBX_RDONLY is subject to the read-only-filesystem
///    verdict (lck.c:170-181): on a writable filesystem an unreadable lck is
///    a clean open failure (lck.c:183-185) - a plain MDBX_RDONLY does NOT
///    enter lck-less via file permissions;
///  - any lck-open failure for a writable environment is a clean open error,
///    i.e. rw+lck-less is unreachable (the #401 matrix row);
///  - with the permissions restored the usual locked open works again.
///
/// Inside the lck-less mode a read-only transaction has no reader slot, so
/// parking is impossible in principle and must be refused with MDBX_EPERM
/// (the #316-hardened contract), while reads of the committed data work.
///
/// POSIX-only: the induction is chmod(2) on the lck-file, and the scenarios
/// are meaningless where permissions are bypassed - under root such runs are
/// skipped, as in issue_gh0317.c++.

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"
#include "../test-env.h++"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef _WIN32

namespace {

const char *const kDb = "lck-less.mdbx";
const std::string kLck = std::string(kDb) + MDBX_LOCK_SUFFIX;

/* RAII for the chmod-induction: makes the lck-file unwritable (0444) and
 * restores the saved mode on every exit path. A leaked unwritable lck would
 * poison the subsequent tests and their cleanup. Note the files-cleanup
 * itself (env_files_guard via mdbx_env_delete) does not need the file's
 * write permission - unlink() depends on the directory's - the restoration
 * keeps the leftovers harmless anyway. */
class lck_unwritable {
  std::string path;
  mode_t saved = 0;

public:
  explicit lck_unwritable(const std::string &lck_path) : path(lck_path) {
    struct stat st;
    if (::stat(lck_path.c_str(), &st) == 0)
      saved = st.st_mode & 07777u;
    if (::chmod(lck_path.c_str(), 0444) != 0)
      saved = 0 /* nothing was induced, nothing to restore */;
  }
  ~lck_unwritable() {
    if (saved)
      ::chmod(path.c_str(), saved);
  }
  lck_unwritable(const lck_unwritable &) = delete;
  lck_unwritable &operator=(const lck_unwritable &) = delete;
};

void make_db_with_data() {
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi main = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main));
  const MDBX_val k1 = {const_cast<char *>("key-one"), 7}, k2 = {const_cast<char *>("key-two"), 7};
  MDBX_val v1 = {const_cast<char *>("value-one"), 9}, v2 = {const_cast<char *>("value-two"), 9};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, main, &k1, &v1, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, main, &k2, &v2, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
}

/* Opens the database with data in the lck-less mode (the EACCES +
 * MDBX_RDONLY|MDBX_EXCLUSIVE entry of lck_setup, lck.c:165-169) and verifies
 * the mode's sentinel: without the lck there is no readers table, so
 * mi_numreaders is the INT32_MAX placeholder (src/api-env.c:817). Returns
 * `true` when the lck-less mode was not reached - the induction did not
 * happen (an exotic setup where permissions are bypassed in spite of the
 * non-root uid), the caller must fail. */
bool open_lckless_ro_exclusive(MDBX_env **env_out) {
  *env_out = nullptr;
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return true;
  if (mdbx_env_open(env, kDb, MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE, 0) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return true;
  }
  MDBX_envinfo info = {};
  if (mdbx_env_info_ex(env, nullptr, &info, sizeof(info)) != MDBX_SUCCESS || info.mi_numreaders != INT32_MAX) {
    mdbx_env_close(env);
    return true;
  }
  *env_out = env;
  return false;
}

/* Scenario 1 (the EACCES+RDONLY|EXCLUSIVE branch, lck.c:165-169): the open
 * succeeds entering the lck-less mode; a write transaction is refused since
 * the environment is read-only; parking a read transaction is refused with
 * MDBX_EPERM - the #316-hardened contract (no reader slot, nothing to park
 * into). */
TEST(lck_less, rdonly_exclusive_refuses_rw_and_park) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  if (::geteuid() == 0)
    GTEST_SKIP() << "the lck-less mode cannot be induced via file permissions under root";
  make_db_with_data();

  MDBX_env *env = nullptr;
  {
    const lck_unwritable induction(kLck);
    ASSERT_FALSE(open_lckless_ro_exclusive(&env)) << "the lck-less mode was not reached";
    ASSERT_NE(env, nullptr);
    const env_handle_guard env_owner(&env);

    /* the write side is unreachable: the environment is read-only */
    MDBX_txn *txn = nullptr;
    EXPECT_EQ(MDBX_EACCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));

    /* the #316-hardened contract: park/unpark refuse a transaction without
     * a reader slot, the transaction stays usable */
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    MDBX_dbi main = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main));
    const txn_handle_guard txn_owner(&txn);
    EXPECT_EQ(MDBX_EPERM, mdbx_txn_park(txn, false));
    EXPECT_EQ(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED) << "a refused park leaves no parked flag";
    EXPECT_EQ(MDBX_EPERM, mdbx_txn_park(txn, true)) << "autounpark makes no difference";
    EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_unpark(txn, false)) << "nothing was parked, the unpark is a no-op";
    MDBX_val key = {const_cast<char *>("key-one"), 7}, data;
    EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn, main, &key, &data));
    EXPECT_EQ(0, memcmp(data.iov_base, "value-one", data.iov_len));
  }
}

/* Scenario 2 (the same lck.c:165-169 entry): a read transaction inside the
 * lck-less mode reads the committed data normally - begin/read/abort, no
 * reader slot needed for the reads themselves. */
TEST(lck_less, read_txn_works) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  if (::geteuid() == 0)
    GTEST_SKIP() << "the lck-less mode cannot be induced via file permissions under root";
  make_db_with_data();

  MDBX_env *env = nullptr;
  {
    const lck_unwritable induction(kLck);
    ASSERT_FALSE(open_lckless_ro_exclusive(&env)) << "the lck-less mode was not reached";
    ASSERT_NE(env, nullptr);
    const env_handle_guard env_owner(&env);

    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    MDBX_dbi main = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main));
    const MDBX_val k1 = {const_cast<char *>("key-one"), 7}, k2 = {const_cast<char *>("key-two"), 7};
    MDBX_val data;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, main, &k1, &data));
    EXPECT_EQ(9u, data.iov_len);
    EXPECT_EQ(0, memcmp(data.iov_base, "value-one", data.iov_len));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, main, &k2, &data));
    EXPECT_EQ(0, memcmp(data.iov_base, "value-two", data.iov_len));
    const MDBX_val missing = {const_cast<char *>("no-such"), 7};
    EXPECT_EQ(MDBX_NOTFOUND, mdbx_get(txn, main, &missing, &data));
    mdbx_txn_abort(txn);
  }
}

/* Scenario 3 (the EACCES fallthrough to the ENOFILE/EROFS case, lck.c:169-181):
 * a plain MDBX_RDONLY (no EXCLUSIVE) does not qualify for the permission-based
 * lck-less entry, and the read-only-filesystem verdict (lck.c:174, the
 * statvfs check of osal_check_fs_rdonly) fails on a writable filesystem - so
 * the open is a clean failure returning the original EACCES (lck.c:183-185,
 * propagated at env.c:439-441), and with the permissions restored the same
 * open succeeds (the failed attempt left nothing behind). */
TEST(lck_less, plain_rdonly_refused_on_writable_fs) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  if (::geteuid() == 0)
    GTEST_SKIP() << "the lck-less mode cannot be induced via file permissions under root";
  make_db_with_data();

  {
    const lck_unwritable induction(kLck);
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    const env_handle_guard env_owner(&env);
    EXPECT_EQ(MDBX_EACCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR | MDBX_RDONLY, 0))
        << "a plain read-only open must fail cleanly on a writable filesystem with an unwritable lck";
  }
  /* with the permissions restored (the induction's destructor) the same
   * open works: the failed attempt left nothing behind */
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const env_handle_guard env_owner(&env);
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR | MDBX_RDONLY, 0));
}

/* Scenario 4 (the writable-environment fallthrough to the default case,
 * lck.c:169-182 -> lck.c:183-185): with the lck unwritable an EXCLUSIVE
 * read-write open fails cleanly with EACCES - the rw+lck-less mode is
 * unreachable (the #401 matrix row) - and after the permissions are restored
 * the usual locked open works again (the sanity of the induction technique,
 * the normal path of lck.c:194+). */
TEST(lck_less, permissions_restored_exclusive_rw_works) {
  const env_files_guard files({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  if (::geteuid() == 0)
    GTEST_SKIP() << "the lck-less mode cannot be induced via file permissions under root";
  make_db_with_data();

  {
    const lck_unwritable induction(kLck);
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    const env_handle_guard env_owner(&env);
    EXPECT_EQ(MDBX_EACCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR | MDBX_EXCLUSIVE, 0644))
        << "the rw+lck-less mode must stay unreachable: a clean EACCES, no silent fallback";
  }
  /* permissions restored (the induction's destructor): the EXCLUSIVE
   * read-write open and the write transactions work as usual */
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb, MDBX_NOSUBDIR | MDBX_EXCLUSIVE, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi main = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main));
  const MDBX_val k3 = {const_cast<char *>("key-three"), 10};
  MDBX_val v3 = {const_cast<char *>("value-three"), 11};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, main, &k3, &v3, MDBX_UPSERT));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

} // namespace

#endif /* !_WIN32 */
