/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but
/// not limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
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
#include <cstring>
#include <memory>
#include <string>

#include <sys/stat.h>

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/316
// In without-lck mode a read transaction has no reader slot (txn->ro.slot is
// null). Parking/unparking such a transaction is impossible in principle:
// there is nowhere to store and track the transaction's MVCC snapshot in a
// way visible outside the transaction, and while a write transaction is
// running (in exclusive mode) no read transaction can hold a different
// snapshot, so no ousting can occur. Therefore mdbx_txn_park() and
// mdbx_txn_unpark() must refuse such transactions with MDBX_EPERM instead of
// crashing (the original symptom), and mdbx_env_copy() with
// MDBX_CP_THROTTLE_MVCC silently proceeds without throttling.
//
// The other tests sweep the public API of a read-only transaction in
// without-lck mode (reads, cursor positioning, transaction/env information
// and operations, write refusals, multiple transactions) to pin down the
// deterministic outcomes and guard against slot-related regressions.

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

MDBX_val val_of(const char *s) {
  MDBX_val v{(void *)s, strlen(s)};
  return v;
}

int make_withoutlck_ro(MDBX_env **env_out, MDBX_txn **txn_out, MDBX_dbi *main_out, MDBX_dbi *dups_out,
                       MDBX_dbi *fixed_out) {
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_set_maxdbs(env, 4);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, kDb.c_str(), MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return rc;
  }
  std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  MDBX_dbi main = 0, dups = 0, fixed = 0;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "dups", MDBX_CREATE | MDBX_DUPSORT, &dups);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "fixed", MDBX_CREATE | MDBX_DUPSORT | MDBX_DUPFIXED, &fixed);
  MDBX_val kv_main[] = {val_of("k1"), val_of("v1"), val_of("k2"), val_of("v2"), val_of("k3"), val_of("v3")};
  MDBX_val k_a = val_of("A"), v_a1 = val_of("a1"), v_a2 = val_of("a2");
  MDBX_val k_b = val_of("B"), v_b1 = val_of("b1");
  char fixed16[16];
  memset(fixed16, 0, sizeof(fixed16));
  const MDBX_val k_f = val_of("f1");
  MDBX_val v_f = {fixed16, sizeof(fixed16)};
  for (int i = 0; rc == MDBX_SUCCESS && i < 3; ++i)
    rc = mdbx_put(txn, main, &kv_main[i * 2], &kv_main[i * 2 + 1], MDBX_UPSERT);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_put(txn, dups, &k_a, &v_a1, MDBX_UPSERT);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_put(txn, dups, &k_a, &v_a2, MDBX_UPSERT);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_put(txn, dups, &k_b, &v_b1, MDBX_UPSERT);
  memcpy(fixed16, "f00000000000001", 15);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_put(txn, fixed, &k_f, &v_f, MDBX_UPSERT);
  memcpy(fixed16, "f00000000000002", 15);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_put(txn, fixed, &k_f, &v_f, MDBX_UPSERT);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return rc;
  }
  /* dispose the write-mode handle: the same handle cannot be re-opened */
  mdbx_env_close(env);
  env = nullptr;
  env_owner.release();

  /* read-only files: the open falls back to without-lck mode, where a read
   * transaction has no reader slot. The read-only open of the database with
   * an unwritable lock file returns MDBX_RESULT_TRUE: the environment falls
   * back to the without-lck mode */
  if (chmod(kLck.c_str(), 0444) != 0 || chmod(kDb.c_str(), 0444) != 0)
    return MDBX_RESULT_TRUE;
  rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_set_maxdbs(env, 4);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, kDb.c_str(), MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE, 0);
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return rc;
  }
  MDBX_envinfo info = {};
  rc = mdbx_env_info_ex(env, nullptr, &info, sizeof(info));
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return rc;
  }
  if (info.mi_numreaders != INT32_MAX) {
    /* without-lck mode not reached (the lock file is writable, e.g. when
     * running as root) */
    mdbx_env_close(env);
    return MDBX_RESULT_TRUE;
  }

  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &main);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "dups", MDBX_DUPSORT, &dups);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, "fixed", MDBX_DUPSORT | MDBX_DUPFIXED, &fixed);
  if (rc != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return rc;
  }
  env_owner.release();
  *env_out = env;
  *txn_out = txn;
  *main_out = main;
  *dups_out = dups;
  *fixed_out = fixed;
  return MDBX_SUCCESS;
}

