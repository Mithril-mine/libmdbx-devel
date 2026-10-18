#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/94
// and https://github.com/Mithril-mine/libmdbx-devel/issues/95 — buffer
// operations that read the buffer's own storage after releasing or
// relocating it:
//  #94: append_hex/append_base58/append_base64 on the buffer itself called
//       reserve_tailroom() first, which relocated the storage, and the
//       producer then read the freed block, appending garbage.
//  #95: move-assign from a reference-modality buffer whose const_data()
//       pointed into this buffer's own storage freed that storage first and
//       then copied from it.

using buffer = mdbx::default_buffer;

static std::string hex_of(const buffer &b) {
  std::string out;
  out.reserve(b.size() * 2);
  static const char *const digits = "0123456789abcdef";
  const uint8_t *p = static_cast<const uint8_t *>(b.data());
  for (size_t i = 0; i < b.size(); ++i) {
    out += digits[p[i] >> 4];
    out += digits[p[i] & 15];
  }
  return out;
}

TEST(issue_gh0094, append_transcoders_produce_before_relocating) {
  /* A value long enough to allocate, shrunk so the next append with the
   * buffer's own bytes as the source must grow the storage again: the
   * reproducer from the issue, where the producer read the freed block. */
  buffer b;
  const std::string src(240, 'A');
  b.assign(mdbx::slice(src.data(), src.size()));
  b.shrink_to_fit();

  /* append_hex(b) on the buffer itself. */
  buffer hexsq(b);
  b.append_hex(hexsq);
  const std::string expected_hex = hex_of(hexsq);
  ASSERT_EQ(b.size(), src.size() + expected_hex.size());
  const char *const appended = static_cast<const char *>(b.data()) + src.size();
  EXPECT_EQ(0, memcmp(appended, expected_hex.data(), expected_hex.size()))
      << "append_hex of the buffer's own bytes must not read freed storage";

  /* Same for base64 on the buffer itself: 240 'A' bytes encode to 320 'Q'. */
  buffer b64;
  b64.assign(mdbx::slice(src.data(), src.size()));
  b64.shrink_to_fit();
  const size_t b64_before = b64.size();
  b64.append_base64(b64);
  ASSERT_EQ(b64.size(), b64_before + 320);
  const char *const b64_appended = static_cast<const char *>(b64.data()) + b64_before;
  /* 240 bytes of 0x41 ('A') encode to 320 base64 chars: "QUFB" x 80. */
  std::string expected_b64;
  expected_b64.reserve(320);
  for (unsigned i = 0; i < 80; ++i)
    expected_b64 += "QUFB";
  EXPECT_EQ(0, memcmp(b64_appended, expected_b64.data(), expected_b64.size()))
      << "append_base64 of the buffer's own bytes must not read freed storage";
}

TEST(issue_gh0095, move_assign_reference_into_own_storage) {
  buffer a;
  const std::string src(64, 'Q');
  a.assign(mdbx::slice(src.data(), src.size()));

  /* Make `b` a reference-modality buffer that refers into `a`'s storage, then
   * move-assign it back into `a`: the old code released `a`'s storage first
   * and then copied from the freed block. */
  buffer b;
  b.assign_reference(a.data(), a.size());

  a = std::move(b);
  EXPECT_EQ(src.size(), a.size());
  EXPECT_EQ(0, memcmp(a.data(), src.data(), src.size()))
      << "move-assigning a reference into the target's own storage must keep the data";
}