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

/* The subtable root field sits at offset 8 inside a tree_t record:
 * u16 flags, u16 height, u32 dupfix_size, then pgno_t root. The offset is
 * provided by the testing API (mdbx_tst_tree_offset_root()), computed from
 * the live definition - see tests/AGENTS.md §8 and issue #519 stage 3. */
#include "../env-guard.h++"
#include <gtest/gtest.h>

#include "../test-env.h++"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *what, int rc) {
  fprintf(stderr, "%s: %s (%d)\n", what, mdbx_strerror(rc), rc);
  exit(2);
}

TEST(issue_gh0017, all) {
  const env_files_guard guard({"issue_gh0017"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *path = "issue_gh0017";
  mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  int rc;
  MDBX_env *env = NULL;
  rc = mdbx_env_create(&env);
  if (rc)
    die("env_create", rc);
  rc = mdbx_env_set_maxdbs(env, 4);
  if (rc)
    die("set_maxdbs", rc);
  rc = mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_WRITEMAP | MDBX_EXCLUSIVE, 0664);
  if (rc)
    die("env_open", rc);

  MDBX_txn *txn = NULL;
  rc = mdbx_txn_begin(env, NULL, (MDBX_txn_flags_t)0, &txn);
  if (rc)
    die("txn_begin(w)", rc);
  MDBX_dbi sub;
  rc = mdbx_dbi_open(txn, "s", MDBX_CREATE, &sub);
  if (rc)
    die("dbi_open(s)", rc);
  MDBX_val k = {(void *)"k", 1}, v = {(void *)"v", 1};
  rc = mdbx_put(txn, sub, &k, &v, (MDBX_put_flags_t)0);
  if (rc)
    die("put", rc);
  rc = mdbx_txn_commit(txn);
  if (rc)
    die("commit", rc);

  rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &txn);
  if (rc)
    die("txn_begin(r)", rc);
  MDBX_val name = {(void *)"s", 1}, treerec;
  rc = mdbx_get(txn, 1 /*MAIN_DBI*/, &name, &treerec);
  if (rc)
    die("get(MAIN,'s')", rc);
  uint32_t old_root, bad_root = 0x40000000u; /* far beyond first_unallocated, not P_INVALID */
  memcpy(&old_root, (char *)treerec.iov_base + mdbx_tst_tree_offset_root(), 4);
  memcpy((char *)treerec.iov_base + mdbx_tst_tree_offset_root(), &bad_root, 4);
  printf("corrupted subtable root pgno: %u -> %u\n", old_root, bad_root);
  mdbx_txn_abort(txn);

  printf("calling mdbx_env_chk()...\n");
  fflush(stdout);
  MDBX_chk_callbacks_t cb;
  memset(&cb, 0, sizeof(cb));
  MDBX_chk_context_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  rc = mdbx_env_chk(env, &cb, &ctx, MDBX_CHK_DEFAULTS, MDBX_chk_info, 0);
  printf("mdbx_env_chk returned %s (%d) -- NO CRASH (bug fixed?)\n", mdbx_strerror(rc), rc);
  mdbx_env_close(env);
}
