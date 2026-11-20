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

/* Issue #535: with no logger installed an application gets routine NOTICE
 * lines on stderr at every MDBX_WRITEMAP open, for instance:
 *
 *   dxb_setup:20885 open-MADV_REMOVE (deallocate file space) 16..8192
 *   dxb_setup:20896 open-MADV_DONTNEED 16..8192
 *   dxb_set_readahead:20363 readahead ON 0..4
 *
 * A library must stay silent on stderr unless a warning or an error occurs.
 */

#include "../env-guard.h++"
#include "../test-env.h++"
#include <gtest/gtest.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
static int sgm_fileno(FILE *stream) { return _fileno(stream); }
static int sgm_dup(int fd) { return _dup(fd); }
/* MSVC's _dup2() returns 0 on success, unlike POSIX dup2() which returns the
 * new descriptor; normalize to the POSIX convention for the assertions. */
static int sgm_dup2(int old_fd, int new_fd) {
  const int rc = _dup2(old_fd, new_fd);
  return (rc == 0) ? new_fd : rc;
}
static void sgm_close(int fd) { _close(fd); }
#else
#include <unistd.h>
static int sgm_fileno(FILE *stream) { return fileno(stream); }
static int sgm_dup(int fd) { return dup(fd); }
static int sgm_dup2(int old_fd, int new_fd) { return dup2(old_fd, new_fd); }
static void sgm_close(int fd) { close(fd); }
#endif

static int open_close_writemap(const char *path) {
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    return rc;
  rc = mdbx_env_open(env, path, MDBX_NOSUBDIR | MDBX_WRITEMAP, 0664);
  const int close_rc = mdbx_env_close(env);
  return (rc == MDBX_SUCCESS) ? close_rc : rc;
}

/* RAII for the stderr-capture file: closes it and removes the file on every
 * exit path of the test, including the ASSERT_ failures. */
struct capture_file {
  FILE *file = nullptr;
  const char *const path;
  explicit capture_file(const char *p) : path(p) { remove(p); }
  capture_file(const capture_file &) = delete;
  capture_file &operator=(const capture_file &) = delete;
  ~capture_file() {
    if (file)
      fclose(file);
    remove(path);
  }
};

TEST(issue_gh0535, open_without_logger_is_quiet) {
  const env_files_guard guard({"issue_gh0535"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *path = "issue_gh0535";

  /* Create the database before capturing: the reproduction opens an existing
   * one, and the creation-time messages (if any) are out of the issue's scope. */
  int rc = open_close_writemap(path);
  ASSERT_EQ(MDBX_SUCCESS, rc) << mdbx_strerror(rc);

  /* Capture stderr at the descriptor level: with no logger set the fallback
   * writes directly to the stderr stream (via fprintf/fflush). */
  fflush(stderr);
  const int saved_fd = sgm_dup(sgm_fileno(stderr));
  ASSERT_GE(saved_fd, 0);
  capture_file capture("issue_gh0535-stderr.tmp");
  capture.file = fopen(capture.path, "w+");
  ASSERT_NE(capture.file, nullptr);
  ASSERT_EQ(sgm_fileno(stderr), sgm_dup2(sgm_fileno(capture.file), sgm_fileno(stderr)));

  rc = open_close_writemap(path);

  fflush(stderr);
  sgm_dup2(saved_fd, sgm_fileno(stderr));
  sgm_close(saved_fd);
  ASSERT_EQ(MDBX_SUCCESS, rc) << mdbx_strerror(rc);

  rewind(capture.file);
  char buffer[1024];
  memset(buffer, 0, sizeof(buffer));
  const size_t printed = fread(buffer, 1, sizeof(buffer) - 1, capture.file);

#if !defined(MDBX_DEBUG_FOR_TEST) || MDBX_DEBUG_FOR_TEST < 1
  EXPECT_EQ(0u, printed) << "libmdbx printed " << printed << " bytes to stderr on a routine open:\n" << buffer;
#else
  /* Debug builds (MDBX_DEBUG > 0) dump meta-page diagnostics (the troika
   * dump) at the NOTICE level on every open, a pre-existing behavior beyond
   * this issue's scope. Assert the issue-scoped messages only: the routine
   * open-time madvise/readahead and the creation "expected empty file" lines
   * are at the VERBOSE level now, which never reaches stderr at the default
   * log level. */
  EXPECT_EQ(nullptr, strstr(buffer, "open-MADV_")) << printed << " bytes:\n" << buffer;
  EXPECT_EQ(nullptr, strstr(buffer, "readahead ")) << printed << " bytes:\n" << buffer;
  EXPECT_EQ(nullptr, strstr(buffer, "empty file")) << printed << " bytes:\n" << buffer;
#endif
}
