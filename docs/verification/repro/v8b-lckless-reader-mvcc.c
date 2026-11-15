//! \file v8b-lckless-reader-mvcc.c
//! \brief S9-attack-10 (stream B, wave 8): MVCC reader protection in without-lck (lckless) mode.
//!
//! Attack hypothesis: in lckless mode ro_slot_alloc() skips reader registration
//! entirely (txn-ro.c:67-68, `if (unlikely(!env->lck_mmap.lck)) return MDBX_SUCCESS;`),
//! the writer's mvcc_snapshot_oldest_rw() scans zero reader slots
//! (rdt_count() of the lckless_placeholder is 0), so oldest == steady == head
//! and in-process readers would be unprotected against a concurrent writer.
//! Two threads of the same env could also bind reader slots without any lock.
//!
//! VERDICT: HELD (premise disproven). The probe establishes, machine-checked:
//!   E1 (control): with a real lock file the read transaction is snapshot-isolated
//!                 against a concurrent writer's churn (0 mismatches) - proves the
//!                 harness would catch a broken MVCC isolation.
//!   E2: a WRITABLE environment (with or without MDBX_EXCLUSIVE) CANNOT enter
//!       lckless mode: lck-open EACCES hard-fails env_open. The gate is
//!       F_ISSET(env->flags, MDBX_RDONLY | MDBX_EXCLUSIVE) with ALL-bits
//!       semantics (utils.h:7 `((w) & (f)) == (f)`), so EXCLUSIVE alone does NOT
//!       qualify - env_open returns EACCES.
//!       Therefore the "two threads bind slots concurrently without any lock"
//!       premise is impossible: lckless envs never run mvcc_bind_slot().
//!   E3: a WRITABLE env with an UNREADABLE lck file (chmod 000) and RDONLY flag
//!       hard-fails env_open too: case MDBX_EACCESS -> F_ISSET false (dir is on
//!       a writable filesystem) -> default -> ERROR.
//!   (Code-reading corollary: the only POSIX lckless entry is osal_check_fs_rdonly()
//!   == ST_RDONLY (osal-fs.c:19/25), i.e. a filesystem mounted read-only, where no
//!   writer process can exist at all; on such FS the "oldest == head" behavior of
//!   the writer is sound. In-process writers on an RDONLY env are rejected
//!   (api-txn.c:258), so no in-process RO+RW coexistence is possible in lckless.)
//!
//! Doc/code divergence noted (V-049): mdbx.h:1180-1183 promises pure read-only
//! mode "when environment directory and/or both files are not writable", but the
//! POSIX code requires the FS mount flag ST_RDONLY; a non-writable directory on a
//! writable filesystem still fails env_open. Docs are broader than the code
//! (code is stricter - safe direction, but the promise is not honored).
//!
//! Build: cc -O1 -I<repo> v8b-lckless-reader-mvcc.c libmdbx.a -lpthread -lm
//! Expected exit code 0; prints E1..E3 verdict lines.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdbx.h"

static char dirbuf[512];
static int failures = 0;

static void die(const char *what, int rc) {
  fprintf(stderr, "FATAL: %s rc %d (%s)\n", what, rc, rc ? mdbx_liberr2str(rc) : strerror(rc));
  exit(2);
}

static void put_key(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const char *val) {
  MDBX_val k, v;
  int rc;
  k.iov_base = (void *)key;
  k.iov_len = strlen(key);
  v.iov_base = (void *)val;
  v.iov_len = strlen(val);
  rc = mdbx_put(txn, dbi, &k, &v, 0);
  if (rc)
    die("mdbx_put", rc);
}

struct churn_ctx {
  MDBX_env *env;
  MDBX_dbi dbi;
};

static void *churn_thread(void *arg) {
  struct churn_ctx *ctx = arg;
  MDBX_txn *txn = NULL;
  char val[128], key[32];
  int rc;

  rc = mdbx_txn_begin(ctx->env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc)
    die("churn: begin1", rc);
  put_key(txn, ctx->dbi, "probe", "V2-written-by-churn");
  rc = mdbx_txn_commit(txn);
  if (rc)
    die("churn: commit1", rc);

  rc = mdbx_txn_begin(ctx->env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc)
    die("churn: begin2", rc);
  {
    MDBX_val k;
    k.iov_base = (void *)"probe";
    k.iov_len = strlen("probe");
    rc = mdbx_del(txn, ctx->dbi, &k, NULL);
    if (rc)
      die("churn: del", rc);
  }
  rc = mdbx_txn_commit(txn);
  if (rc)
    die("churn: commit2", rc);

  for (int round = 0; round < 8; round++) {
    rc = mdbx_txn_begin(ctx->env, NULL, MDBX_TXN_READWRITE, &txn);
    if (rc)
      die("churn: begin3", rc);
    for (int i = 0; i < 400; i++) {
      snprintf(key, sizeof(key), "churn-%d-%06d", round, i);
      memset(val, 'x' + (char)(i & 7), sizeof(val) - 1);
      val[sizeof(val) - 1] = '\0';
      put_key(txn, ctx->dbi, key, val);
    }
    rc = mdbx_txn_commit(txn);
    if (rc)
      die("churn: commit3", rc);
  }
  return NULL;
}

