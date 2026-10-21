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

#include <stdio.h>
#include <stdlib.h>

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

static void check(int rc, const char *op) {
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "%s: %d (%s)\n", op, rc, mdbx_strerror(rc));
    exit(1);
  }
}

static int get(MDBX_txn *txn, MDBX_dbi dbi) {
  MDBX_val key = {(void *)"k", 1};
  MDBX_val val;
  return mdbx_get(txn, dbi, &key, &val);
}

TEST(ut_nested_drop_abort, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *dbfile = "nested_drop_abort";
  MDBX_env *env;
  mdbx_env_delete(dbfile, MDBX_ENV_JUST_DELETE);
  check(mdbx_env_create(&env), "env_create");
  check(mdbx_env_set_maxdbs(env, 16), "set_maxdbs");
  check(mdbx_env_open(env, dbfile, MDBX_NOSUBDIR, 0660), "env_open");

  MDBX_txn *parent;
  check(mdbx_txn_begin(env, NULL, (MDBX_txn_flags_t)0, &parent), "begin parent");

  MDBX_dbi dbi;
  check(mdbx_dbi_open(parent, "t", MDBX_CREATE, &dbi), "create t");
  MDBX_val key = {(void *)"k", 1};
  MDBX_val val = {(void *)"v", 1};
  check(mdbx_put(parent, dbi, &key, &val, (MDBX_put_flags_t)0), "put");
  check(get(parent, dbi), "get before child");

  MDBX_txn *child;
  check(mdbx_txn_begin(env, parent, (MDBX_txn_flags_t)0, &child), "begin child");
  check(mdbx_drop(child, dbi, true), "drop in child");
  check(mdbx_txn_abort(child), "abort child");

  int old_rc = get(parent, dbi);
  MDBX_dbi reopened;
  int open_rc = mdbx_dbi_open(parent, "t", (MDBX_db_flags_t)0, &reopened);
  int get_rc = open_rc == MDBX_SUCCESS ? get(parent, reopened) : open_rc;

  printf("libmdbx %u.%u.%u.%u (%s)\n", mdbx_version.major, mdbx_version.minor, mdbx_version.patch, mdbx_version.tweak,
         mdbx_version.git.describe);
  printf("old DBI after child abort: %d (%s)\n", old_rc, mdbx_strerror(old_rc));
  printf("reopen after child abort: %d (%s)\n", open_rc, mdbx_strerror(open_rc));
  printf("get through reopened DBI: %d (%s)\n", get_rc, mdbx_strerror(get_rc));

  check(mdbx_txn_abort(parent), "abort parent");
  check(mdbx_env_close(env), "env_close");
}
