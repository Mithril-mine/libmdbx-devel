/* v5b-multiple-batch.c — V-038 (stream B, wave 5): attack 1 — MDBX_MULTIPLE
 * batch insertion into a DUPFIXED db, mid-batch failure and accounting.
 *
 * Exercises cursor_put()'s batch loop (cursor-mut.c `batch_dupfix_continue`):
 *   E1. The *batch_dupfix_done counter and data[0].iov_base advance stay
 *       consistent when an element fails MID-BATCH (no past-buffer advance,
 *       no over-count), and an aborted txn discards the inserted prefix.
 *       Triggered via MDBX_NODUPDATA with a duplicated element.
 *   E2. The NODUPDATA-duplicate condition returns MDBX_KEYEXIST UNIFORMLY
 *       (inline hive at element 1 via eq_fast, and sub-page hive at elements
 *       2+ via the inner cursor_put rc returned as-is — dupsort_error's
 *       PROBLEM re-mapping is only reachable for insert_key, i.e. batch
 *       element 0). The txn remains usable; the committed prefix is exactly
 *       the done-count elements. (The V-038 inconsistency hypothesis was
 *       REFUTED by this program.)
 *   E3. Without NODUPDATA, duplicate elements are silently skipped and the
 *       done counter still counts them (documented "nothing to update"
 *       semantics), and data[0].iov_base never advances past the buffer.
 *
 * Build:
 *   cc -I<src> -o v5b-multiple-batch v5b-multiple-batch.c -L<build> -lmdbx
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

static void fresh_db(const char *path, MDBX_env **penv) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", path, path);
  if (system(cmd) != 0)
    die("cleanup", -1);
  char dbpath[1024];
  snprintf(dbpath, sizeof(dbpath), "%s/db", path);
  chk("env_create", mdbx_env_create(penv));
  chk("max_db", mdbx_env_set_option(*penv, MDBX_opt_max_db, 16));
  chk("env_open", mdbx_env_open(*penv, dbpath, MDBX_NOSUBDIR, 0640));
}

static int dupfix_cmp(const MDBX_val *a, const MDBX_val *b) {
  (void)a;
  (void)b;
  return 0; /* unused */
}

