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

#include "../test-env.h++"

TEST(ut_slice_view, iterators_and_front_back) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice s("Hello");
  size_t count = 0;
  for (mdbx::byte b : s) {
    EXPECT_NE(b, mdbx::byte(0));
    ++count;
  }
  EXPECT_EQ(count, 5u);
  EXPECT_EQ(s.front(), mdbx::byte('H'));
  EXPECT_EQ(s.back(), mdbx::byte('o'));
  EXPECT_NE(s.begin(), s.end());
  EXPECT_EQ(s.end() - s.begin(), 5);
}

TEST(ut_slice_view, substr_and_middle) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice s("Hello, world!");
  EXPECT_EQ(s.substr(0, 5), mdbx::slice("Hello"));
  EXPECT_EQ(s.substr(7, 5), mdbx::slice("world"));
  EXPECT_EQ(s.substr(7), mdbx::slice("world!"));
  EXPECT_EQ(s.substr(s.size()), mdbx::slice());
  EXPECT_THROW(s.substr(s.size() + 1), std::out_of_range);
  EXPECT_EQ(s.head(5), mdbx::slice("Hello"));
  EXPECT_EQ(s.tail(6), mdbx::slice("world!"));
  EXPECT_EQ(s.middle(7, 5), mdbx::slice("world"));
  EXPECT_THROW(s.safe_head(s.size() + 1), std::out_of_range);
  EXPECT_THROW(s.safe_tail(s.size() + 1), std::out_of_range);
  EXPECT_THROW(s.safe_middle(7, s.size()), std::out_of_range);
  EXPECT_NO_THROW(s.safe_head(5));
}

TEST(ut_slice_view, find_family) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice s("Hello, world!");
  EXPECT_EQ(s.find(mdbx::slice("world")), 7u);
  EXPECT_EQ(s.find(mdbx::slice("world"), 8), mdbx::slice::npos);
  EXPECT_EQ(s.find(mdbx::slice("xyz")), mdbx::slice::npos);
  EXPECT_EQ(s.find(mdbx::slice()), 0u);
  EXPECT_EQ(s.find(mdbx::byte('o')), 4u);
  EXPECT_EQ(s.find(mdbx::byte('z')), mdbx::slice::npos);
  EXPECT_EQ(s.rfind(mdbx::byte('o')), 8u);
  EXPECT_EQ(s.rfind(mdbx::slice("world")), 7u);
  EXPECT_EQ(s.rfind(mdbx::slice("xyz")), mdbx::slice::npos);
  EXPECT_EQ(s.find_first_of(mdbx::slice(" ,!")), 5u);
  EXPECT_EQ(s.find_last_of(mdbx::slice(" ,!")), 12u);
  EXPECT_EQ(s.find_first_of(mdbx::byte(',')), 5u);
  EXPECT_EQ(s.find_last_of(mdbx::byte('!')), 12u);
  EXPECT_EQ(s.find_first_not_of(mdbx::slice("Helo")), 5u);
  EXPECT_EQ(s.find_last_not_of(mdbx::slice("!d")), 10u);
  EXPECT_EQ(s.find_first_not_of(mdbx::byte('H')), 1u);
  EXPECT_EQ(s.find_last_not_of(mdbx::byte('!')), 11u);
  EXPECT_TRUE(s.contains(mdbx::slice("world")));
  EXPECT_FALSE(s.contains(mdbx::slice("xyz")));
  EXPECT_LT(s.compare(mdbx::slice("Jello")), 0);
  EXPECT_GT(s.compare(mdbx::slice("Gello")), 0);
  EXPECT_EQ(s.compare(mdbx::slice("Hello, world!")), 0);
}