/* The predicate for mdbx_cursor_scan(): counts the scanned pairs. */
int scan_count(void *context, MDBX_val *, MDBX_val *, void *) noexcept {
  ++*static_cast<size_t *>(context);
  return MDBX_RESULT_FALSE;
}

/* The core regression: park and unpark must refuse a read transaction
 * without a reader slot, the transaction must stay usable. */
TEST(issue_gh0316, park_unpark_eperm) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const files_remover cleanup;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi main = 0, dups = 0, fixed = 0;
  const int setup = make_withoutlck_ro(&env, &txn, &main, &dups, &fixed);
  std::unique_ptr<MDBX_env, env_closer> env_owner(setup == MDBX_SUCCESS ? env : nullptr);
  std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(setup == MDBX_SUCCESS ? txn : nullptr);
  if (setup == MDBX_RESULT_TRUE) {
    GTEST_SKIP() << "without-lck mode not reached (the lock file is writable, e.g. when running as root)";
    return;
  }
  ASSERT_EQ(setup, MDBX_SUCCESS);

  EXPECT_EQ(MDBX_EPERM, mdbx_txn_park(txn, false));
  EXPECT_EQ(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED) << "a refused park leaves no parked flag";
  EXPECT_EQ(MDBX_EPERM, mdbx_txn_park(txn, true)) << "autounpark makes no difference";
  /* nothing was parked (the park above was refused), so the unpark is a
   * quiet no-op success */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_unpark(txn, false));

  /* the transaction remains fully usable after the refusals */
  MDBX_val key = val_of("k1"), data;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn, main, &key, &data));
  EXPECT_EQ(data.iov_len, strlen("v1"));
  EXPECT_EQ(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED);
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn_owner.release();
}

/* A throttled copy parks the read transaction internally; in without-lck
 * mode the park is refused and the throttling silently skipped: the copy
 * proceeds and the caller's transaction stays untouched. */
TEST(issue_gh0316, copy_throttle_mvcc_unthrottled) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const files_remover cleanup;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi main = 0, dups = 0, fixed = 0;
  const int setup = make_withoutlck_ro(&env, &txn, &main, &dups, &fixed);
  std::unique_ptr<MDBX_env, env_closer> env_owner(setup == MDBX_SUCCESS ? env : nullptr);
  std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(setup == MDBX_SUCCESS ? txn : nullptr);
  if (setup == MDBX_RESULT_TRUE) {
    GTEST_SKIP() << "without-lck mode not reached (the lock file is writable, e.g. when running as root)";
    return;
  }
  ASSERT_EQ(setup, MDBX_SUCCESS);

  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_copy(env, kCopy.c_str(), MDBX_CP_THROTTLE_MVCC));
  EXPECT_EQ(0, mdbx_txn_flags(txn) & MDBX_TXN_PARKED) << "the copy must not park the transaction";

  /* the copy is usable and reflects the snapshot */
  MDBX_env *copy_env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&copy_env));
  const std::unique_ptr<MDBX_env, env_closer> copy_owner(copy_env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(copy_env, kCopy.c_str(), MDBX_NOSUBDIR | MDBX_RDONLY, 0));
  MDBX_txn *copy_txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(copy_env, nullptr, MDBX_TXN_RDONLY, &copy_txn));
  const std::unique_ptr<MDBX_txn, txn_aborter> copy_txn_owner(copy_txn);
  MDBX_dbi copy_main = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(copy_txn, nullptr, MDBX_DB_DEFAULTS, &copy_main));
  MDBX_val key = val_of("k1"), data;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(copy_txn, copy_main, &key, &data));
  EXPECT_EQ(data.iov_len, strlen("v1"));
}

/* The API sweep for a read-only transaction in without-lck mode: transaction
 * operations, env information/operations and the write refusals. */
