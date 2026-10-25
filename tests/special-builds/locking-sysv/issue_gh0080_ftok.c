/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/80
 * The System V IPC locking key used by libmdbx is produced by lck_ftok(),
 * which is a passthrough to the system ftok(path, 42). This test opens a
 * database and verifies that the semaphore set created for it exists under
 * exactly ftok(path, 42), proving the wrapper keeps the system-ftok behavior. */
#include "mdbx.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <unistd.h>

int main(void) {
  const char *db = "gh0080-ftok.mdbx";
  char lck[128];
  snprintf(lck, sizeof lck, "%s-lck", db);
  unlink(db);
  unlink(lck);

  MDBX_env *env = NULL;
  if (mdbx_env_create(&env)) {
    fprintf(stderr, "mdbx_env_create() failed\n");
    return 2;
  }
  if (mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, 4096)) {
    fprintf(stderr, "mdbx_env_set_geometry() failed\n");
    mdbx_env_close(env);
    return 2;
  }
  if (mdbx_env_open(env, db, MDBX_NOSUBDIR, 0644)) {
    fprintf(stderr, "mdbx_env_open() failed\n");
    mdbx_env_close(env);
    return 2;
  }

  const key_t expect = ftok(db, 42);
  if (expect == (key_t)-1) {
    fprintf(stderr, "ftok() failed: %s\n", strerror(errno));
    mdbx_env_close(env);
    return 2;
  }

  const int semid = semget(expect, 2, 0);
  const int semerr = errno;
  const int rc = mdbx_env_close(env);

  unlink(db);
  unlink(lck);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_env_close() failed: %d\n", rc);
    return 2;
  }
  if (semid == -1) {
    fprintf(stderr, "no semaphore set under ftok(path, 42) key %d: %s\n", (int)expect, strerror(semerr));
    return 1;
  }
  printf("OK: SysV semaphore set exists under the system ftok() key\n");
  return 0;
}