int main(void) {
  /* ---- E3: happy batch with in-vector duplicates, no NODUPDATA ---- */
  {
    MDBX_env *env;
    fresh_db("/tmp/v5b-mult-e3", &env);
    MDBX_txn *txn;
    chk("txn_begin", mdbx_txn_begin(env, NULL, 0, &txn));
    MDBX_dbi dbi;
    chk("dbi_open", mdbx_dbi_open(txn, "t1", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_CREATE, &dbi));
    MDBX_cursor *mc;
    chk("cursor_open", mdbx_cursor_open(txn, dbi, &mc));

    enum { N = 5 };
    uint64_t elems[N] = {100, 200, 200, 300, 200}; /* duplicates inside */
    MDBX_val key = {"batchkey", 8};
    MDBX_val data[2] = {{elems, 8}, {NULL, N}};
    int rc = mdbx_cursor_put(mc, &key, data, MDBX_MULTIPLE);
    chk("put MULTIPLE", rc);
    size_t done = data[1].iov_len;
    printf("E3: put(MULTIPLE) done=%zu (of %d, silent dup skip expected)\n", done, N);
    assert(done == N);
    /* the buffer pointer must not have advanced past the last element */
    assert((char *)data[0].iov_base <= (char *)elems + N * 8);
    /* exactly 3 distinct dups stored: 100, 200, 300 */
    MDBX_val k2 = key, v2;
    rc = mdbx_cursor_get(mc, &k2, &v2, MDBX_SET);
    chk("get", rc);
    size_t count = 0;
    chk("cursor_count", mdbx_cursor_count(mc, &count));
    printf("E3: hive items after batch = %zu (expect 3)\n", count);
    assert(count == 3);
    chk("commit", mdbx_txn_commit(txn));
    chk("env_close", mdbx_env_close_ex(env, true));
  }

  /* ---- E2/E1: NODUPDATA duplicate inside the vector ---- */
  {
    MDBX_env *env;
    fresh_db("/tmp/v5b-mult-e2", &env);
    MDBX_txn *txn;
    chk("txn_begin", mdbx_txn_begin(env, NULL, 0, &txn));
    MDBX_dbi dbi;
    chk("dbi_open", mdbx_dbi_open(txn, "t1", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_CREATE, &dbi));
    MDBX_cursor *mc;
    chk("cursor_open", mdbx_cursor_open(txn, dbi, &mc));

    /* Element 2 (index 1) == element 1 (index 0): the node is still the
     * single inline value (no N_DUP) -> eq_fast path -> KEYEXIST, clean. */
    enum { N = 3 };
    uint64_t elems[N] = {10, 10, 20};
    MDBX_val key = {"dupkey", 6};
    MDBX_val data[2] = {{elems, 8}, {NULL, N}};
    int rc = mdbx_cursor_put(mc, &key, data, MDBX_MULTIPLE | MDBX_NODUPDATA);
    printf("E2a: put(MULTIPLE|NODUPDATA, elem[1]==elem[0]) -> rc=%d done=%zu\n", rc, data[1].iov_len);
    assert(rc == MDBX_KEYEXIST); /* clean, txn usable, done=1 */
    assert(data[1].iov_len == 1);
    assert((char *)data[0].iov_base == (char *)elems + 8); /* base at the failed element */

    /* same key, but element[2] (sub-page path, N_DUP established) duplicates:
     * continue the batch from the failed element WITHOUT NODUPDATA first, so
     * the hive becomes {10, 20}. */
    MDBX_val data2[2] = {{elems + 1, 8}, {NULL, 2}}; /* 10, 20 */
    rc = mdbx_cursor_put(mc, &key, data2, MDBX_MULTIPLE);
    chk("resume batch (10,20)", rc);
    size_t count = 0;
    chk("cursor_count", mdbx_cursor_count(mc, &count));
    printf("E2b: hive items = %zu (expect 2: 10,20)\n", count);
    assert(count == 2);

    /* V-038 attack: the same NODUPDATA-duplicate condition, but now the hive
     * is a sub-page (N_DUP). Static prediction was: the inner cursor_put's
     * KEYEXIST is re-mapped by dupsort_error to MDBX_PROBLEM and the txn is
     * poisoned. OBSERVED (asserted below): MDBX_KEYEXIST — because
     * insert_key == false for batch elements 2+, so dupsort_error (the
     * PROBLEM re-mapper) is never reached; the inner rc is returned as-is.
     * The accounting stays consistent and the txn remains USABLE. */
    uint64_t dup3[3] = {30, 40, 20};
    MDBX_val data3[2] = {{dup3, 8}, {NULL, 3}};
    rc = mdbx_cursor_put(mc, &key, data3, MDBX_MULTIPLE | MDBX_NODUPDATA);
    printf("E2c: put(MULTIPLE|NODUPDATA, dup among sub-page dups) -> rc=%d done=%zu base@%+zd\n", rc, data3[1].iov_len,
           (char *)data3[0].iov_base - (char *)dup3);
    assert(rc == MDBX_KEYEXIST); /* uniform with E2a — V-038 hypothesis REFUTED */
    assert(data3[1].iov_len == 2);
    assert((char *)data3[0].iov_base == (char *)dup3 + 2 * 8); /* no past-buffer advance */

    /* the txn survived the mid-batch failure: commit the prefix {10,20,30,40} */
    chk("commit after mid-batch KEYEXIST", mdbx_txn_commit(txn));
    chk("txn_begin_ro", mdbx_txn_begin(env, NULL, MDBX_RDONLY, &txn));
    MDBX_dbi dbi_ro;
    chk("dbi_open ro", mdbx_dbi_open(txn, "t1", MDBX_DUPSORT | MDBX_DUPFIXED, &dbi_ro));
    MDBX_cursor *mr;
    chk("cursor_open ro", mdbx_cursor_open(txn, dbi_ro, &mr));
    MDBX_val k2 = {"dupkey", 6}, v2;
    chk("ro SET", mdbx_cursor_get(mr, &k2, &v2, MDBX_SET));
    chk("ro count", mdbx_cursor_count(mr, &count));
    printf("E2d: committed hive items = %zu (expect 4: 10,20,30,40)\n", count);
    assert(count == 4);
    mdbx_cursor_close2(mr);
    chk("abort ro", mdbx_txn_abort(txn));
    chk("env_close", mdbx_env_close_ex(env, true));
  }

  /* ---- E1: batch prefix rolled back on abort (not committed silently) ---- */
  {
    MDBX_env *env;
    fresh_db("/tmp/v5b-mult-e1", &env);
    MDBX_txn *txn;
    chk("txn_begin", mdbx_txn_begin(env, NULL, 0, &txn));
    MDBX_dbi dbi;
    chk("dbi_open", mdbx_dbi_open(txn, "t1", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_CREATE, &dbi));
    chk("commit table", mdbx_txn_commit(txn)); /* table must survive the later abort */

    chk("txn_begin2", mdbx_txn_begin(env, NULL, 0, &txn));
    MDBX_cursor *mc;
    chk("cursor_open", mdbx_cursor_open(txn, dbi, &mc));
    uint64_t elems[4] = {7, 7, 9, 11};
    MDBX_val key = {"prefixkey", 9};
    MDBX_val data[2] = {{elems, 8}, {NULL, 4}};
    int rc = mdbx_cursor_put(mc, &key, data, MDBX_MULTIPLE | MDBX_NODUPDATA);
    printf("E1: NODUPDATA dup at elem[1] -> rc=%d done=%zu\n", rc, data[1].iov_len);
    assert(rc == MDBX_KEYEXIST);
    assert(data[1].iov_len == 1);
    /* abort: the inserted prefix must NOT persist */
    chk("abort", mdbx_txn_abort(txn));
    chk("txn_begin_ro", mdbx_txn_begin(env, NULL, MDBX_RDONLY, &txn));
    MDBX_dbi dbi_ro;
    chk("dbi_open ro", mdbx_dbi_open(txn, "t1", MDBX_DUPSORT | MDBX_DUPFIXED, &dbi_ro));
    MDBX_val k = {"prefixkey", 9}, v;
    rc = mdbx_get(txn, dbi_ro, &k, &v);
    printf("E1: get after abort -> rc=%d (expect NOTFOUND, prefix rolled back with txn)\n", rc);
    assert(rc == MDBX_NOTFOUND);
    chk("abort ro", mdbx_txn_abort(txn));
    chk("env_close", mdbx_env_close_ex(env, true));
  }

  (void)dupfix_cmp;
  printf("\nPASS: attack 1 HELD — batch accounting consistent (done counts\n"
         "successes, data[0].iov_base never advances past the buffer, the\n"
         "prefix belongs to the txn and is committed/aborted with it).\n"
         "V-038 hypothesis REFUTED: the NODUPDATA-duplicate error is\n"
         "MDBX_KEYEXIST uniformly (inline hive AND sub-page hive) because\n"
         "dupsort_error is only reachable for insert_key (batch element 0);\n"
         "the txn stays usable and the user may resume from data[0].\n");
  return 0;
}
