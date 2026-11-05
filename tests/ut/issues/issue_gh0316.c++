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

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include <sys/stat.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/316
// In without-lck mode a read transaction has no reader slot (txn->ro.slot is
// null), but txn_ro_park() read slot->pid unconditionally, so mdbx_txn_park(),
// and mdbx_env_copy(MDBX_CP_THROTTLE_MVCC) that parks internally, crashed with
// a null dereference. Without a slot nothing can oust the reader, so park and
// unpark now change only the transaction flags. POSIX-only: read-only files
// force without-lck mode (chmod).

namespace {

const std::string kDb = "issue316.mdbx", kLck = kDb + "-lck", kCopy = "issue316-copy.mdbx";

void remove_files() {
  for (const std::string &path : {kDb, kLck, kCopy, kCopy + "-lck"}) {
    chmod(path.c_str(), 0644);
    std::remove(path.c_str());
  }
}

/* Deletes the files before the test and on every exit path of it. */
struct files_remover {
  files_remover() { remove_files(); }
  ~files_remover() { remove_files(); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};

struct txn_aborter {
  void operator()(MDBX_txn *txn) const { mdbx_txn_abort(txn); }
};

} // namespace

TEST(issue_gh0316, park_without_lck) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const files_remover cleanup;
  MDBX_env *env = nullptr;
  MDBX_val key = {(void *)"key", 3}, data = {(void *)"value", 5};
  {
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb.c_str(), MDBX_NOSUBDIR, 0644));
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(txn);
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn_owner.release()));
  }

  /* read-only files: the open falls back to without-lck mode, where a read transaction has no reader slot */
  if (chmod(kLck.c_str(), 0444) != 0 || chmod(kDb.c_str(), 0444) != 0) {
    GTEST_SKIP() << "the file system does not support POSIX permissions";
    return;
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDb.c_str(), MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE, 0));
  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  if (info.mi_numreaders != INT32_MAX) {
    GTEST_SKIP() << "without-lck mode not reached (the lock file is writable, e.g. when running as root)";
    return;
  }

  for (const bool autounpark : {false, true}) {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    const std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(txn);
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(txn, autounpark)) << "autounpark " << autounpark;
    EXPECT_NE(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED) << "autounpark " << autounpark;
    MDBX_val found;
    if (!autounpark) {
      EXPECT_EQ(MDBX_BAD_TXN, mdbx_get(txn, dbi, &key, &found)) << "a parked transaction must not read";
      EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_unpark(txn, false));
    }
    EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn, dbi, &key, &found)) << "autounpark " << autounpark;
    EXPECT_EQ(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED) << "autounpark " << autounpark;
  }

  /* a broken parked transaction: unpark resets it, as with a reader slot */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
    const std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_park(txn, false));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_break(txn));
    EXPECT_EQ(MDBX_BAD_TXN, mdbx_txn_unpark(txn, false));
    EXPECT_NE(0, mdbx_txn_flags(txn) & MDBX_TXN_FINISHED);
    EXPECT_EQ(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED) << "a reset leaves no parked flag";
  }

  /* a throttled copy parks the read transaction internally */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_copy(env, kCopy.c_str(), MDBX_CP_THROTTLE_MVCC));
}
