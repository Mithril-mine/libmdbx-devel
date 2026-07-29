/* rthc_lckless.c — rthc_thread_dtor() dereferences a null lck_mmap.lck for any
 * lck-less environment registered in the same process. */
#include "mdbx.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHK(expr)                                                                                                      \
  do {                                                                                                                 \
    int err_ = (expr);                                                                                                 \
    if (err_ != MDBX_SUCCESS) {                                                                                        \
      fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #expr, mdbx_strerror(err_), err_);                 \
      exit(2);                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void *reader_thread(void *arg) {
  (void)arg;
  MDBX_txn *txn = NULL;
  /* binds a reader slot -> thread_rthc_set() -> dtor will run at thread exit */
  CHK(mdbx_txn_begin(arg, NULL, MDBX_TXN_RDONLY, &txn));
  mdbx_txn_abort(txn);
  return NULL;
}

int main(int argc, char **argv) {
  const char *dir = (argc > 1) ? argv[1] : "issue_gh16";
  char rodb[512], normdb[512], buf[600];
  snprintf(rodb, sizeof(rodb), "%s/ro.db", dir);
  snprintf(normdb, sizeof(normdb), "%s.norm.db", dir);

  /* fresh state */
  chmod(dir, 0755);
  snprintf(buf, sizeof(buf), "%s/ro.db", dir);
  unlink(buf);
  snprintf(buf, sizeof(buf), "%s/ro.db-lck", dir);
  unlink(buf);
  rmdir(dir);
  unlink(normdb);
  snprintf(buf, sizeof(buf), "%s-lck", normdb);
  unlink(buf);
  if (mkdir(dir, 0755) && access(dir, F_OK)) {
    perror("mkdir");
    return 2;
  }

  /* 1. create the soon-to-be read-only DB normally, then close it */
  {
    MDBX_env *env = NULL;
    CHK(mdbx_env_create(&env));
    CHK(mdbx_env_open(env, rodb, MDBX_NOSUBDIR, 0664));
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(env, NULL, 0, &txn));
    CHK(mdbx_txn_commit(txn));
    mdbx_env_close(env);
  }
  /* Make the lck FILE itself read-only, keeping it present and the directory
   * writable, so osal_openfile() returns EACCES verbatim and lck_setup() falls
   * back to lck-less mode.  Deleting the lck file instead does NOT work. */
  snprintf(buf, sizeof(buf), "%s/ro.db-lck", dir);
  if (chmod(buf, 0444)) {
    perror("chmod lck");
    return 2;
  }

  /* 2. a normal env, whose reader thread will trigger the dtor */
  MDBX_env *normal_env = NULL;
  CHK(mdbx_env_create(&normal_env));
  CHK(mdbx_env_open(normal_env, normdb, MDBX_NOSUBDIR, 0664));

  /* 3. the lck-less env, registered in the same process */
  MDBX_env *lckless = NULL;
  CHK(mdbx_env_create(&lckless));
  int rc = mdbx_env_open(lckless, rodb, MDBX_NOSUBDIR | MDBX_RDONLY | MDBX_EXCLUSIVE, 0664);
  printf("lck-less env open -> %s (%d)\n", mdbx_strerror(rc), rc);
  if (rc != MDBX_SUCCESS) {
    printf("RESULT: inconclusive - could not force lck-less mode\n");
    mdbx_env_close(lckless);
    mdbx_env_close(normal_env);
    chmod(buf, 0644);
    return 3;
  }

  /* 4. thread takes a read txn on the normal env and exits ->
   *    rthc_thread_dtor() walks BOTH envs, including the lck-less one */
  pthread_t th;
  if (pthread_create(&th, NULL, reader_thread, normal_env))
    return 2;
  pthread_join(th, NULL);
  if (pthread_create(&th, NULL, reader_thread, lckless))
    return 2;
  pthread_join(th, NULL);
  printf("reader thread joined (rthc_thread_dtor has run)\n");

  mdbx_env_close(lckless);
  mdbx_env_close(normal_env);
  chmod(buf, 0644);
  printf("RESULT: done\n");
  return 0;
}
