/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include <filesystem>

namespace {
const char *kPathA = "test-fluent-a";
const char *kPathB = "test-fluent-b";

mdbx::path path_of(const char *name) { return mdbx::path(name); }

uintmax_t file_size_of(const mdbx::path &p) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(p.string(), ec);
  return ec ? 0 : size;
}
} // namespace

TEST(ut_fluent_params, geometry_fluent_matches_c_api) {
  mdbx::env::remove(path_of(kPathA));
  mdbx::env::remove(path_of(kPathB));

  const intptr_t two_mb = 2 * 1024 * 1024;

  {
    mdbx::create_parameters params;
    params.geometry.set_size_lower(two_mb).set_size_now(two_mb);
    mdbx::env_managed env(path_of(kPathA), params, mdbx::operate_parameters());
  }

  {
    MDBX_env *raw = nullptr;
    ASSERT_EQ(mdbx_env_create(&raw), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_env_set_geometry(raw, -1, two_mb, -1, -1, -1, -1), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_env_open(raw, kPathB, (MDBX_env_flags_t)(unsigned(MDBX_CREATE) | unsigned(MDBX_NOSUBDIR)), 0664),
                   MDBX_SUCCESS);
    ASSERT_EQ(mdbx_env_close(raw), MDBX_SUCCESS);
  }

  const auto size_a = file_size_of(path_of(kPathA));
  const auto size_b = file_size_of(path_of(kPathB));
  EXPECT_EQ(size_a, size_b) << "fluent geometry must produce the same initial file size as mdbx_env_set_geometry";
  EXPECT_GE(size_a, uintmax_t(two_mb));

  mdbx::env::remove(path_of(kPathA));
  mdbx::env::remove(path_of(kPathB));
}

TEST(ut_fluent_params, runtime_options_roundtrip) {
  mdbx::env::remove(path_of("test-fluent-opt"));
  mdbx::env_managed env(path_of("test-fluent-opt"), mdbx::create_parameters(), mdbx::operate_parameters());

  env.set_sync_threshold(16 * 1024);
  EXPECT_EQ(env.sync_threshold(), 16u * 1024) << "sync threshold round-trips (in whole pages)";

  const mdbx::duration period(49152u); /* 0.75 s in 1/65536 units */
  env.set_sync_period(period);
  const auto actual = env.sync_period();
  EXPECT_EQ(actual.count(), period.count()) << "sync period round-trips in 1/65536-second units";

  EXPECT_GT(env.max_readers(), 0u);

  env.close();
  mdbx::env::remove(path_of("test-fluent-opt"));
}