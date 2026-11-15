/* v3c-put-dangling-cursor.c — F-C5 repro (stream C wave 3, pair 4).
 *
 * mdbx_put() (src/api-txn-data.c) registers its stack cursor_couple_t into
 * txn->cursors[dbi] at line 282-283, but the MDBX_CURRENT size-precheck at
 * lines 294-301 performs a bare `return LOG_IFERR(MDBX_BAD_VALSIZE);` that
 * bypasses the unregister at line 311.  The dead stack frame stays linked in
 * txn->cursors[dbi]; every subsequent cursor iteration on that dbi (m2/m3
 * loops of node_move/page_merge/cursor_del/insert) and txn_done_cursors() at
 * txn end dereferences the dangling pointer.
 *
 * Scenario: MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP table (v.clc lmin == lmax == 4);
 * a key with >= 2 dups (node has N_DUP); mdbx_put(..., MDBX_CURRENT) with
 * data->iov_len = 5 (out of the [4;4] bound).  MDBX_ALLDUPS and
 * MDBX_NOOVERWRITE are both unset, so the precheck branch is entered and the
 * bare return fires.  mdbx_replace_ex() uses `goto bailout` for the same
 * situation and is clean -- included for contrast.
 *
 * Trigger: mdbx_del() in the SAME txn after the failed put -- cursor_touch
 * walks the tracked cursors' stacks (page_touch_unmodifiable) and reads the
 * dead mdbx_put frame.  Expected with clang + ASAN
 * detect_stack_use_after_return=1: "AddressSanitizer: stack-use-after-return
 * ... in frame mdbx_put ... 'cx' (line 265)".  Without ASAN the run corrupts
 * or crashes later; without the mdbx_del the leak is silent.
 */
#include "mdbx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char *what, int rc) {
  {
    char b[64];
    mdbx_strerror_r(rc, b, sizeof(b));
    fprintf(stderr, "FAIL %s: %d (%s)\n", what, rc, b);
  }
  exit(2);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char dbuf[] = "/tmp/v3c-put-dangling.db";
  unlink(dbuf);
  MDBX_env *env = NULL;
  int rc = mdbx_env_create(&env);
  if (rc)
    die("env_create", rc);
  mdbx_env_set_maxdbs(env, 16);
  rc = mdbx_env_open(env, dbuf, MDBX_NOSUBDIR, 0644);
  if (rc)
    die("env_open", rc);

  MDBX_txn *txn = NULL;
  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc)
    die("begin", rc);
  MDBX_dbi dbi = 0;
  rc = mdbx_dbi_open(txn, "t", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_CREATE, &dbi);
  if (rc)
    die("dbi_open", rc);

  char key1[] = "k";
  int32_t val4 = 42, val4b = 43;
  MDBX_val k = {key1, 1}, v4 = {&val4, 4}, v4b = {&val4b, 4};
  /* two dups => the outer node carries N_DUP */
  rc = mdbx_put(txn, dbi, &k, &v4, 0);
  if (rc)
    die("put#1", rc);
  rc = mdbx_put(txn, dbi, &k, &v4b, 0);
  if (rc)
    die("put#2", rc);
  rc = mdbx_txn_commit(txn);
  if (rc)
    die("commit#1", rc);

  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc)
    die("begin#2", rc);

  /* the poison: new data length 5 is outside [4;4] */
  int64_t val5 = 0;
  MDBX_val v5 = {&val5, 5};
  rc = mdbx_put(txn, dbi, &k, &v5, MDBX_CURRENT);
  {
    char b[64];
    mdbx_strerror_r(rc, b, sizeof(b));
    printf("mdbx_put(MDBX_CURRENT, out-of-bound length) => %d (%s)\n", rc, b);
  }
  assert(rc == MDBX_BAD_VALSIZE);

  /* mdbx_put returned, but its stack cursor_couple_t is still linked in
   * txn->cursors[dbi].  A subsequent cursor op on the same dbi walks the
   * list in its m3-adjustment loops: */
  printf("mdbx_del in the same txn (cursor_del m3-loop walks the dangling list)...\n");
  rc = mdbx_del(txn, dbi, &k, NULL);
  printf("mdbx_del => %d\n", rc);
  rc = mdbx_txn_abort(txn);
  printf("mdbx_txn_abort => %d\n", rc);

  /* For contrast: mdbx_replace_ex() with the same data length is clean. */
  rc = mdbx_txn_begin(env, NULL, 0, &txn);
  if (rc)
    die("begin#3", rc);
  MDBX_val old = {NULL, 0};
  rc = mdbx_replace(txn, dbi, &k, &v5, &old, MDBX_CURRENT);
  printf("mdbx_replace(same out-of-bound length) => %d\n", rc);
  rc = mdbx_txn_abort(txn);
  printf("abort#2 => %d\n", rc);

  mdbx_env_close(env);
  unlink(dbuf);
  printf("done -- if this line is reached without an ASAN report, the leak did not crash this build\n");
  return 0;
}
