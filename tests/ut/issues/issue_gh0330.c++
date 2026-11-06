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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/330
// When the C headers lack FALLOC_FL_ALLOCATE_RANGE (it is 0 and only recent
// kernel headers define it), osal_fsetsize() did not compile its raw
// fallocate() branch but called posix_fallocate(). On a filesystem without
// fallocate() glibc emulates it by reading and writing the file; a copy
// destination is write-only, so the read failed with EBADF and the copy failed.
//
// A child process mounts a ramfs (no fallocate()) in its own user and mount
// namespaces and copies a database with mdbx_env_copy2fd() to a write-only file
// there. The copy must succeed and be complete. Linux only; skipped when
// unprivileged user namespaces are not available.

namespace {

const char *const kSourceDb = "issue330-src.mdbx";
const char *const kMountDir = "issue330-ramfs";
const int kRecords = 1000;
const int kSkip = 77;

/* Deletes the source database and the mount point before the test and on every exit path. */
struct files_guard {
  files_guard() { cleanup(); }
  ~files_guard() { cleanup(); }
  static void cleanup() {
    mdbx_env_delete(kSourceDb, MDBX_ENV_JUST_DELETE);
    rmdir(kMountDir);
  }
};

int create_source() {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, kSourceDb, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644);
  MDBX_txn *txn = nullptr;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  MDBX_dbi dbi = 0;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
  for (int i = 0; rc == MDBX_SUCCESS && i < kRecords; ++i) {
    MDBX_val key = {&i, sizeof(i)}, data = {&i, sizeof(i)};
    rc = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT);
  }
  if (txn)
    rc = (rc == MDBX_SUCCESS) ? mdbx_txn_commit(txn) : (mdbx_txn_abort(txn), rc);
  if (env)
    mdbx_env_close(env);
  return rc;
}

/* Number of records of the main table of the database at `path` that hold the value written by create_source(),
 * read with a cursor, or -1. */
int count_records(const char *path) {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_RDONLY, 0644);
  int records = 0;
  MDBX_txn *txn = nullptr;
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc == MDBX_SUCCESS) {
    MDBX_dbi dbi = 0;
    MDBX_cursor *cursor = nullptr;
    rc = mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi);
    if (rc == MDBX_SUCCESS)
      rc = mdbx_cursor_open(txn, dbi, &cursor);
    MDBX_val key, data;
    for (MDBX_cursor_op op = MDBX_FIRST; rc == MDBX_SUCCESS; op = MDBX_NEXT) {
      rc = mdbx_cursor_get(cursor, &key, &data, op);
      if (rc == MDBX_SUCCESS && key.iov_len == sizeof(int) && data.iov_len == sizeof(int) &&
          memcmp(key.iov_base, data.iov_base, sizeof(int)) == 0)
        ++records;
    }
    if (rc == MDBX_NOTFOUND)
      rc = MDBX_SUCCESS;
    if (cursor)
      mdbx_cursor_close(cursor);
    mdbx_txn_abort(txn);
  }
  if (env)
    mdbx_env_close(env);
  return (rc == MDBX_SUCCESS) ? records : -1;
}

bool write_file(const char *path, const std::string &text) {
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  const bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
  close(fd);
  return ok;
}

/* The child's work; returns its exit status (kSkip when the ramfs cannot be mounted). */
int copy_to_ramfs() {
  const uid_t uid = getuid();
  const gid_t gid = getgid();
  if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    fprintf(stderr, "unshare(): %s\n", strerror(errno));
    return kSkip;
  }
  /* as `unshare -r`: map the current user to root of the new namespace */
  if (!write_file("/proc/self/setgroups", "deny") ||
      !write_file("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1") ||
      !write_file("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1")) {
    fprintf(stderr, "uid/gid map: %s\n", strerror(errno));
    return kSkip;
  }
  if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0 ||
      mount("none", kMountDir, "ramfs", 0, nullptr) != 0) {
    fprintf(stderr, "mount(): %s\n", strerror(errno));
    return kSkip;
  }

  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, kSourceDb, MDBX_NOSUBDIR | MDBX_RDONLY, 0644);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "open the source: %s\n", mdbx_strerror(rc));
    return 2;
  }
  const std::string copy = std::string(kMountDir) + "/copy.mdbx";
  /* a copy destination is write-only, as libmdbx opens it for mdbx_env_copy() */
  const int fd = open(copy.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    fprintf(stderr, "open the copy: %s\n", strerror(errno));
    mdbx_env_close(env);
    return 3;
  }
  rc = mdbx_env_copy2fd(env, fd, MDBX_CP_DEFAULTS);
  close(fd);
  mdbx_env_close(env);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "mdbx_env_copy2fd() to a write-only file on ramfs: %d (%s)\n", rc, mdbx_strerror(rc));
    return 1;
  }
  const int records = count_records(copy.c_str());
  if (records != kRecords) {
    fprintf(stderr, "the copy has %d records, expected %d\n", records, kRecords);
    return 4;
  }
  return 0;
}

} // namespace

TEST(issue_gh0330, copy_to_a_filesystem_without_fallocate) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  files_guard files;
  ASSERT_EQ(create_source(), MDBX_SUCCESS);
  ASSERT_EQ(mkdir(kMountDir, 0755), 0) << strerror(errno);

  fflush(nullptr);
  const pid_t pid = fork();
  ASSERT_GE(pid, 0) << "fork() failed: " << strerror(errno);
  if (pid == 0)
    _exit(copy_to_ramfs()); /* the ramfs and the copy vanish with the child's mount namespace */
  /* wait up to 60 seconds, then kill the child */
  int status = 0;
  pid_t got = 0;
  for (int i = 0; i < 60000 && got == 0; ++i) {
    got = waitpid(pid, &status, WNOHANG);
    if (got < 0 && errno == EINTR)
      got = 0;
    if (got == 0)
      usleep(1000);
  }
  if (got == 0) {
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  ASSERT_EQ(got, pid) << "the child hung or could not be waited for: " << strerror(errno);
  ASSERT_TRUE(WIFEXITED(status)) << "the child failed, wait status 0x" << std::hex << status;
  if (WEXITSTATUS(status) == kSkip) {
    GTEST_SKIP() << "a ramfs cannot be mounted in an unprivileged user namespace here";
  }
  EXPECT_EQ(WEXITSTATUS(status), 0) << "the copy to a write-only file on a filesystem without fallocate() failed "
                                       "(EBADF from the posix_fallocate() emulation?), see the child's message above";
}
