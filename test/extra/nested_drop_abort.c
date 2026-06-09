#include <stdio.h>
#include <stdlib.h>

#include "mdbx.h"

static void check(int rc, const char *op) {
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "%s: %d (%s)\n", op, rc, mdbx_strerror(rc));
    exit(1);
  }
}

static int get(MDBX_txn *txn, MDBX_dbi dbi) {
  MDBX_val key = {.iov_base = (void *)"k", .iov_len = 1};
  MDBX_val val;
  return mdbx_get(txn, dbi, &key, &val);
}

int main() {
  const char* dbfile = "nested_drop_abort";
  MDBX_env *env;
  check(mdbx_env_create(&env), "env_create");
  check(mdbx_env_set_maxdbs(env, 16), "set_maxdbs");
  check(mdbx_env_open(env, dbfile, MDBX_NOSUBDIR, 0660), "env_open");

  MDBX_txn *parent;
  check(mdbx_txn_begin(env, NULL, 0, &parent), "begin parent");

  MDBX_dbi dbi;
  check(mdbx_dbi_open(parent, "t", MDBX_CREATE, &dbi), "create t");
  MDBX_val key = {.iov_base = (void *)"k", .iov_len = 1};
  MDBX_val val = {.iov_base = (void *)"v", .iov_len = 1};
  check(mdbx_put(parent, dbi, &key, &val, 0), "put");
  check(get(parent, dbi), "get before child");

  MDBX_txn *child;
  check(mdbx_txn_begin(env, parent, 0, &child), "begin child");
  check(mdbx_drop(child, dbi, true), "drop in child");
  check(mdbx_txn_abort(child), "abort child");

  int old_rc = get(parent, dbi);
  MDBX_dbi reopened;
  int open_rc = mdbx_dbi_open(parent, "t", 0, &reopened);
  int get_rc = open_rc == MDBX_SUCCESS ? get(parent, reopened) : open_rc;

  printf("libmdbx %u.%u.%u.%u (%s)\n", mdbx_version.major, mdbx_version.minor, mdbx_version.patch, mdbx_version.tweak,
         mdbx_version.git.describe);
  printf("old DBI after child abort: %d (%s)\n", old_rc, mdbx_strerror(old_rc));
  printf("reopen after child abort: %d (%s)\n", open_rc, mdbx_strerror(open_rc));
  printf("get through reopened DBI: %d (%s)\n", get_rc, mdbx_strerror(get_rc));

  check(mdbx_txn_abort(parent), "abort parent");
  check(mdbx_env_close(env), "env_close");
  return 0;
}
