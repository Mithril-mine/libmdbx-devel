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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* A value long enough to allocate, shrunk so the next append with the
   * buffer's own bytes as the source must grow the storage again: the
   * reproducer from the issue, where the producer read the freed block. */
  buffer b;
  const std::string src(240, 'A');
  b.assign(mdbx::slice(src.data(), src.size()));
  b.shrink_to_fit();

  /* append_hex(b) on the buffer itself. */
  buffer hexsq(b);
  b.append_hex(b);
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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

template <typename APPLY>
static void check_self_append(const char *name, const buffer &input, const buffer &expected, APPLY apply) {
  buffer b(input);
  b.shrink_to_fit();
  ASSERT_LT(b.tailroom(), expected.size()) << name << ": the storage must grow";
  ASSERT_NO_THROW(apply(b)) << name;
  ASSERT_EQ(b.size(), input.size() + expected.size()) << name;
  EXPECT_EQ(0, memcmp(b.byte_ptr() + input.size(), expected.data(), expected.size()))
      << name << " of the buffer's own bytes must not read freed storage";
}

TEST(issue_gh0094, append_base58_and_decoded_of_itself) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string raw(150, 'A');
  const mdbx::slice bin(raw.data(), raw.size());
  const buffer hex = buffer::hex(bin), b58 = buffer::base58(bin), b64 = buffer::base64(bin);
  check_self_append("append_base58", buffer(bin), b58, [](buffer &b) { b.append_base58(b); });
  check_self_append("append_decoded_hex", hex, buffer(bin), [](buffer &b) { b.append_decoded_hex(b); });
  check_self_append("append_decoded_base58", b58, buffer(bin), [](buffer &b) { b.append_decoded_base58(b); });
  check_self_append("append_decoded_base64", b64, buffer(bin), [](buffer &b) { b.append_decoded_base64(b); });
}

TEST(issue_gh0094, append_and_add_header_from_own_head_or_tail_room) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* A source lying in the own storage, but outside the content, is not carried
   * over when the storage grows, so it must be copied out beforehand. */
  std::string src;
  for (size_t i = 0; i < 200; ++i)
    src += char('a' + i % 26);

  buffer b(mdbx::slice(src.data(), src.size()));
  b.shrink_to_fit();
  const mdbx::slice head(b.byte_ptr(), 100);
  b.remove_prefix(100);
  ASSERT_LT(b.tailroom(), head.size());
  b.append(head);
  ASSERT_EQ(200u, b.size());
  EXPECT_EQ(0, memcmp(b.byte_ptr(), src.data() + 100, 100));
  EXPECT_EQ(0, memcmp(b.byte_ptr() + 100, src.data(), 100)) << "append() from the own headroom";

  buffer h(mdbx::slice(src.data(), src.size()));
  h.shrink_to_fit();
  const mdbx::slice tail(h.byte_ptr() + 100, 100);
  h.remove_suffix(100);
  ASSERT_LT(h.headroom(), tail.size());
  h.add_header(tail);
  ASSERT_EQ(200u, h.size());
  EXPECT_EQ(0, memcmp(h.byte_ptr(), src.data() + 100, 100)) << "add_header() from the own tailroom";
  EXPECT_EQ(0, memcmp(h.byte_ptr() + 100, src.data(), 100));
}

TEST(issue_gh0095, move_assign_reference_into_own_storage_keeps_storage) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* Such move-assign is noexcept for the legacy allocator, so it must not
   * allocate (out-of-memory would std::terminate), but move the bytes in place. */
  using legacy_buffer = mdbx::buffer<mdbx::legacy_allocator, mdbx::default_capacity_policy>;
  std::string src;
  for (size_t i = 0; i < 1000; ++i)
    src += char('a' + i % 26);
  legacy_buffer a(mdbx::slice(src.data(), src.size()));
  ASSERT_EQ(0u, a.headroom());
  const size_t capacity = a.capacity();
  const void *const storage = a.data();

  legacy_buffer ref(mdbx::slice(a.byte_ptr() + 10, 200), true);
  a = std::move(ref);
  ASSERT_EQ(200u, a.size());
  EXPECT_EQ(0, memcmp(a.data(), src.data() + 10, 200));
  EXPECT_EQ(capacity, a.capacity()) << "the storage must be kept, without reallocation";
  EXPECT_EQ(storage, a.data()) << "the storage must be kept, without reallocation";
}

TEST(issue_gh0094, append_and_add_header_from_content_into_tailroom) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* A source that starts in the content and runs into the tailroom is not
   * wholly carried over when the storage grows, so it must be copied out too. */
  buffer a(0, mdbx::slice("ABCD"), 3);
  const size_t tr = a.tailroom();
  ASSERT_GT(tr, 0u);
  memset(a.end(), 'x', tr);
  a.append(mdbx::slice(a.byte_ptr() + 2, 2 + tr));
  EXPECT_EQ("ABCDCD" + std::string(tr, 'x'), std::string(a.char_ptr(), a.length()))
      << "append() from the own content and tailroom";

  buffer h(0, mdbx::slice("ABCD"), 3);
  const size_t htr = h.tailroom();
  ASSERT_EQ(0u, h.headroom());
  memset(h.end(), 'y', htr);
  h.add_header(mdbx::slice(h.byte_ptr() + 2, 2 + htr));
  EXPECT_EQ("CD" + std::string(htr, 'y') + "ABCD", std::string(h.char_ptr(), h.length()))
      << "add_header() from the own content and tailroom";
}