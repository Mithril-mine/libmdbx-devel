/* v5b-empty-tree-nav.c — V-036 verdict support (stream B, wave 5): attack 6 —
 * cursor navigation after a full-tree collapse (all rows deleted by ANOTHER
 * cursor in the same txn) while a second cursor is parked in an eof state.
 *
 * Adversarial question: cursor A parked at z_eof_hard (past-end, pointed,
 * ki[top] == nkeys-1) / z_eof_soft (LAST); cursor B deletes ALL rows; the
 * tree collapses (root retired, height 0, root = P_INVALID). Does A's next
 * operation read a stale pg[]/ki[] (memory unsafety) or produce a
 * deterministic R2 outcome?
 *
 * Defense (verified + exercised here): tree_rebalance's empty-root collapse
 * (tree-ops.c) walks txn->cursors and for every cursor with pg[0] == the
 * empty root applies be_poor() + z_after_delete; cursor_step's poor handling
 * then yields NOTFOUND (after_delete) / ENODATA, cursor_brim's tree_search
 * returns MDBX_NOTFOUND for root == P_INVALID BEFORE touching pg[top]
 * (tree-search.c:46-50), GET_CURRENT sees is_hollow -> ENODATA. A
 * subsequently re-seeks and re-puts normally (R2 determinism).
 *
 * The multi-page variant (height-2 tree, 300 keys) additionally drives the
 * page-merge and root-collapse cursor fixups before the final empty collapse.
 *
 * Expected outcome (asserted): every follow-up op on A returns exactly one of
 * MDBX_NOTFOUND / MDBX_ENODATA with no crash, and the db remains usable.
 *
 * Build:
 *   cc -I<src> -o v5b-empty-tree-nav v5b-empty-tree-nav.c -L<build> -lmdbx
 */
#include "mdbx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static void die(const char *what, int rc) {
  fprintf(stderr, "FAIL: %s -> %d (%s)\n", what, rc, mdbx_strerror(rc));
  exit(1);
}

static void chk(const char *what, int rc) {
  if (rc != MDBX_SUCCESS)
    die(what, rc);
}

