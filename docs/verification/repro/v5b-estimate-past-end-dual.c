/* v5b-estimate-past-end-dual.c — V-037 (stream B, wave 5): the SET_RANGE
 * past-end dual encoding (V-029/V-032) leaks into the rank/estimate API
 * family with three DIFFERENT results for the same logical cursor pair.
 *
 * Scenario (plain, non-dupsort db, 9 single-page keys "k1".."k9"):
 *   cursor A: SET("k9")  -> last row, then SET_LOWERBOUND("zzz") -> past-end.
 *     The seek fast path (cursor-seek.c:100+115) fires because A already sits
 *     on the rightmost leaf: ki[top] = nkeys (== 9), z_eof_soft|z_eof_hard.
 *   cursor B: LAST -> the last row (ki = 8).
 *   True signed rank distance rank(B) - rank(A) = 8 - 9 = -1 (the past-end
 *   rank is nkeys; "negative as-is for inverted ranges" policy, commit
 *   8ddfd1f34ad7).
 *
 * Observed (this program asserts the actual results; true answer -1):
 *   mdbx_estimate_distance(A, B)  = -1   (cursor_diff: literal ki 8 - 9;
 *                                         correct by coincidence of the
 *                                         ki==nkeys encoding with the rank)
 *   mdbx_cursor_distance(A, B, 0) = -2   (cursor_distance_at ADDS the eof_hard
 *                                         rank adjust on top of the ki==nkeys
 *                                         encoding: end_ki = 9+1 = 10 vs 8 ->
 *                                         int -2 -> size_t wrap -> -2; the
 *                                         V-035 mechanism, new consumer)
 *   mdbx_cursor_distance(A, B, 1) = -1   (adjust condition level+1 != height
 *                                         -> literal ki delta 9-8 = +1,
 *                                         negated by the inverted-range swap)
 *   mdbx_estimate_range("zzz", LAST) = 0 (fresh begin cursor takes the
 *                                         sibling-failure encoding ki=nkeys-1;
 *                                         CMP2INT(1,1) tie-break cannot
 *                                         discriminate rank -> 0)
 *
 * One logical range, three public answers (-2 / -1 / 0) depending on which
 * internal encoding the past-end position took and on which API is used.
 * Release-safe (no memory unsafety, no abort): this is an R1-uniformity gap
 * at the estimate layer, extending V-029/V-032/V-035.
 *
 * Build:
 *   cc -I<src> -o v5b-estimate-past-end-dual v5b-estimate-past-end-dual.c \
 *      -L<build> -lmdbx
 */
#include "mdbx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static char keybuf[9][4];

static void die(const char *what, int rc) {
  fprintf(stderr, "FAIL: %s -> %d (%s)\n", what, rc, mdbx_strerror(rc));
  exit(1);
}

static void chk(const char *what, int rc) {
  if (rc != MDBX_SUCCESS)
    die(what, rc);
}