TEST(issue_gh0316, withoutlck_ro_txn_and_env_ops) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const files_remover cleanup;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi main = 0, dups = 0, fixed = 0;
  const int setup = make_withoutlck_ro(&env, &txn, &main, &dups, &fixed);
  std::unique_ptr<MDBX_env, env_closer> env_owner(setup == MDBX_SUCCESS ? env : nullptr);
  std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(setup == MDBX_SUCCESS ? txn : nullptr);
  if (setup == MDBX_RESULT_TRUE) {
    GTEST_SKIP() << "without-lck mode not reached (the lock file is writable, e.g. when running as root)";
    return;
  }
  ASSERT_EQ(setup, MDBX_SUCCESS);

  /* transaction information, with and without the (expensive) reader-table scan */
  MDBX_txn_info tinfo = {};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_info(txn, &tinfo, false));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_info(txn, &tinfo, true));
  EXPECT_NE(0, mdbx_txn_flags(txn) & MDBX_TXN_RDONLY);

  /* no stale readers could exist in without-lck mode */
  int dead = -1;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_reader_check(env, &dead));
  EXPECT_EQ(0, dead);

  /* a read-only environment has nothing to sync */
  EXPECT_EQ(MDBX_EACCESS, mdbx_env_sync_ex(env, true, false));

  /* the env-level information and statistics are available */
  MDBX_envinfo envinfo = {};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, txn, &envinfo, sizeof(envinfo)));
  MDBX_stat envstat = {};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_stat_ex(env, txn, &envstat, sizeof(envstat)));

  /* the write-side operations on the read-only environment are refused */
  MDBX_val key = val_of("k1"), data = val_of("x");
  EXPECT_EQ(MDBX_EACCESS, mdbx_put(txn, main, &key, &data, MDBX_UPSERT));
  EXPECT_EQ(MDBX_EACCESS, mdbx_del(txn, main, &key, nullptr));
  MDBX_dbi tmp = 0;
  EXPECT_EQ(MDBX_EACCESS, mdbx_dbi_open(txn, "new", MDBX_CREATE, &tmp));
  EXPECT_EQ(MDBX_EACCESS, mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, true));
  EXPECT_NE(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, -1));

  /* the read transaction stays usable */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn, main, &key, &data));
  EXPECT_EQ(data.iov_len, strlen("v1"));
}

/* The cursor sweep for a read-only transaction in without-lck mode:
 * positioning over the plain and the dupsort/fixed tables. */
TEST(issue_gh0316, withoutlck_ro_cursor_ops) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const files_remover cleanup;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi main = 0, dups = 0, fixed = 0;
  const int setup = make_withoutlck_ro(&env, &txn, &main, &dups, &fixed);
  std::unique_ptr<MDBX_env, env_closer> env_owner(setup == MDBX_SUCCESS ? env : nullptr);
  std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(setup == MDBX_SUCCESS ? txn : nullptr);
  if (setup == MDBX_RESULT_TRUE) {
    GTEST_SKIP() << "without-lck mode not reached (the lock file is writable, e.g. when running as root)";
    return;
  }
  ASSERT_EQ(setup, MDBX_SUCCESS);

  /* the plain table: positioning and the traversal. NOTE: the named tables
   * ("dups", "fixed") are stored as records of the main table, so they are
   * visible to the main-table cursor as well */
  const MDBX_val expected_keys[] = {val_of("dups"), val_of("fixed"), val_of("k1"), val_of("k2"), val_of("k3")};
  MDBX_cursor *cur = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, main, &cur));
  MDBX_val key, data;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_FIRST));
  EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[0].iov_base, key.iov_len));
  for (int i = 1; i < 5; ++i) {
    EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_NEXT));
    EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[i].iov_base, key.iov_len)) << "i " << i;
  }
  EXPECT_EQ(MDBX_NOTFOUND, mdbx_cursor_get(cur, &key, &data, MDBX_NEXT));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_LAST));
  EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[4].iov_base, key.iov_len));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_PREV));
  EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[3].iov_base, key.iov_len));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_PREV_NODUP));
  EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[2].iov_base, key.iov_len));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_PREV_NODUP));
  EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[1].iov_base, key.iov_len));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_PREV_NODUP));
  EXPECT_EQ(0, memcmp(key.iov_base, expected_keys[0].iov_base, key.iov_len));
  EXPECT_EQ(MDBX_NOTFOUND, mdbx_cursor_get(cur, &key, &data, MDBX_PREV));

  key = val_of("k2");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_SET));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_GET_CURRENT));
  EXPECT_EQ(0, memcmp(key.iov_base, val_of("k2").iov_base, key.iov_len));
  key = val_of("k2x");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_SET_RANGE));
  EXPECT_EQ(0, memcmp(key.iov_base, val_of("k3").iov_base, key.iov_len));
  key = val_of("zz");
  EXPECT_EQ(MDBX_NOTFOUND, mdbx_cursor_get(cur, &key, &data, MDBX_SET_RANGE));
  mdbx_cursor_close(cur);

  /* the dupsort table: the dup-operations */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dups, &cur));
  key = val_of("A");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_SET));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_FIRST_DUP));
  EXPECT_EQ(0, memcmp(data.iov_base, val_of("a1").iov_base, data.iov_len));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_NEXT_DUP));
  EXPECT_EQ(0, memcmp(data.iov_base, val_of("a2").iov_base, data.iov_len));
  EXPECT_EQ(MDBX_NOTFOUND, mdbx_cursor_get(cur, &key, &data, MDBX_NEXT_DUP));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_LAST_DUP));
  EXPECT_EQ(0, memcmp(data.iov_base, val_of("a2").iov_base, data.iov_len));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_PREV_DUP));
  EXPECT_EQ(0, memcmp(data.iov_base, val_of("a1").iov_base, data.iov_len));
  key = val_of("A");
  data = val_of("a2");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_GET_BOTH));
  key = val_of("A");
  data = val_of("a1");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_GET_BOTH_RANGE));
  EXPECT_EQ(0, memcmp(data.iov_base, val_of("a1").iov_base, data.iov_len));
  size_t count = 0;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_count(cur, &count));
  EXPECT_EQ(2u, count);
  size_t scanned = 0;
  EXPECT_EQ(MDBX_RESULT_FALSE, mdbx_cursor_scan(cur, scan_count, &scanned, MDBX_FIRST, MDBX_NEXT, nullptr));
  /* the scan is through: 2 dups of "A" + 1 dup of "B" */
  EXPECT_EQ(3u, scanned);
  mdbx_cursor_close(cur);

  /* the dupfixed table: the multiple-values batch retrieval (the cursor
   * must be positioned on the key first) */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, fixed, &cur));
  key = val_of("f1");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_SET));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_GET_MULTIPLE));
  EXPECT_EQ(2 * 16u, data.iov_len);
  mdbx_cursor_close(cur);
}