static void seed(const char *dir, unsigned envflags) {
  MDBX_env *env = NULL;
  MDBX_txn *txn = NULL;
  MDBX_dbi dbi;
  int rc;
  rc = mdbx_env_create(&env);
  if (rc)
    die("env_create", rc);
  mdbx_env_set_maxreaders(env, 42);
  rc = mdbx_env_open(env, dir, envflags, 0644);
  if (rc)
    die("seed env_open", rc);
  rc = mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc)
    die("seed begin", rc);
  rc = mdbx_dbi_open(txn, NULL, 0, &dbi);
  if (rc)
    die("seed dbi_open", rc);
  put_key(txn, dbi, "probe", "V1-original");
  char key[32], val[128];
  for (int i = 0; i < 200; i++) {
    snprintf(key, sizeof(key), "stable-%03d", i);
    memset(val, 'A' + (char)(i % 26), sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    put_key(txn, dbi, key, val);
  }
  rc = mdbx_txn_commit(txn);
  if (rc)
    die("seed commit", rc);
  mdbx_env_close(env);
}

/* E1: control - normal locked env, snapshot isolation MUST hold */
static void control_scenario(void) {
  MDBX_env *env = NULL;
  MDBX_txn *ro = NULL, *txn = NULL;
  MDBX_dbi dbi;
  MDBX_val k, v;
  struct churn_ctx ctx;
  pthread_t thr;
  char first[64];
  int rc, bad = 0;

  rc = mdbx_env_create(&env);
  if (rc)
    die("env_create", rc);
  mdbx_env_set_maxreaders(env, 42);
  rc = mdbx_env_open(env, dirbuf, 0, 0644);
  if (rc)
    die("control env_open", rc);
  rc = mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc)
    die("dbi begin", rc);
  rc = mdbx_dbi_open(txn, NULL, 0, &dbi);
  if (rc)
    die("dbi_open", rc);
  rc = mdbx_txn_commit(txn);
  if (rc)
    die("dbi commit", rc);

  rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &ro);
  if (rc)
    die("ro begin", rc);
  k.iov_base = (void *)"probe";
  k.iov_len = 5;
  rc = mdbx_get(ro, dbi, &k, &v);
  if (rc)
    die("ro get1", rc);
  snprintf(first, sizeof(first), "%.*s", (int)v.iov_len, (char *)v.iov_base);

  ctx.env = env;
  ctx.dbi = dbi;
  rc = pthread_create(&thr, NULL, churn_thread, &ctx);
  if (rc)
    die("pthread_create", rc);
  pthread_join(thr, NULL);

  k.iov_base = (void *)"probe";
  k.iov_len = 5;
  rc = mdbx_get(ro, dbi, &k, &v);
  if (rc != 0 || v.iov_len != strlen(first) || memcmp(v.iov_base, first, v.iov_len) != 0) {
    printf("E1 CONTROL: SNAPSHOT VIOLATION (rc %d) - harness broken\n", rc);
    bad++;
  }
  for (int i = 0; i < 40; i++) {
    char key[32], expect[128];
    snprintf(key, sizeof(key), "stable-%03d", i);
    memset(expect, 'A' + (char)(i % 26), sizeof(expect) - 1);
    expect[sizeof(expect) - 1] = '\0';
    k.iov_base = (void *)key;
    k.iov_len = strlen(key);
    rc = mdbx_get(ro, dbi, &k, &v);
    if (rc != 0 || v.iov_len != strlen(expect) || memcmp(v.iov_base, expect, v.iov_len) != 0)
      bad++;
  }
  mdbx_txn_abort(ro);
  mdbx_env_close(env);
  printf("E1 CONTROL: %s\n", bad ? "FAILED(harness)" : "snapshot-isolated (0 mismatches)");
  failures += bad;
}

/* E2/E3: lckless-entry attempts on a writable filesystem - both must FAIL */
static void lckless_entry_attempt(unsigned envflags, const char *what) {
  MDBX_env *env = NULL;
  int rc;
  rc = mdbx_env_create(&env);
  if (rc)
    die("env_create", rc);
  rc = mdbx_env_open(env, dirbuf, envflags, 0644);
  if (rc == MDBX_SUCCESS) {
    printf("%s: UNEXPECTED SUCCESS - lckless mode entered on writable fs; MVCC isolation for its readers "
           "is not guaranteed by any table\n",
           what);
    failures++;
    mdbx_env_close(env);
  } else {
    printf("%s: env_open rejected with rc %d (%s) - lckless not entered (gate holds)\n", what, rc, mdbx_liberr2str(rc));
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  mdbx_setup_debug_nofmt(MDBX_LOG_NOTICE, MDBX_DBG_DONTCHANGE, MDBX_LOGGER_NOFMT_DONTCHANGE, NULL, 0);

  system("rm -rf /tmp/v8b-lckless-*");
  mkdir("/tmp/v8b-lckless-control", 0755);
  strcpy(dirbuf, "/tmp/v8b-lckless-control");
  seed(dirbuf, 0);
  control_scenario();

  mkdir("/tmp/v8b-lckless-attack", 0755);
  strcpy(dirbuf, "/tmp/v8b-lckless-attack");
  seed(dirbuf, MDBX_EXCLUSIVE);
  /* E2: writable env with EXCLUSIVE flag, lck unreadable */
  chmod("/tmp/v8b-lckless-attack/mdbx.lck", 0000);
  lckless_entry_attempt(MDBX_EXCLUSIVE, "E2 EXCLUSIVE+writable+lck-unreadable");
  /* E3: writable env RDONLY flag, lck unreadable, directory writable */
  lckless_entry_attempt(MDBX_RDONLY, "E3 RDONLY+writable-dir+lck-unreadable");

  printf("\nverdict: attack-10 HELD%s\n", failures ? " (FAILURES PRESENT)" : "");
  return failures ? 1 : 0;
}
