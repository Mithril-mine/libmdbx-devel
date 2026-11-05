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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/320:
 * when the headers define FALLOC_FL_ALLOCATE_RANGE, osal_fsetsize() took the
 * raw fallocate() result (-1 on any failure, which equals MDBX_RESULT_TRUE) as
 * the error code, so every failure, ENOSPC included, was ignored as "not
 * supported" and the database file grew as a sparse file while the call
 * reported success.
 *
 * The fsetsize_fallocate_errno FAULT site makes that fallocate() call fail
 * with the injected errno, as a full or broken filesystem would. The test
 * opens a new database without a fault, then grows its data file while
 * fallocate() fails: the error must be reported, while "not supported" must
 * still be ignored.
 *
 * Requires the build from tests/special-builds/fallocate-range/CMakeLists.txt
 * (probe-bus on, FALLOC_FL_ALLOCATE_RANGE defined) and MDBX_PROBES=1 at run
 * time. */
#include "mdbx.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>

extern int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

static const char db_path[] = "gh0320.mdbx";

/* Opens a new database, then grows its data file while fallocate() fails with
 * `injected_errno`. Returns the mdbx_env_set_geometry() result of the growth,
 * or -1 when the test cannot run. */
static int grow_with_failed_fallocate(int injected_errno, size_t *site_hits) {
  char reply[512], cmd[128];
  *site_hits = 0;
  mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);
  MDBX_env *env = NULL;
  int rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS) /* 1 MiB now, up to 256 MiB */
    rc = mdbx_env_set_geometry(env, -1, 1 << 20, 256 << 20, 1 << 20, -1, 4096);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, db_path, MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "cannot open the database: %d (%s)\n", rc, mdbx_strerror(rc));
    rc = -1;
    goto bailout;
  }

  rc = -1;
  if (mdbx_probe_ctl("reset fsetsize_fallocate_errno", reply, sizeof reply) != 0) {
    fprintf(stderr, "mdbx_probe_ctl(reset) failed: %s", reply);
    goto bailout;
  }
  snprintf(cmd, sizeof cmd, "fault fsetsize_fallocate_errno %d", injected_errno);
  if (mdbx_probe_ctl(cmd, reply, sizeof reply) != 0) {
    fprintf(stderr, "mdbx_probe_ctl(%s) failed: %s", cmd, reply);
    goto bailout;
  }
  /* grow the data file to 8 MiB: osal_fsetsize() preallocates it */
  rc = mdbx_env_set_geometry(env, -1, 8 << 20, -1, -1, -1, -1);
  mdbx_probe_ctl("fault fsetsize_fallocate_errno none", reply, sizeof reply);

  if (mdbx_probe_ctl("query fsetsize_fallocate_errno", reply, sizeof reply) == 0) {
    /* reply: "ok\n" then "site <name> <kind> <armed> <seen> <hits> ..." */
    const char *site = strstr(reply, "site ");
    char name[64];
    unsigned kind, armed;
    size_t seen;
    if (!site || sscanf(site, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, site_hits) != 5)
      *site_hits = 0;
  }

bailout:
  if (env)
    mdbx_env_close(env);
  mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);
  return rc;
}

static int check(const char *what, int injected_errno, int expected_rc) {
  size_t hits = 0;
  const int rc = grow_with_failed_fallocate(injected_errno, &hits);
  if (rc == -1)
    return 2;
  if (hits == 0) {
    fprintf(stderr, "%s: the fallocate() FAULT site was not reached, is FALLOC_FL_ALLOCATE_RANGE defined?\n", what);
    return 2;
  }
  if (rc != expected_rc) {
    fprintf(stderr, "BUG PRESENT (gh0320): %s: the growth -> %d (%s), expected %d (%s)\n", what, rc, mdbx_strerror(rc),
            expected_rc, mdbx_strerror(expected_rc));
    return 1;
  }
  printf("%s: the growth -> %d (%s), as expected\n", what, rc, mdbx_strerror(rc));
  return 0;
}

int main(void) {
  int failed = check("fallocate() fails with EIO", EIO, EIO);
  if (!failed)
    failed = check("fallocate() fails with EOPNOTSUPP", EOPNOTSUPP, MDBX_SUCCESS);

  /* ENOSPC is ignored on purpose on in-memory filesystems (a workaround for
   * tests in containers), so check it only on a regular one. */
  struct statfs fs;
  if (!failed && statfs(".", &fs) == 0 && (unsigned)fs.f_type != 0x01021994 /* TMPFS_MAGIC */ &&
      (unsigned)fs.f_type != 0x858458f6 /* RAMFS_MAGIC */)
    failed = check("fallocate() fails with ENOSPC", ENOSPC, ENOSPC);
  else if (!failed)
    printf("fallocate() fails with ENOSPC: skipped on an in-memory filesystem\n");
  return failed;
}
