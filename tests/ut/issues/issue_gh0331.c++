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

#include <sys/stat.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/331 (file mode part)
// copy2pathname() created the destination with mode 0660 (before the umask)
// whatever the mode of the source file was, so a copy of a private (0600)
// database was readable by its group. Now the owner always gets read/write (as
// for the lock file) and the group gets no more than on the source file.
//
// Under umask 022 a 0600 source must give 0600 copies, a 0644 source 0640
// copies (as before) and a source made read-only (0440) after the open 0640
// copies (still writable by the owner), with and without compaction. POSIX only.

namespace {

const char *const kSrc = "issue331-src.mdbx";
const char *const kCopy = "issue331-copy.mdbx";
const char *const kCopyCompact = "issue331-copy-compact.mdbx";

/* Deletes the databases and restores the umask before the test and on every exit path. */
struct files_guard {
  const mode_t old_umask = umask(022);
  files_guard() { remove_all(); }
  ~files_guard() {
    remove_all();
    umask(old_umask);
  }
  static void remove_all() {
    mdbx_env_delete(kSrc, MDBX_ENV_JUST_DELETE);
    mdbx_env_delete(kCopy, MDBX_ENV_JUST_DELETE);
    mdbx_env_delete(kCopyCompact, MDBX_ENV_JUST_DELETE);
  }
};

struct env_guard {
  MDBX_env *env = nullptr;
  ~env_guard() {
    if (env)
      mdbx_env_close(env);
  }
};

unsigned mode_of(const char *path) {
  struct stat st;
  return stat(path, &st) ? 0u : static_cast<unsigned>(st.st_mode & 07777);
}

/* Creates the source with `source_mode` (then changes it to `chmod_to`, if given), copies it both ways and checks
 * the modes of the copies. */
void check_copy_modes(unsigned source_mode, unsigned expected, unsigned chmod_to = 0) {
  files_guard files;
  env_guard src;
  ASSERT_EQ(mdbx_env_create(&src.env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(src.env, kSrc, MDBX_NOSUBDIR, static_cast<mdbx_mode_t>(source_mode)), MDBX_SUCCESS);
  ASSERT_EQ(mode_of(kSrc), source_mode & ~022u);
  if (chmod_to) {
    ASSERT_EQ(chmod(kSrc, chmod_to), 0);
    source_mode = chmod_to;
  }
  ASSERT_EQ(mdbx_env_copy(src.env, kCopy, MDBX_CP_DEFAULTS), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_copy(src.env, kCopyCompact, MDBX_CP_COMPACT), MDBX_SUCCESS);
  EXPECT_EQ(mode_of(kCopy), expected) << "as-is copy of a 0" << std::oct << source_mode << " source";
  EXPECT_EQ(mode_of(kCopyCompact), expected) << "compacting copy of a 0" << std::oct << source_mode << " source";
}

} // namespace

TEST(issue_gh0331, copy_group_access_follows_source) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
  }
  check_copy_modes(0600, 0600);
  if (HasFatalFailure())
    return;
  check_copy_modes(0644, 0640);
  if (HasFatalFailure())
    return;
  check_copy_modes(0640, 0640, 0440);
}
