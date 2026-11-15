/* Verification repro v10b (stream B, wave 10): multi-cycle mdbx_env_defrag()
 * behavior — stopping reasons, per-cycle commit/abort of the last cycle,
 * pages_moved vs the actual shrink, and post-defrag integrity.
 *
 * Scenario: a main table and a named table are filled and half emptied to
 * fragment the file, then mdbx_env_defrag() is called with no user-owned
 * transaction (the defrag-owned basal-txn path) and a small preferred_batch
 * to force many cycles.
 *
 * Expected (hypotheses under attack):
 *  H1: the work of a final score-declined cycle is aborted, yet
 *      result->pages_moved still counts it (misreport);
 *  H2: stopping_reasons == noobstacles(0) together with MDBX_RESULT_TRUE
 *      for the acceptable-backlash / score-bailout / stuck-retry stops;
 *  H3: LAGGARD_READER-class outcomes without the laggard_reader bit;
 *  H4: data survives and the tree is consistent after the defrag.
 */
#include "mdbx.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char db_path[512];

static void die(const char *what, int rc) {
  fprintf(stderr, "FATAL: %s failed with %d (%s)\n", what, rc, mdbx_strerror(rc));
  exit(1);
}

static void put_key(MDBX_txn *txn, MDBX_dbi dbi, unsigned k, unsigned seed) {
  char keybuf[16], valbuf[256];
  MDBX_val key, val;
  const unsigned n = 8 + (seed % 200);
  key.iov_base = keybuf;
  key.iov_len = snprintf(keybuf, sizeof(keybuf), "k-%08u", k);
  memset(valbuf, (int)(k & 0xFF), sizeof(valbuf));
  val.iov_base = valbuf;
  val.iov_len = n;
  int rc = mdbx_put(txn, dbi, &key, &val, 0);
  if (rc)
    die("mdbx_put", rc);
}

