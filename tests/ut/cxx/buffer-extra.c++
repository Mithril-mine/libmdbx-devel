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

TEST(ut_buffer_extra, modality_transitions) {
  using buffer = mdbx::buffer<>;

  buffer empty;
  EXPECT_FALSE(empty.is_freestanding());
  EXPECT_FALSE(empty.is_inplace());
  EXPECT_TRUE(empty.is_reference());
  EXPECT_EQ(empty.content_modality(), buffer::modality::reference);

  const mdbx::slice abc_payload("abc");
  buffer small_buf(abc_payload, /*make_reference=*/false);
  EXPECT_TRUE(small_buf.is_freestanding());
  EXPECT_TRUE(small_buf.is_inplace());
  EXPECT_FALSE(small_buf.is_reference());
  EXPECT_EQ(small_buf.content_modality(), buffer::modality::inplace);
  EXPECT_EQ(small_buf.size(), 3u);

  const mdbx::slice xyz_payload("xyz");
  buffer ref(xyz_payload, /*make_reference=*/true);
  EXPECT_FALSE(ref.is_freestanding());
  EXPECT_TRUE(ref.is_reference());
  EXPECT_EQ(ref.content_modality(), buffer::modality::reference);
  ref.make_freestanding();
  EXPECT_TRUE(ref.is_freestanding());
  EXPECT_EQ(ref.content_modality(), buffer::modality::inplace);

  buffer big;
  big.reserve(1024);
  EXPECT_TRUE(big.is_freestanding());
  EXPECT_FALSE(big.is_inplace());
  EXPECT_EQ(big.content_modality(), buffer::modality::allocated);
}

TEST(ut_buffer_extra, stl_members) {
  mdbx::buffer<> buf;
  EXPECT_EQ(buf.size(), 0u);
  buf.reserve(64);
  EXPECT_TRUE(buf.is_freestanding());
  EXPECT_GE(buf.capacity(), 64u);

  buf.resize(8, mdbx::byte(0xAB));
  EXPECT_EQ(buf.size(), 8u);
  size_t count = 0;
  for (mdbx::byte b : buf) {
    EXPECT_EQ(b, mdbx::byte(0xAB));
    ++count;
  }
  EXPECT_EQ(count, 8u);

  buf.resize(4);
  EXPECT_EQ(buf.size(), 4u);
  EXPECT_EQ(buf.front(), mdbx::byte(0xAB));
  EXPECT_EQ(buf.back(), mdbx::byte(0xAB));

  buf.shrink_to_fit();
  buf.clear();
  EXPECT_EQ(buf.size(), 0u);
}

TEST(ut_buffer_extra, self_append_and_add_header) {
  const mdbx::slice abc_payload("abc");
  mdbx::buffer<> buf(abc_payload, /*make_reference=*/false);
  const mdbx::slice view = buf;
  buf.append(view);
  EXPECT_TRUE(buf == mdbx::slice("abcabc"));
  const mdbx::slice view2 = buf; /* re-capture after possible relocation */
  buf.append(view2);
  EXPECT_TRUE(buf == mdbx::slice("abcabcabcabc"));

  const mdbx::slice world_payload("world");
  mdbx::buffer<> hdr(world_payload, /*make_reference=*/false);
  const mdbx::slice hview = hdr;
  hdr.add_header(hview);
  EXPECT_TRUE(hdr == mdbx::slice("worldworld"));
  const mdbx::slice hview2 = hdr; /* re-capture after possible relocation */
  hdr.add_header(hview2);
  EXPECT_TRUE(hdr == mdbx::slice("worldworldworldworld"));
}