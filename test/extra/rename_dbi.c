/*
Repro for mdbx_dbi_rename() on a DBI created in the current write transaction,
followed by mdbx_dbi_open() of the old name.

Compile from /home/cosmin:

        cc -std=c11 -Wall -Wextra -DMDBX_DEBUG=1 -Isdk/c/mdbx/src \
                sdk/tests/mdbx_rename_repro.c sdk/c/mdbx/src/mdbx.c \
                -pthread -ldl -o /tmp/mdbx_rename_repro

Run:

        /tmp/mdbx_rename_repro top
        /tmp/mdbx_rename_repro nested

Observed with MDBX_DEBUG=1:

        * top: mdbx_dbi_rename() succeeds, mdbx_dbi_open(old name) returns
          MDBX_NOTFOUND, then mdbx_txn_commit() asserts.
        * nested: child commit succeeds, then parent commit asserts.
*/

#include "mdbx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int rc, const char *op) {
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "%s: %d: %s\n", op, rc, mdbx_strerror(rc));
    exit(2);
  }
}

static void cleanup(const char *path) {
  char lck[512];
  snprintf(lck, sizeof(lck), "%s-lck", path);
  unlink(path);
  unlink(lck);
}

static MDBX_env *open_env(const char *path) {
  MDBX_env *env = NULL;
  check(mdbx_env_create(&env), "mdbx_env_create");
  check(mdbx_env_set_maxdbs(env, 16), "mdbx_env_set_maxdbs");
  check(mdbx_env_open(env, path, MDBX_NOSUBDIR, 0660), "mdbx_env_open");
  return env;
}

static MDBX_dbi create_put_rename(MDBX_txn *txn) {
  MDBX_dbi dbi = 0;
  MDBX_val key = {(void *)"k", 1};
  MDBX_val val = {(void *)"v", 1};
  int rc = mdbx_dbi_open(txn, "t", 0, &dbi);
  if (rc != MDBX_NOTFOUND)
    check(rc, "mdbx_dbi_open t without MDBX_CREATE");
  check(mdbx_dbi_open(txn, "t", MDBX_CREATE, &dbi), "mdbx_dbi_open t with MDBX_CREATE");
  check(mdbx_put(txn, dbi, &key, &val, 0), "mdbx_put t/k");
  check(mdbx_dbi_rename(txn, dbi, "u"), "mdbx_dbi_rename t -> u");
  return dbi;
}

static void reopen_old_name(MDBX_txn *txn) {
  MDBX_dbi dbi = 0;
  int rc = mdbx_dbi_open(txn, "t", 0, &dbi);
  if (rc == MDBX_NOTFOUND)
    puts("reopen old name: MDBX_NOTFOUND");
  else
    check(rc, "mdbx_dbi_open old name");
}

static void run_top(MDBX_env *env) {
  MDBX_txn *txn = NULL;
  check(mdbx_txn_begin(env, NULL, 0, &txn), "mdbx_txn_begin top");
  (void)create_put_rename(txn);
  reopen_old_name(txn);
  puts("top: rename succeeded; committing");
  fflush(stdout);
  check(mdbx_txn_commit(txn), "mdbx_txn_commit top");
  puts("top: commit succeeded");
}

static void run_nested(MDBX_env *env) {
  MDBX_txn *parent = NULL;
  MDBX_txn *child = NULL;
  check(mdbx_txn_begin(env, NULL, 0, &parent), "mdbx_txn_begin parent");
  check(mdbx_txn_begin(env, parent, 0, &child), "mdbx_txn_begin child");
  (void)create_put_rename(child);
  reopen_old_name(child);
  puts("nested: rename succeeded; committing child");
  fflush(stdout);
  check(mdbx_txn_commit(child), "mdbx_txn_commit child");
  puts("nested: child commit succeeded; committing parent");
  fflush(stdout);
  check(mdbx_txn_commit(parent), "mdbx_txn_commit parent");
  puts("nested: parent commit succeeded");
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "rename_repro";
  cleanup(path);

  MDBX_env *env = open_env(path);
  run_top(env);
  check(mdbx_env_close(env), "mdbx_env_close");
  cleanup(path);

  env = open_env(path);
  run_nested(env);
  check(mdbx_env_close(env), "mdbx_env_close");
  cleanup(path);

  return 0;
}
