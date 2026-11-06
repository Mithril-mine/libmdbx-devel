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

#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/345
// For a MDBX_NOSUBDIR database whose data file is gone but whose lock file is
// still there, mdbx_env_delete() returned MDBX_RESULT_TRUE ("No corresponding
// files or directories were found, so no deletion was performed") in every mode
// and left the lock file, so no API call could remove it. With
// MDBX_ENV_ENSURE_UNUSED the same answer was given even while another process
// still had the environment open.

namespace {

const char *const kDb = "issue345.mdbx";
const std::string kLck = std::string(kDb) + MDBX_LOCK_SUFFIX;

bool exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f)
    fclose(f);
  return f != nullptr;
}

bool lck_exists() { return exists(kLck.c_str()); }

void make_db() {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_NE(env, nullptr);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}

void remove_both() {
  std::remove(kDb);
  std::remove(kLck.c_str());
}

/* An unrelated file named "issue345.lck" (the alternative lock name: the
   extension of the pathname replaced by MDBX_LOCK_SUFFIX) must not be touched
   nor change the answer. */
std::string alt_lck_path() { return std::string("issue345.lck"); }

} // namespace

TEST(issue_gh0345, just_delete_both_files) {
  remove_both();
  make_db();
  EXPECT_EQ(mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE), MDBX_SUCCESS);
  EXPECT_FALSE(lck_exists());
  remove_both();
}

TEST(issue_gh0345, just_delete_nothing) {
  remove_both();
  EXPECT_EQ(mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE), MDBX_RESULT_TRUE);
  EXPECT_FALSE(lck_exists());
}

/* The defect itself: the data file is gone, its lock file is not. */
TEST(issue_gh0345, just_delete_orphan_lock) {
  remove_both();
  make_db();
  ASSERT_EQ(std::remove(kDb), 0);
  ASSERT_TRUE(lck_exists());
  EXPECT_EQ(mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE), MDBX_SUCCESS);
  EXPECT_FALSE(lck_exists());
  remove_both();
}

/* An orphan data-file lock next to an alternative lock file: the delete must
   report MDBX_DUPLICATED_LCK and keep both the orphan `-lck` and the
   alternative lock file, not purge the orphan despite the alternative. */
TEST(issue_gh0345, orphan_lock_with_alternative_lck) {
  remove_both();
  const std::string alt = alt_lck_path();
  std::remove(alt.c_str());
  make_db();
  ASSERT_EQ(std::remove(kDb), 0);
  FILE *f = fopen(alt.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fputs("alternative", f);
  fclose(f);
  EXPECT_EQ(mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE), MDBX_DUPLICATED_LCK);
  EXPECT_TRUE(lck_exists()) << "the orphan lock file must be kept when an alternative lock file exists";
  EXPECT_TRUE(exists(alt.c_str()));
  std::remove(alt.c_str());
  remove_both();
}

TEST(issue_gh0345, unrelated_lock_file_untouched) {
  remove_both();
  const std::string alt = alt_lck_path();
  FILE *f = fopen(alt.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fputs("unrelated", f);
  fclose(f);
  EXPECT_EQ(mdbx_env_delete(kDb, MDBX_ENV_JUST_DELETE), MDBX_RESULT_TRUE);
  EXPECT_TRUE(exists(alt.c_str()));
  std::remove(alt.c_str());
  remove_both();
}

/* Missing paths of other shapes are still "nothing found". */
TEST(issue_gh0345, missing_paths) {
#ifdef _WIN32
  static const char *const shapes[] = {"missing\\", "missing\\.", "m1\\m2\\.."};
  const char sep = '\\';
#else
  static const char *const shapes[] = {"missing/", "missing/.", "m1/m2/.."};
  const char sep = '/';
#endif
  for (const char *shape : shapes) {
    const std::string missing = std::string("issue345-sandbox") + sep + shape;
    EXPECT_EQ(mdbx_env_delete(missing.c_str(), MDBX_ENV_JUST_DELETE), MDBX_RESULT_TRUE) << shape;
  }
}

#ifndef _WIN32
/* On Windows MDBX_ENV_ENSURE_UNUSED/MDBX_ENV_WAIT_FOR_UNUSED are broken
   independently (#336), so these cases are POSIX-only. */
TEST(issue_gh0345, ensure_unused_orphan_lock) {
  remove_both();
  make_db();
  ASSERT_EQ(std::remove(kDb), 0);
  ASSERT_TRUE(lck_exists());
  EXPECT_EQ(mdbx_env_delete(kDb, MDBX_ENV_ENSURE_UNUSED), MDBX_SUCCESS);
  EXPECT_FALSE(lck_exists());
  remove_both();
}

/* Waits a single byte within the timeout, for the fork() handshake below. */
static bool wait_byte(int fd, char *out) {
  struct pollfd pfd = {(decltype(pollfd::fd))fd, POLLIN, 0};
  if (poll(&pfd, 1, 10000) != 1)
    return false;
  return read(fd, out, 1) == 1;
}

/* While another process has the environment open, MDBX_ENV_ENSURE_UNUSED must
   refuse with an error and keep the files: both present (control) and with the
   data file unlinked under the live environment (only its lock file left).
   The fork() handshake below uses timeouts: no failure may hang the test. */
TEST(issue_gh0345, ensure_unused_in_use) {
  for (int unlink_data = 0; unlink_data < 2; ++unlink_data) {
    remove_both();
    make_db();
    int ready[2], done[2];
    ASSERT_NE(pipe(ready), -1);
    ASSERT_NE(pipe(done), -1);
    fflush(nullptr);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      /* the child holds the environment open until the parent's command */
      MDBX_env *env = nullptr;
      char c = 0;
      const bool opened = mdbx_env_create(&env) == MDBX_SUCCESS && env != nullptr &&
                          mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS;
      c = opened ? 1 : 0;
      if (write(ready[1], &c, 1) != 1)
        _exit(3);
      if (!wait_byte(done[0], &c))
        _exit(3);
      if (env)
        mdbx_env_close(env);
      _exit(opened ? 0 : 2);
    }
    char c = 0;
    const bool child_ready = wait_byte(ready[0], &c) && c == 1;
    int rc = -1;
    bool lck_left = true;
    if (child_ready) {
      if (unlink_data) {
        EXPECT_EQ(unlink(kDb), 0);
      }
      rc = mdbx_env_delete(kDb, MDBX_ENV_ENSURE_UNUSED);
      lck_left = lck_exists();
      EXPECT_EQ(write(done[1], &c, 1), 1);
    } else {
      kill(pid, SIGKILL);
      EXPECT_EQ(write(done[1], &c, 1), 1);
    }
    int wstatus = 0;
    EXPECT_EQ(waitpid(pid, &wstatus, 0), pid);
    ASSERT_TRUE(child_ready) << "the child did not open the environment in time";
    ASSERT_EQ(WEXITSTATUS(wstatus), 0) << "the child failed to open the environment";
    EXPECT_NE(rc, MDBX_SUCCESS) << (unlink_data ? "lock only, in use" : "data + lock, in use");
    EXPECT_NE(rc, MDBX_RESULT_TRUE);
    EXPECT_TRUE(lck_left) << (unlink_data ? "lock only, in use" : "data + lock, in use");
    remove_both();
  }
}
#endif /* !_WIN32 */
