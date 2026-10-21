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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include <gtest/gtest.h>

#include "../test-env.h"

#include <mdbx.h>
#include <stdio.h>
#include <stdlib.h>

#define check_is(success, rc)                                                                                          \
  do {                                                                                                                 \
    int __rc = (rc);                                                                                                   \
    if (__rc != success) {                                                                                             \
      fprintf(stderr, "mdbx failed at %s:%d: %s\n", __FILE__, __LINE__, mdbx_strerror(__rc));                          \
      exit(EXIT_FAILURE);                                                                                              \
    }                                                                                                                  \
  } while (0)

#define check(rc) check_is(MDBX_SUCCESS, rc)

TEST(ut_dbi_nested_txn, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *db_filename = "./test-dbi-nested-txn";
  int err = mdbx_env_delete(db_filename, MDBX_ENV_JUST_DELETE);
  if (err)
    check_is(MDBX_RESULT_TRUE, err);

  MDBX_env *env;
  check(mdbx_env_create(&env));
  mdbx_env_set_maxdbs(env, 64);
  check(mdbx_env_open(env, db_filename, MDBX_NOSUBDIR, 0666));

  MDBX_txn *txn;
  check(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
  MDBX_txn *txn_nested;
  check(mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &txn_nested));
  MDBX_dbi dbi;
  check(mdbx_dbi_open(txn_nested, "test", MDBX_CREATE, &dbi));
  check(mdbx_txn_abort(txn_nested));
  check_is(MDBX_NOTFOUND, mdbx_dbi_open(txn, "test", (MDBX_db_flags_t)0, &dbi));
  check(mdbx_txn_abort(txn));

  check(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
  check_is(MDBX_NOTFOUND, mdbx_dbi_open(txn, "test", (MDBX_db_flags_t)0, &dbi));
  check(mdbx_txn_abort(txn));

  check(mdbx_env_close(env));
}