TEST(ut_slice_view, remove_prefix_suffix_and_at) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::slice s("Hello, world!");
  s.remove_prefix(7);
  EXPECT_EQ(s, mdbx::slice("world!"));
  s.remove_suffix(1);
  EXPECT_EQ(s, mdbx::slice("world"));
  EXPECT_EQ(s.at(0), mdbx::byte('w'));
  EXPECT_EQ(s.at(4), mdbx::byte('d'));
  // Explicit member call: on 32-bit MSVC `s[2]` is ambiguous between the
  // member operator[] and the built-in subscript via `operator MDBX_val *()`.
  EXPECT_EQ(s.operator[](2), mdbx::byte('r'));
  EXPECT_THROW(s.at(s.size()), std::out_of_range);
  EXPECT_THROW(s.safe_remove_prefix(s.size() + 1), std::out_of_range);
  EXPECT_THROW(s.safe_remove_suffix(s.size() + 1), std::out_of_range);
  s.safe_remove_prefix(2);
  EXPECT_EQ(s, mdbx::slice("rld"));
  s.safe_remove_suffix(2);
  EXPECT_EQ(s, mdbx::slice("r"));
}

TEST(ut_slice_view, prefix_suffix) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice s("Hello, world!");
  EXPECT_TRUE(s.starts_with(mdbx::slice("Hello")));
  EXPECT_FALSE(s.starts_with(mdbx::slice("World")));
  EXPECT_TRUE(s.ends_with(mdbx::slice("world!")));
  EXPECT_FALSE(s.ends_with(mdbx::slice("World!")));
  EXPECT_TRUE(s.starts_with(mdbx::slice()));
  EXPECT_TRUE(s.ends_with(mdbx::slice()));
}

TEST(ut_slice_view, invalid_sentinel_flows_freely) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* The deliberately-corrupted INVALID() sentinel is a legit state: it must
   * flow through the whole assignment/copy/move machinery without exceptions
   * and assert-failures (its null + max length is accepted by the internal
   * unchecked primitives), staying invalid in every copy. */
  mdbx::slice sentinel = mdbx::slice::invalid();
  EXPECT_FALSE(sentinel.is_valid());

  mdbx::slice dst(mdbx::slice("Hello"));
  ASSERT_TRUE(dst.is_valid());
  dst = sentinel; /* move-assign of the sentinel */
  EXPECT_FALSE(dst.is_valid());

  mdbx::slice copied = sentinel; /* copy of the sentinel */
  EXPECT_FALSE(copied.is_valid());

  mdbx::slice moved(::std::move(sentinel)); /* move-ctor of the sentinel */
  EXPECT_FALSE(moved.is_valid());

  /* a valid slice re-assigned from an invalid one stays valid */
  mdbx::slice valid(mdbx::slice("data"));
  valid.assign(sentinel);
  EXPECT_FALSE(valid.is_valid());
  valid.assign(mdbx::slice("restored"));
  EXPECT_TRUE(valid.is_valid());
}

TEST(ut_slice_view, assign_exception_safety) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  static const char data[] = "Hello";
  mdbx::slice s(data);
  /* an oversize length must throw and leave the slice unchanged */
  EXPECT_THROW(s.assign(::MDBX_val{const_cast<char *>(data), mdbx::slice::max_length + 1}), std::length_error);
  EXPECT_EQ(s.size(), 5u);
  EXPECT_EQ(s.data(), static_cast<const void *>(data));
  EXPECT_EQ(s, mdbx::slice("Hello"));
  /* a null base with a non-zero length must throw and leave the slice unchanged */
  EXPECT_THROW(s.assign(::MDBX_val{nullptr, 3}), mdbx::bad_value_size);
  EXPECT_EQ(s.size(), 5u);
  EXPECT_EQ(s.data(), static_cast<const void *>(data));
  EXPECT_EQ(s, mdbx::slice("Hello"));
  /* a valid assignment still works */
  s.assign(::MDBX_val{const_cast<char *>(data + 1), 3});
  EXPECT_EQ(s, mdbx::slice("ell"));
  /* the direct ptr/bytes overload: the same contract */
  EXPECT_THROW(s.assign(static_cast<const void *>(data), mdbx::slice::max_length + 1), std::length_error);
  EXPECT_EQ(s.size(), 3u);
  EXPECT_EQ(s, mdbx::slice("ell"));
  s.assign(static_cast<const void *>(data), 5);
  EXPECT_EQ(s, mdbx::slice("Hello"));
  /* re-binding via the string overloads must work equally well */
  EXPECT_NO_THROW(s.assign(::std::string_view("xyz")));
  EXPECT_EQ(s, mdbx::slice("xyz"));
}