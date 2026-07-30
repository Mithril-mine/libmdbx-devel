/* offsetof(tree_t, root) == 8: u16 flags, u16 height, u32 dupfix_size, then pgno_t root */
#include "../../src/essentials.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int die(const char *what, int rc) {
  fprintf(stderr, "%s: %s (%d)\n", what, mdbx_strerror(rc), rc);
  return 2;
}

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "issue_gh0017";
  mdbx_env_delete(path, MDBX_ENV_JUST_DELETE);
  int rc;
  MDBX_env *env = NULL;
  if ((rc = mdbx_env_create(&env)))
    return die("env_create", rc);
  if ((rc = mdbx_env_set_maxdbs(env, 4)))
    return die("set_maxdbs", rc);
  if ((rc = mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_WRITEMAP | MDBX_EXCLUSIVE, 0664)))
    return die("env_open", rc);

  MDBX_txn *txn = NULL;
  if ((rc = mdbx_txn_begin(env, NULL, 0, &txn)))
    return die("txn_begin(w)", rc);
  MDBX_dbi sub;
  if ((rc = mdbx_dbi_open(txn, "s", MDBX_CREATE, &sub)))
    return die("dbi_open(s)", rc);
  MDBX_val k = {(void *)"k", 1}, v = {(void *)"v", 1};
  if ((rc = mdbx_put(txn, sub, &k, &v, 0)))
    return die("put", rc);
  if ((rc = mdbx_txn_commit(txn)))
    return die("commit", rc);

  if ((rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &txn)))
    return die("txn_begin(r)", rc);
  MDBX_val name = {(void *)"s", 1}, treerec;
  if ((rc = mdbx_get(txn, 1 /*MAIN_DBI*/, &name, &treerec)))
    return die("get(MAIN,'s')", rc);
  uint32_t old_root, bad_root = 0x40000000u; /* far beyond first_unallocated, not P_INVALID */
  memcpy(&old_root, (char *)treerec.iov_base + offsetof(tree_t, root), 4);
  memcpy((char *)treerec.iov_base + offsetof(tree_t, root), &bad_root, 4);
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
  return 0;
}
