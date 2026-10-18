#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

using buffer = mdbx::buffer<>;

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/101
// Overload-resolution traps: `if (s)` on a non-const slice was always true
// (so an absent key looked present); a 3-argument cursor::move() silently
// used the value as the boolean flag; buffers passed to move() became
// references into the map; buffer(p, 3)/assign(p, 3) ignored the length; and
// hex/base58/base64 of a string_view or MDBX_val encoded the object instead
// of the data.

TEST(issue_gh0101, slice_bool_is_null_aware) {
  mdbx::slice s; /* empty */
  EXPECT_FALSE(s) << "an empty slice must be false";
  mdbx::slice v = mdbx::slice("x", 1);
  EXPECT_TRUE(v) << "a non-empty slice must be true";
}

TEST(issue_gh0101, buffer_with_integer_length_copies) {
  const char *const p = "abcdef";
  buffer b(p, 3);
  EXPECT_EQ(3u, b.length()) << "buffer(p, 3) must copy 3 bytes, not run strlen";
  EXPECT_EQ(0, memcmp(b.data(), "abc", 3));
  EXPECT_FALSE(b.is_reference()) << "the buffer must own its 3 bytes";

  buffer a;
  a.assign(p, 3);
  EXPECT_EQ(3u, a.length()) << "assign(p, 3) must copy 3 bytes";
  EXPECT_EQ(0, memcmp(a.data(), "abc", 3));
  EXPECT_FALSE(a.is_reference());
}

TEST(issue_gh0101, hex_of_string_like_encodes_data) {
  const std::string_view sv("abc");
  const buffer h = buffer::hex(sv);
  EXPECT_EQ((size_t)6, h.length()) << "hex of three bytes is six characters";
  EXPECT_EQ(0, memcmp(h.data(), "616263", 6)) << "hex of \"abc\" must be \"616263\"";
}