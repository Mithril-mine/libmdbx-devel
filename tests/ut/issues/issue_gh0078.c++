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

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <string>
#include <utility>

#if defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/78
// Two buffer defects: (1) `buffer::operator=(std::string_view)` was declared
// `noexcept` even though assigning copies the view's bytes into owned storage
// and can throw bad_alloc, turning a recoverable out-of-memory into process
// termination via std::terminate(); (2) `is_freestanding()` used `offset <
// capacity` instead of `<=`, misclassifying an owned buffer whose data starts
// at the very end of the silo as a non-owning reference.

TEST(issue_gh0078, buffer_string_view_assignment_is_not_noexcept) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  static_assert(!noexcept(std::declval<mdbx::buffer<> &>() = std::declval<const std::string_view &>()),
                "buffer::operator=(string_view) allocates, so it must be allowed to throw on out-of-memory");
}

TEST(issue_gh0078, buffer_string_view_assignment_roundtrip) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::buffer<> b;
  std::string_view payload("hello, world");
  b = payload;
  EXPECT_TRUE(b.is_freestanding());
  EXPECT_EQ(payload.size(), b.length());
  EXPECT_EQ(std::string(payload), std::string(b.char_ptr(), b.length()));
}

TEST(issue_gh0078, owned_buffer_reserve_keeps_freestanding) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* an owned buffer whose payload is moved around the silo (reserve a
   * headroom) keeps owning its allocation: capacity(), headroom() and
   * content_modality() must stay consistent with is_freestanding() */
  mdbx::buffer<> b;
  b.assign(std::string_view("payload"));
  EXPECT_TRUE(b.is_freestanding());
  const size_t initial_capacity = b.capacity();
  b.resize(0);
  b.reserve_headroom(initial_capacity);
  EXPECT_TRUE(b.is_freestanding()) << "reserving headroom must not turn an owned buffer into a reference";
  EXPECT_EQ(initial_capacity, b.headroom());
  EXPECT_EQ(mdbx::buffer<>::modality::allocated, b.content_modality());
  EXPECT_GE(b.capacity(), initial_capacity);
}

TEST(issue_gh0078, owned_buffer_data_at_silo_end_is_freestanding) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* the `<=` boundary itself: an allocated buffer filled exactly to capacity,
   * then emptied from the front, has data_offset == capacity */
  mdbx::buffer<> b;
  b.assign(std::string(1000, 'x'));
  ASSERT_EQ(mdbx::buffer<>::modality::allocated, b.content_modality());
  const size_t capacity = b.capacity();
  b.append(std::string(b.tailroom(), 'y'));
  ASSERT_EQ(capacity, b.capacity());
  ASSERT_EQ(0u, b.tailroom());
  b.remove_prefix(b.length());
  EXPECT_TRUE(b.is_freestanding()) << "an owned buffer with its data at the very end of the silo is not a reference";
  EXPECT_EQ(capacity, b.headroom());
  EXPECT_EQ(mdbx::buffer<>::modality::allocated, b.content_modality());
}

TEST(issue_gh0078, inplace_buffer_data_at_storage_end_stays_inplace) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* the same boundary for the inplace storage: an empty payload at its very end
   * is still inplace, so a move yields an owned buffer, not a reference into
   * the moved-from object */
  mdbx::buffer<> b;
  b.assign(std::string(1, 'x'));
  ASSERT_EQ(mdbx::buffer<>::modality::inplace, b.content_modality());
  b.assign(std::string(b.capacity(), 'x'));
  ASSERT_EQ(mdbx::buffer<>::modality::inplace, b.content_modality());
  b.remove_prefix(b.length());
  EXPECT_EQ(mdbx::buffer<>::modality::inplace, b.content_modality());
  mdbx::buffer<> moved(std::move(b));
  EXPECT_TRUE(moved.is_freestanding()) << "a moved inplace buffer must not refer into the moved-from object";
  EXPECT_EQ(mdbx::buffer<>::modality::inplace, moved.content_modality());
  EXPECT_EQ(0u, moved.length());
}

#else /* !(string_view available) */

TEST(issue_gh0078, buffer_string_view_assignment_is_not_noexcept) {
  GTEST_SKIP() << "requires <string_view> (C++17), disabled since the requested standard is older";
}

TEST(issue_gh0078, buffer_string_view_assignment_roundtrip) {
  GTEST_SKIP() << "requires <string_view> (C++17), disabled since the requested standard is older";
}

TEST(issue_gh0078, owned_buffer_reserve_keeps_freestanding) {
  GTEST_SKIP() << "requires <string_view> (C++17), disabled since the requested standard is older";
}

TEST(issue_gh0078, owned_buffer_data_at_silo_end_is_freestanding) {
  GTEST_SKIP() << "requires <string_view> (C++17), disabled since the requested standard is older";
}

TEST(issue_gh0078, inplace_buffer_data_at_storage_end_stays_inplace) {
  GTEST_SKIP() << "requires <string_view> (C++17), disabled since the requested standard is older";
}

#endif /* __cpp_lib_string_view >= 201606L */