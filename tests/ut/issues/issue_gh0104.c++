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
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/104
// Parking a broken read transaction released its reader slot but kept the
// transaction live, so the later abort failed an assertion and terminated the
// process; and txn_ro_unpark() returned success on failure paths after having
// finished the transaction, hiding the problem from the caller.

TEST(issue_gh0104, park_broken_read_then_abort_is_safe) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue104.mdbx";
  remove(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  MDBX_txn *r = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &r));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_break(r));
  /* parking a broken read txn must finish it, not leave a live but slot-less
   * transaction whose abort would trip an assertion in txn_ro_reset() */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_park(r, false)) << "parking a broken read txn must succeed";
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(r)) << "aborting a parked broken read txn must not assert/crash";

  /* a fresh read txn must be fine afterwards */
  MDBX_txn *r2 = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &r2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(r2));

  mdbx_env_close(env);
  remove(testdb);
}

TEST(issue_gh0104, unpark_broken_read_reports_bad_txn) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue104b.mdbx";
  remove(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  MDBX_txn *r = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &r));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_park(r, false));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_break(r));
  /* unparking a broken transaction must report the failure (MDBX_BAD_TXN),
   * not success for a transaction that has been finished. */
  EXPECT_EQ(MDBX_BAD_TXN, mdbx_txn_unpark(r, false))
      << "unparking a broken read txn must report MDBX_BAD_TXN, not success";
  /* abandon the parked broken read txn so it does not leak under LSan */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(r)) << "aborting a parked broken read txn must not assert/crash";

  mdbx_env_close(env);
  remove(testdb);
}