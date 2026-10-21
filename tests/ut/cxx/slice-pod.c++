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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

TEST(ut_slice_pod, as_pod_typed) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const uint64_t value = 0x1122334455667788ull;
  const mdbx::slice s(&value, sizeof(value));
  EXPECT_EQ(s.as_pod<uint64_t>(), value);
  EXPECT_EQ(s.as_uint64(), value);
  EXPECT_EQ(s.size(), 8u);

  const uint32_t v32 = 0xdeadbeefu;
  const mdbx::slice s32(&v32, sizeof(v32));
  EXPECT_EQ(s32.as_uint32(), v32);

  EXPECT_THROW(s.as_uint32(), mdbx::bad_value_size); // wrong size
  EXPECT_THROW(s.as_pod<uint16_t>(), mdbx::bad_value_size);
  EXPECT_NO_THROW((void)s.as_pod<uint64_t>());
}

TEST(ut_slice_pod, byte_ptr_and_views) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice s("Hello");
  EXPECT_EQ(s.byte_ptr()[0], mdbx::byte('H'));
  EXPECT_EQ(s.end_byte_ptr()[-1], mdbx::byte('o'));
  EXPECT_EQ(s.end_byte_ptr() - s.byte_ptr(), 5);
  EXPECT_EQ(std::string(s.byte_ptr(), s.byte_ptr() + s.size()), "Hello");
  EXPECT_FALSE(s.empty());
  EXPECT_TRUE(mdbx::slice().empty());
}

TEST(ut_slice_pod, construction_variants) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_TRUE(mdbx::slice::null().empty());
  EXPECT_FALSE(mdbx::slice::invalid().is_valid());
  EXPECT_TRUE(empty.is_valid());

  const mdbx::slice cstr("abc");
  EXPECT_EQ(cstr.size(), 3u);

  const std::string str = "Hello, world!";
  const mdbx::slice from_str(str);
  EXPECT_EQ(from_str.size(), str.size());
  EXPECT_EQ(from_str, mdbx::slice(str.data(), str.size()));

  const uint64_t v = 42;
  const mdbx::slice from_pod(&v, sizeof(v));
  EXPECT_EQ(from_pod.size(), sizeof(v));
  EXPECT_EQ(from_pod.as_uint64(), 42u);
}