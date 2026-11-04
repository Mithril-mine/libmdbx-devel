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
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/310
// When the exclusive create of the copy destination failed with anything but
// EEXIST, copy2pathname() took the path for absent or created by itself and
// removed it. But open(O_CREAT | O_EXCL) fails with EMFILE, ENFILE or ENOMEM
// before it looks at the path, so mdbx_env_copy() in a process at its fd limit
// deleted an existing destination, with flags 0 as well as with
// MDBX_CP_OVERWRITE. POSIX-only (setrlimit, dup).

namespace {

const char *const kSource = "issue310-src.mdbx";
const char *const kDest = "issue310-dest.mdbx";
const std::string kPrecious = "PRECIOUS";

void remove_db(const char *path) {
  std::remove(path);
  std::remove((std::string(path) + "-lck").c_str());
}

/* Deletes both files before the test and on every exit path of it. */
struct db_remover {
  db_remover() { remove_db(kSource), remove_db(kDest); }
  ~db_remover() { remove_db(kSource), remove_db(kDest); }
};

struct env_closer {
  void operator()(MDBX_env *env) const { mdbx_env_close(env); }
};

/* Fills the descriptor table, as in a server that reached its `ulimit -n`; restores everything on destruction. */
struct fd_table_filler {
  struct rlimit saved;
  bool limited = false;
  std::vector<int> fds;
  bool full = false;
  fd_table_filler() {
    if (getrlimit(RLIMIT_NOFILE, &saved) != 0)
      return;
    struct rlimit low = saved;
    low.rlim_cur = 256 < saved.rlim_cur ? 256 : saved.rlim_cur;
    limited = setrlimit(RLIMIT_NOFILE, &low) == 0;
    const int source = open("/dev/null", O_RDONLY);
    if (source < 0) {
      full = errno == EMFILE /* already full */;
      return;
    }
    fds.push_back(source);
    int fd;
    while ((fd = dup(source)) >= 0)
      fds.push_back(fd);
    full = errno == EMFILE;
  }
  ~fd_table_filler() {
    for (int fd : fds)
      close(fd);
    if (limited)
      setrlimit(RLIMIT_NOFILE, &saved);
  }
};

std::string read_file(const char *path) {
  std::string content;
  FILE *const f = std::fopen(path, "rb");
  if (!f)
    return "<missing>";
  for (int c; (c = std::fgetc(f)) != EOF;)
    content += char(c);
  std::fclose(f);
  return content;
}

} // namespace

TEST(issue_gh0310, copy_at_fd_limit_keeps_existing_destination) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const db_remover cleanup;

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const std::unique_ptr<MDBX_env, env_closer> env_owner(env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kSource, MDBX_NOSUBDIR, 0644));

  for (const MDBX_copy_flags_t flags : {MDBX_CP_DEFAULTS, MDBX_CP_OVERWRITE}) {
    /* An existing file at the destination: an older backup, or another process's database. */
    FILE *const f = std::fopen(kDest, "wb");
    ASSERT_NE(nullptr, f);
    const size_t written = std::fwrite(kPrecious.data(), 1, kPrecious.size(), f);
    ASSERT_EQ(0, std::fclose(f));
    ASSERT_EQ(kPrecious.size(), written);

    int rc;
    {
      const fd_table_filler filler;
      ASSERT_TRUE(filler.full) << "could not fill the descriptor table";
      rc = mdbx_env_copy(env, kDest, flags);
    }
    EXPECT_EQ(EMFILE, rc) << "flags " << flags;
    EXPECT_EQ(kPrecious, read_file(kDest)) << "a failed copy (flags " << flags << ") must keep the destination";
  }
}
