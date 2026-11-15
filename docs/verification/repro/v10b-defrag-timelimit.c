/* Verification repro v10b (stream B, wave 10), scenario 2: mdbx_env_defrag()
 * time_limit behavior when the defrag_atleast goal is unreachable.
 *
 * mdbx.h documents time_limit_dot16 as a hard cap: "When the specified limit
 * is reached, defragmentation will not continue". defrag_progress() gates the
 * MDBX_defrag_time_limit bit on `defrag_atleast >= first_unallocated` (the
 * goal edge reached), so with an unreachable goal the cap is never evaluated.
 *
 * This repro builds a fragmented DB, then calls mdbx_env_defrag() with a tiny
 * time limit (a few milliseconds) and an unreachable atleast goal, and prints
 * the elapsed wallclock, the number of cycles and the stopping_reasons.
 * Expected: if the cap is honored, elapsed stays near the limit; if the gate
 * suppresses it, the call runs to the stuck/backlash stop far beyond it.
 */
#include "mdbx.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  const char *db_path = argv[1] ? argv[1] : "/tmp/verif-b-tl-db";

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

  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  /* 655 dot16-units ~= 10ms; the atleast goal (600 pages) is unreachable since
   * the reclaimable volume is smaller and obstacles stop the moves earlier. */
  const uint64_t t0 = (uint64_t)clock();
  rc = mdbx_env_defrag(env, /*atleast(unreachable)*/ 600, /*time_atleast*/ 0, /*enough*/ 0,
                       /*time_limit*/ 655, /*backlash*/ 4, /*preferred_batch*/ 20, NULL, NULL, &result);
  const uint64_t t1 = (uint64_t)clock();
  const double seconds = (double)(t1 - t0) / CLOCKS_PER_SEC;
  printf("mdbx_env_defrag => rc %d (%s), elapsed %.3f s\n", rc, mdbx_strerror(rc), seconds);
  printf("  stopping_reasons 0x%x%s%s%s%s\n", (unsigned)result.stopping_reasons,
         (result.stopping_reasons & MDBX_defrag_time_limit) ? " time_limit" : "",
         (result.stopping_reasons & MDBX_defrag_enough_threshold) ? " enough_threshold" : "",
         (result.stopping_reasons & MDBX_defrag_step_size) ? " step_size" : "",
         (result.stopping_reasons & MDBX_defrag_large_chunk) ? " large_chunk" : "");
  printf("  cycles %u, pages_moved %" PRIuPTR ", shrinked %" PRIiPTR ", left %" PRIuPTR "\n", (unsigned)result.cycles,
         (uintptr_t)result.pages_moved, (intptr_t)result.pages_shrinked, (uintptr_t)result.pages_left);
  printf(seconds > 1.0 ? "VERDICT: time_limit NOT honored (gate suppressed by unreachable goal)\n"
                       : "VERDICT: time_limit honored or work finished early\n");

  rc = mdbx_env_close(env);
  if (rc)
    die("env_close", rc);
  return 0;
}
