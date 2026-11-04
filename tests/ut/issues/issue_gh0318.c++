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

#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/318
// The env::operate_options conversion from C environment flags kept no_sticky_threads only when
// MDBX_EXCLUSIVE was absent, so the flags combination MDBX_NOSTICKYTHREADS | MDBX_EXCLUSIVE silently
// produced sticky threads: a read transaction started on the thread that holds the write transaction
// failed with MDBX_TXN_OVERLAPPING. The reverse conversion env::operate_parameters::make_flags() has
// no such condition, so the two conversions were asymmetric and the requested threading model was
// dropped without any diagnostics.

namespace {

constexpr MDBX_env_flags_t both_flags() { return MDBX_env_flags_t(MDBX_NOSTICKYTHREADS | MDBX_EXCLUSIVE); }

/* Removes both the database and the lock file, on every exit path. */
void remove_db(const char *path) {
  std::remove(path);
  std::remove((std::string(path) + "-lck").c_str());
}

struct db_remover {
  const char *path;
  explicit db_remover(const char *p) : path(p) { remove_db(path); }
  ~db_remover() { remove_db(path); }
};

} // namespace

/* The flags-to-options conversion must take the MDBX_NOSTICKYTHREADS bit as it is,
   regardless of the other bits set alongside. */
TEST(issue_gh0318, options_from_flags) {
  EXPECT_TRUE(mdbx::env::operate_options(MDBX_NOSTICKYTHREADS).no_sticky_threads);
  EXPECT_TRUE(mdbx::env::operate_options(both_flags()).no_sticky_threads);
  EXPECT_TRUE(mdbx::env::operate_options(both_flags()).exclusive);
  EXPECT_FALSE(mdbx::env::operate_options(MDBX_EXCLUSIVE).no_sticky_threads);
  EXPECT_FALSE(mdbx::env::operate_options(MDBX_env_flags_t(0)).no_sticky_threads);
}

/* The forward and reverse conversions must be symmetric. */
TEST(issue_gh0318, roundtrip_make_flags) {
  mdbx::env::operate_parameters params;
  params.mode = mdbx::env::mode::write_file_io;
  params.options = mdbx::env::operate_options(both_flags());
  const MDBX_env_flags_t flags = params.make_flags(false, false);
  EXPECT_NE(flags & MDBX_NOSTICKYTHREADS, 0);
  EXPECT_NE(flags & MDBX_EXCLUSIVE, 0);
}

/* End-to-end: an environment opened with the converted options must accept a read
   transaction on the thread that holds the write transaction. */
TEST(issue_gh0318, read_on_writer_thread) {
  static const char *const path = "issue318.mdbx";
  db_remover guard(path);
  mdbx::env::operate_parameters params;
  params.mode = mdbx::env::mode::write_file_io;
  params.options = mdbx::env::operate_options(both_flags());
  mdbx::env_managed::create_parameters create;
  mdbx::env_managed env(mdbx::path(path), create, params);
  auto write = env.start_write();
  MDBX_txn *read = nullptr;
  EXPECT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &read), MDBX_SUCCESS);
  if (read)
    mdbx_txn_abort(read);
  write.abort();
}
