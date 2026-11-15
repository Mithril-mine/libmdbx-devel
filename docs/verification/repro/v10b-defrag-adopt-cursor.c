/* Verification repro v10b (stream B, wave 10), scenario 3:
 * mdbx_env_defrag() called while the caller's write transaction is open
 * (#306 adoption path) with an open user cursor.
 *
 * Checks:
 *  1. The defrag adopts the txn: per-cycle txn_basal_checkpoint() commits and
 *     RESTARTS the caller's txn (the user's pending writes become durable
 *     before mdbx_txn_commit()).
 *  2. The user's cursor is detached (cursor_eot -> drown -> ready4dispose):
 *     the next cursor_get() must fail safely (EINVAL), not crash / not
 *     return stale data from pre-defrag page numbers.
 *  3. The txn remains usable and commit works after the defrag.
 */
#include "mdbx.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  const char *db_path = argv[1] ? argv[1] : "/tmp/verif-b-adopt-db";

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

  MDBX_dbi main_dbi;
  {
    MDBX_txn *txn;
    rc = mdbx_txn_begin(env, NULL, 0, &txn);
    if (rc)
      die("txn_begin", rc);
    rc = mdbx_dbi_open(txn, NULL, 0, &main_dbi);
    if (rc)
      die("dbi_open", rc);
    for (unsigned i = 0; i < 12000; ++i)
      put_key(txn, main_dbi, i, i * 31 + 7);
    rc = mdbx_txn_commit(txn);
    if (rc)
      die("txn_commit(fill)", rc);
  }
  {
    MDBX_txn *txn;
    rc = mdbx_txn_begin(env, NULL, 0, &txn);
    if (rc)
      die("txn_begin", rc);
    for (unsigned i = 0; i < 12000; i += 2) {
      char keybuf[16];
      MDBX_val key;
      key.iov_base = keybuf;
      key.iov_len = snprintf(keybuf, sizeof(keybuf), "k-%08u", i);
      rc = mdbx_del(txn, main_dbi, &key, NULL);
      if (rc && rc != MDBX_NOTFOUND)
        die("mdbx_del", rc);
    }
    rc = mdbx_txn_commit(txn);
    if (rc)
      die("txn_commit(del)", rc);
  }

  /* user write txn with a pending put and an open cursor, then defrag */
  MDBX_txn *txn;
  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc)
    die("txn_begin(user)", rc);
  put_key(txn, main_dbi, 5, 42);
  MDBX_cursor *cur;
  rc = mdbx_cursor_open(txn, main_dbi, &cur);
  if (rc)
    die("cursor_open", rc);
  MDBX_val key, data;
  rc = mdbx_cursor_get(cur, &key, &data, MDBX_FIRST);
  if (rc)
    die("cursor_get(before)", rc);
  printf("cursor before defrag: key %.*s\n", (int)key.iov_len, (char *)key.iov_base);

  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  rc = mdbx_env_defrag(env, 0, 0, 0, 0, 4, 300, NULL, NULL, &result);
  printf("mdbx_env_defrag (adopted txn) => rc %d (%s), cycles %u, moved %" PRIuPTR "\n", rc, mdbx_strerror(rc),
         (unsigned)result.cycles, (uintptr_t)result.pages_moved);

  /* 2: cursor must fail safely */
  rc = mdbx_cursor_get(cur, &key, &data, MDBX_FIRST);
  printf("cursor_get after defrag => rc %d (%s)%s\n", rc, mdbx_strerror(rc),
         (rc == MDBX_EINVAL || rc == MDBX_EBADSIGN || rc == MDBX_BAD_TXN)
             ? " [safe refusal]"
             : (rc ? " [UNEXPECTED rc]" : " [PROBLEM: stale data served]"));

  /* 3: the defrag's per-cycle checkpoints committed the user's pending
   * writes mid-txn: abort the user's txn and check whether the put survived. */
  mdbx_txn_abort(txn);
  {
    MDBX_txn *rtxn;
    rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &rtxn);
    if (rc)
      die("txn_begin(reader)", rc);
    char keybuf[16];
    MDBX_val rkey = {.iov_base = keybuf, .iov_len = snprintf(keybuf, sizeof(keybuf), "k-%08u", 5)};
    rc = mdbx_get(rtxn, main_dbi, &rkey, &data);
    printf("user pending put survives txn_abort after defrag => %s\n",
           (rc == MDBX_SUCCESS)    ? "YES (was checkpointed mid-txn by defrag)"
           : (rc == MDBX_NOTFOUND) ? "no (rollback semantics kept)"
                                   : "err");
    if (rc && rc != MDBX_NOTFOUND)
      die("mdbx_get", rc);
    mdbx_txn_abort(rtxn);
  }

  mdbx_cursor_close(cur);

  rc = mdbx_env_close(env);
  if (rc)
    die("env_close", rc);
  printf("DONE\n");
  return 0;
}
