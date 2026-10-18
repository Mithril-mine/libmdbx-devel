#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

using buffer = mdbx::buffer<>;

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/100
// C++ API build failures: three documented templates failed to compile on
// first use, and selecting the experimental filesystem broke the build or
// silently changed mdbx::path. The templates now compile and work:
// buffer(std::to_string(..)) copies, append(basic_string_view) appends the
// raw bytes, slice(std::span) points at the span data.

TEST(issue_gh0100, documented_templates_compile_and_work) {
  /* buffer(std::string&&): the removed const&& constructor let rvalue strings
   * use the lvalue one, which copies. */
  buffer a(std::to_string(42));
  EXPECT_EQ(0, memcmp(a.data(), "42", 2)) << "buffer(to_string(42)) must hold \"42\"";
  EXPECT_EQ(2u, a.length());

  /* buffer::append(basic_string_view): appends the raw bytes. */
  buffer b;
  b.append(mdbx::slice("x", 1));
  b.append(std::string_view("ab"));
  EXPECT_EQ(3u, b.length());
  EXPECT_EQ(0, memcmp(b.data(), "xab", 3)) << "append(string_view) must append the bytes";

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
  /* slice(std::span): points at the span data with its byte size. */
  const int arr[3] = {1, 2, 3};
  mdbx::slice s{std::span<const int>(arr)};
  EXPECT_EQ((size_t)12, s.length()) << "slice(span) must span the whole array";
  EXPECT_EQ(arr, static_cast<const void *>(s.data()));
#endif
}

TEST(issue_gh0100, experimental_filesystem_selector) {
  /* This is a compile-time property: the branch must follow the macro that
   * the detection defines, not the misspelled old name. A wrong selection
   * either fails the build (C++17) or silently falls back mdbx::path to a
   * string (C++14), so a runtime probe cannot fail; compile is the test. */
  static_assert(MDBX_USING_CXX_EXPERIMENTAL_FILESYSTEM == 0 || MDBX_USING_CXX_EXPERIMENTAL_FILESYSTEM == 1,
                "the experimental filesystem selector must be a boolean");
  /* The path type exists and is a filesystem path or a string, whichever the
   * configuration chose; the point is that it must follow the renamed macro. */
  mdbx::path p("issue100");
  EXPECT_FALSE(p.empty());
}