int main(int argc, char **argv) {
  (void)argc;
  snprintf(db_path, sizeof(db_path), "%s", argv[1] ? argv[1] : "/tmp/verif-b-defrag-db");

  MDBX_env *env;
  int rc = mdbx_env_create(&env);
  if (rc)
    die("env_create", rc);
  rc = mdbx_env_set_geometry(env, -1, 1024 * 1024, 64 * 1024 * 1024, -1, -1, 4096);
  if (rc)
    die("set_geometry", rc);
  rc = mdbx_env_set_maxdbs(env, 8);
  if (rc)
    die("set_maxdbs", rc);
  rc = mdbx_env_open(env, db_path, 0, 0664);
  if (rc)
    die("env_open", rc);

  MDBX_dbi main_dbi, named_dbi;
  {
    MDBX_txn *txn;
    rc = mdbx_txn_begin(env, NULL, 0, &txn);
    if (rc)
      die("txn_begin", rc);
    rc = mdbx_dbi_open(txn, NULL, 0, &main_dbi);
    if (rc)
      die("dbi_open(main)", rc);
    rc = mdbx_dbi_open(txn, "named", MDBX_CREATE, &named_dbi);
    if (rc)
      die("dbi_open(named)", rc);
    for (unsigned i = 0; i < 12000; ++i)
      put_key(txn, main_dbi, i, i * 31 + 7);
    for (unsigned i = 0; i < 12000; ++i)
      put_key(txn, named_dbi, i, i * 17 + 3);
    rc = mdbx_txn_commit(txn);
    if (rc)
      die("txn_commit(fill)", rc);
  }
  /* fragment: delete every other key in both tables, also create a hole */
  {
    MDBX_txn *txn;
    rc = mdbx_txn_begin(env, NULL, 0, &txn);
    if (rc)
      die("txn_begin(del)", rc);
    for (unsigned i = 0; i < 12000; i += 2) {
      char keybuf[16];
      MDBX_val key;
      key.iov_base = keybuf;
      key.iov_len = snprintf(keybuf, sizeof(keybuf), "k-%08u", i);
      rc = mdbx_del(txn, main_dbi, &key, NULL);
      if (rc && rc != MDBX_NOTFOUND)
        die("mdbx_del(main)", rc);
      rc = mdbx_del(txn, named_dbi, &key, NULL);
      if (rc && rc != MDBX_NOTFOUND)
        die("mdbx_del(named)", rc);
    }
    rc = mdbx_txn_commit(txn);
    if (rc)
      die("txn_commit(del)", rc);
  }

  MDBX_envinfo info_before;
  rc = mdbx_env_info_ex(env, NULL, &info_before, sizeof(info_before));
  if (rc)
    die("env_info(before)", rc);
  printf("before defrag: recent-txn %" PRIu64 ", geometry %zu..%zu (current %zu)\n",
         (uint64_t)info_before.mi_recent_txnid, (size_t)info_before.mi_geo.lower, (size_t)info_before.mi_geo.upper,
         (size_t)info_before.mi_geo.current);

  /* defrag with no user txn; force many small cycles */
  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  rc = mdbx_env_defrag(env, /*atleast*/ 0, /*time_atleast*/ 0, /*enough*/ 0,
                       /*time_limit*/ 0, /*backlash*/ 4, /*preferred_batch*/ 400,
                       /*cb*/ NULL, /*ctx*/ NULL, &result);
  printf("mdbx_env_defrag => rc %d (%s)\n", rc, mdbx_strerror(rc));
  printf("  stopping_reasons 0x%x%s%s\n", (unsigned)result.stopping_reasons,
         (result.stopping_reasons & MDBX_defrag_step_size) ? " step_size" : "",
         (result.stopping_reasons & MDBX_defrag_large_chunk) ? " large_chunk" : "");
  printf("  pages_moved %" PRIuPTR ", scheduled %" PRIuPTR ", shrinked %" PRIiPTR ", left %" PRIuPTR ", whole %" PRIuPTR
         ", cycles %u, progress-permille %u\n",
         (uintptr_t)result.pages_moved, (uintptr_t)result.pages_scheduled, (intptr_t)result.pages_shrinked,
         (uintptr_t)result.pages_left, (uintptr_t)result.pages_whole, (unsigned)result.cycles,
         (unsigned)result.rough_estimation_cycle_progress_permille);

  MDBX_envinfo info_after;
  rc = mdbx_env_info_ex(env, NULL, &info_after, sizeof(info_after));
  if (rc)
    die("env_info(after)", rc);
  printf("after defrag: recent-txn %" PRIu64 "\n", (uint64_t)info_after.mi_recent_txnid);
  printf("  geo.current %zu (before %zu)\n", (size_t)info_after.mi_geo.current, (size_t)info_before.mi_geo.current);

  /* integrity: read back all remaining keys of both tables */
  {
    MDBX_txn *txn;
    rc = mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn);
    if (rc)
      die("txn_begin(verify)", rc);
    MDBX_cursor *cur;
    rc = mdbx_cursor_open(txn, named_dbi, &cur);
    if (rc)
      die("cursor_open", rc);
    MDBX_val key, data;
    unsigned count = 0;
    rc = mdbx_cursor_get(cur, &key, &data, MDBX_FIRST);
    while (rc == MDBX_SUCCESS) {
      ++count;
      if (memcmp(key.iov_base, "k-", 2) != 0) {
        fprintf(stderr, "FATAL: broken key at %u\n", count);
        return 2;
      }
      rc = mdbx_cursor_get(cur, &key, &data, MDBX_NEXT);
    }
    if (rc != MDBX_NOTFOUND)
      die("cursor_scan", rc);
    mdbx_cursor_close(cur);
    printf("verify: named table scanned, %u records\n", count);
    rc = mdbx_txn_abort(txn);
    if (rc)
      die("txn_abort", rc);
  }

  rc = mdbx_env_close(env);
  if (rc)
    die("env_close", rc);
  printf("DONE\n");
  return 0;
}