/* Several read-only transactions of the same without-lck environment coexist
 * (no reader slots, no coordination needed), the cursors may be moved between
 * them. */
TEST(issue_gh0316, withoutlck_ro_multi_txn) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const files_remover cleanup;
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi main = 0, dups = 0, fixed = 0;
  const int setup = make_withoutlck_ro(&env, &txn, &main, &dups, &fixed);
  std::unique_ptr<MDBX_env, env_closer> env_owner(setup == MDBX_SUCCESS ? env : nullptr);
  std::unique_ptr<MDBX_txn, txn_aborter> txn_owner(setup == MDBX_SUCCESS ? txn : nullptr);
  if (setup == MDBX_RESULT_TRUE) {
    GTEST_SKIP() << "without-lck mode not reached (the lock file is writable, e.g. when running as root)";
    return;
  }
  ASSERT_EQ(setup, MDBX_SUCCESS);

  MDBX_txn *txn2 = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn2));
  const std::unique_ptr<MDBX_txn, txn_aborter> txn2_owner(txn2);

  /* both transactions read the same data */
  MDBX_val key = val_of("k2"), data;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn, main, &key, &data));
  EXPECT_EQ(data.iov_len, strlen("v2"));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn2, main, &key, &data));
  EXPECT_EQ(data.iov_len, strlen("v2"));

  /* a cursor may be bound/renewed to the other transaction */
  MDBX_cursor *cur = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, main, &cur));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_bind(txn2, cur, main));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_FIRST));
  EXPECT_EQ(0, memcmp(key.iov_base, val_of("dups").iov_base, key.iov_len))
      << "the first record of the main table is the named-table record";
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_renew(txn, cur));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cur, &key, &data, MDBX_FIRST));
  EXPECT_EQ(0, memcmp(key.iov_base, val_of("dups").iov_base, key.iov_len));

  /* a copy of the cursor */
  MDBX_cursor *copy = mdbx_cursor_create(nullptr);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_copy(cur, copy));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_get(copy, &key, &data, MDBX_FIRST));
  EXPECT_EQ(0, memcmp(key.iov_base, val_of("dups").iov_base, key.iov_len));
  mdbx_cursor_close(copy);

  /* aborting one transaction does not affect the other */
  mdbx_cursor_close(cur);
  mdbx_txn_abort(txn);
  txn_owner.release();
  key = val_of("k2");
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn2, main, &key, &data));
  EXPECT_EQ(data.iov_len, strlen("v2"));
}
} // namespace