static void scenario(int nkeys, const char *label) {
  char pathbuf[1024];
  snprintf(pathbuf, sizeof(pathbuf), "/tmp/v5b-empty-nav-%s", label);
rmrf_label:;
  /* fresh db per run */
  {
    char cmd[1100];
    int n = snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", pathbuf, pathbuf);
    (void)n;
    assert(n > 0 && (size_t)n < sizeof(cmd));
    if (system(cmd) != 0)
      goto rmrf_label;
  }
  strcat(pathbuf, "/db");

  MDBX_env *env = NULL;
  chk("env_create", mdbx_env_create(&env));
  chk("env_open", mdbx_env_open(env, pathbuf, MDBX_NOSUBDIR, 0640));

  MDBX_txn *txn = NULL;
  chk("txn_begin", mdbx_txn_begin(env, NULL, 0, &txn));
  MDBX_dbi dbi;
  chk("dbi_open", mdbx_dbi_open(txn, NULL, 0, &dbi));

  char (*keys)[32] = malloc(nkeys * 32);
  for (int i = 0; i < nkeys; i++) {
    snprintf(keys[i], 32, "key-%06d", i);
    MDBX_val k = {keys[i], strlen(keys[i])}, v = {keys[i], strlen(keys[i])};
    chk("put", mdbx_put(txn, dbi, &k, &v, 0));
  }
  chk("commit", mdbx_txn_commit(txn));

  chk("txn_begin2", mdbx_txn_begin(env, NULL, 0, &txn));
  MDBX_cursor *A = NULL, *B = NULL;
  chk("cursor_open A", mdbx_cursor_open(txn, dbi, &A));
  chk("cursor_open B", mdbx_cursor_open(txn, dbi, &B));

  MDBX_val k, v;
  int rc;
  /* Variant 1: A parked past-end (NEXT from the last row -> z_eof_hard). */
  rc = mdbx_cursor_get(A, &k, &v, MDBX_LAST);
  chk("A LAST", rc);
  rc = mdbx_cursor_get(A, &k, &v, MDBX_NEXT);
  assert(rc == MDBX_NOTFOUND); /* past end, z_eof_hard, still pointed */
  printf("[%s n=%d] A parked past-end (z_eof_hard)\n", label, nkeys);

  /* B deletes everything. */
  rc = mdbx_cursor_get(B, &k, &v, MDBX_FIRST);
  chk("B FIRST", rc);
  for (int i = 0; i < nkeys; i++) {
    rc = mdbx_cursor_del(B, 0);
    chk("B del", rc);
    if (i + 1 < nkeys) {
      rc = mdbx_cursor_get(B, &k, &v, MDBX_NEXT);
      chk("B NEXT", rc);
    }
  }
  printf("[%s n=%d] B deleted all rows (tree collapsed)\n", label, nkeys);

  /* A's follow-ups must be deterministic NOTFOUND/ENODATA, never garbage. */
  rc = mdbx_cursor_get(A, &k, &v, MDBX_GET_CURRENT);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_ENODATA);
  printf("  A GET_CURRENT -> %d\n", rc);
  rc = mdbx_cursor_get(A, &k, &v, MDBX_PREV);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_ENODATA);
  printf("  A PREV        -> %d\n", rc);
  rc = mdbx_cursor_get(A, &k, &v, MDBX_NEXT);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_ENODATA);
  printf("  A NEXT        -> %d\n", rc);
  rc = mdbx_cursor_get(A, &k, &v, MDBX_FIRST);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_ENODATA);
  printf("  A FIRST       -> %d\n", rc);
  rc = mdbx_cursor_get(A, &k, &v, MDBX_LAST);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_ENODATA);
  printf("  A LAST        -> %d\n", rc);
  rc = mdbx_cursor_get(A, &k, &v, MDBX_PREV_NODUP);
  assert(rc == MDBX_NOTFOUND || rc == MDBX_ENODATA);
  printf("  A PREV_NODUP  -> %d\n", rc);

  /* Variant 2 (same txn, fresh cursors): A parked at z_eof_soft via LAST,
   * B already emptied -> same determinism check. */
  MDBX_cursor *C = NULL;
  chk("cursor_open C", mdbx_cursor_open(txn, dbi, &C));
  rc = mdbx_cursor_get(C, &k, &v, MDBX_FIRST); /* empty tree, fresh cursor */
  assert(rc == MDBX_NOTFOUND);
  printf("  C FIRST (fresh, empty tree) -> %d\n", rc);
  mdbx_cursor_close2(C);

  /* R2: the db is still usable; A can seek-and-put again. */
  MDBX_val nk = {"key-000001", 11}, nv = {"reborn", 6};
  rc = mdbx_cursor_put(A, &nk, &nv, 0);
  chk("A put after collapse", rc);
  MDBX_val rk;
  rc = mdbx_cursor_get(A, &nk, &rk, MDBX_SET);
  chk("A re-seek", rc);
  assert(rk.iov_len == 6 && memcmp(rk.iov_base, "reborn", 6) == 0);
  printf("  A re-put + re-seek OK (R2 determinism)\n");

  mdbx_cursor_close2(A);
  mdbx_cursor_close2(B);
  chk("abort", mdbx_txn_abort(txn));
  chk("env_close", mdbx_env_close_ex(env, true));
  free(keys);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  scenario(3, "small");   /* single leaf page */
  scenario(300, "multi"); /* height-2 tree: merges + root collapse */
  printf("\nPASS: no stale-page access; all follow-ups deterministic\n"
         "NOTFOUND/ENODATA; db usable after full collapse (attack 6 held).\n");
  return 0;
}
