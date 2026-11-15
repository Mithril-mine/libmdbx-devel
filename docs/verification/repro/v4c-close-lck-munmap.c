/* v4c-close-lck-munmap.c — wave 4c pair-2 probe (env_close vs lck_destroy munmap ordering).
 *
 * Static chain: mdbx_env_close_ex -> env_close -> rthc_remove -> rthc_drown ->
 *   lck_destroy() takes the exclusive flock, and when the lck file is still
 *   linked (st_nlink > 0, the normal case: libmdbx keeps the lck file) calls
 *   osal_munmap(&env->lck_mmap) at lck-posix.c:514.  Posix osal_munmap zeroes
 *   only base/limit/current and leaves map->lck dangling (no assignment
 *   `lck_mmap.lck = nullptr` exists anywhere in src/).  Back in env_close
 *   (env.c:586-587) `if (env->lck_mmap.lck) osal_munmap(&env->lck_mmap)`
 *   therefore calls munmap(nullptr, 0) -> EINVAL -> spurious
 *   "Unexpected munmap() error 22" ERROR-level log on every last-env close.
 *
 * Probe: open + close an env twice (two create/open/close rounds so the lck
 * file is present), with the default logger active and MDBX_DEBUG=1, and
 * strace-count munmap(2) calls.  Expected: no crash (base is nullptr by then);
 * the second munmap is munmap(NULL, 0) = EINVAL.  PASS criteria: process
 * completes; the point is to observe/confirm the benign double-munmap, see
 * docs/verification/doc-additions-v4c.md finding F-C4.
 */
#include "mdbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char *what, int rc) {
  char b[64];
  mdbx_strerror_r(rc, b, sizeof(b));
  fprintf(stderr, "FAIL %s: %d (%s)\n", what, rc, b);
  exit(2);
}

static void round1(const char *path) {
  MDBX_env *env = NULL;
  int rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    die("create", rc);
  rc = mdbx_env_open(env, path, MDBX_NOSUBDIR /* lck file kept linked */, 0640);
  if (rc != MDBX_SUCCESS)
    die("open", rc);
  rc = mdbx_env_close_ex(env, true);
  if (rc != MDBX_SUCCESS)
    die("close_ex", rc);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  const char *base = (argc > 1) ? argv[1] : "/tmp/v4c-close-lck-munmap.db";
  char path[512];
  snprintf(path, sizeof(path), "%s", base);
  unlink(path);
  unlink("/tmp/v4c-close-lck-munmap.db.lck");
  unlink("/tmp/v4c-close-lck-munmap-m2.db");
  unlink("/tmp/v4c-close-lck-munmap-m2.db.lck");

  round1(path);
  printf("round 1 done\n");
  round1("/tmp/v4c-close-lck-munmap-m2.db");
  printf("round 2 done\n");

  printf("PASS (no crash; check strace/log for the EINVAL munmap)\n");
  return 0;
}