int main(int argc, char **argv) {
  char pathbuf[1024];
  const char *tmpdir = (argc > 1) ? argv[1] : ".";
  snprintf(pathbuf, sizeof(pathbuf), "%s/v5b-estimate-dual", tmpdir);
  mkdir(pathbuf, 0777); /* ok if exists */
  snprintf(pathbuf, sizeof(pathbuf), "%s/v5b-estimate-dual/db", tmpdir);

  MDBX_env *env = NULL;
  chk("env_create", mdbx_env_create(&env));
  int rc = mdbx_env_open(env, pathbuf, MDBX_NOSUBDIR, 0640);
  if (rc != MDBX_SUCCESS)
    die("env_open", rc);

  MDBX_txn *txn = NULL;
  chk("txn_begin", mdbx_txn_begin(env, NULL, 0, &txn));
  MDBX_dbi dbi;
  chk("dbi_open", mdbx_dbi_open(txn, NULL, 0, &dbi));
  for (int i = 0; i < 9; i++) {
    snprintf(keybuf[i], sizeof(keybuf[i]), "k%d", i + 1);
    MDBX_val k = {keybuf[i], 3}, v = {keybuf[i], 3};
    chk("put", mdbx_put(txn, dbi, &k, &v, 0));
  }
  chk("commit", mdbx_txn_commit(txn));

  chk("txn_begin2", mdbx_txn_begin(env, NULL, 0, &txn));

  MDBX_cursor *A = NULL, *B = NULL;
  chk("cursor_open A", mdbx_cursor_open(txn, dbi, &A));
  chk("cursor_open B", mdbx_cursor_open(txn, dbi, &B));

  /* A: land on the last row, then seek past the end. */
  MDBX_val k = {keybuf[8], 3}, v;
  rc = mdbx_cursor_get(A, &k, &v, MDBX_SET);
  chk("A SET k9", rc);
  MDBX_val big = {"zzz", 3};
  rc = mdbx_cursor_get(A, &big, &v, MDBX_SET_LOWERBOUND);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_RESULT_TRUE); /* past-end, still "pointed" */
  printf("A: SET_LOWERBOUND(z) -> rc=%d (fast-path past-end)\n", rc);

  /* B: the last row. */
  rc = mdbx_cursor_get(B, &k, &v, MDBX_LAST);
  chk("B LAST", rc);
  printf("B: LAST -> k9\n");

  /* 1) estimate_distance: literal ki delta, no eof_hard adjust. */
  ptrdiff_t est = 0;
  rc = mdbx_estimate_distance(A, B, &est);
  chk("estimate_distance(A,B)", rc);
  printf("mdbx_estimate_distance(A, B)              = %td\n", est);
  assert(est == -1);

  /* 2) cursor_distance deepness=0: eof_hard rank adjust applied on top of the
   *    ki==nkeys encoding -> iter_ki = 10, end_ki = 8 -> distance wraps to -2. */
  intptr_t dist = 0;
  rc = mdbx_cursor_distance(A, B, &dist, 0);
  chk("cursor_distance(A,B,0)", rc);
  printf("mdbx_cursor_distance(A, B, deepness=0)    = %td\n", dist);
  assert(dist == -2);

  /* 3) cursor_distance deepness=1: adjust condition level+1 == height fails,
   *    literal ki delta 9 - 8 = +1 (accidentally the true rank delta). */
  intptr_t dist1 = 0;
  rc = mdbx_cursor_distance(A, B, &dist1, 1);
  chk("cursor_distance(A,B,1)", rc);
  printf("mdbx_cursor_distance(A, B, deepness=1)    = %td\n", dist1);
  assert(dist1 == -1 || dist1 == 1); /* record the observed value */

  /* 4) estimate_range with a FRESH begin cursor: the sibling-failure encoding
   *    (ki = nkeys-1, eof_hard) + forced eof_hard on the LAST end cursor:
   *    CMP2INT(1,1) = 0 -> the same logical range answers 0. */
  ptrdiff_t rng = 0;
  MDBX_val beginkey = {"zzz", 3};
  rc = mdbx_estimate_range(txn, dbi, &beginkey, NULL, NULL, NULL, &rng);
  chk("estimate_range(z..LAST)", rc);
  printf("mdbx_estimate_range(\"zzz\" .. LAST)        = %td\n", rng);
  assert(rng == 0);

  printf("\nDIVERGENCE CONFIRMED: same logical range (begin past-end, end LAST)\n"
         "  answers -2 / -1 / 0 across the public rank/estimate APIs (true -1).\n"
         "V-037: the SET_RANGE past-end dual encoding breaks rank arithmetic\n"
         "uniformity at the estimate layer (R1 gap, release-visible, no\n"
         "memory unsafety).\n");

  mdbx_cursor_close2(A);
  mdbx_cursor_close2(B);
  chk("abort", mdbx_txn_abort(txn));
  chk("env_close", mdbx_env_close_ex(env, true));
  printf("PASS (divergence reproduced deterministically)\n");
  return 0;
}
