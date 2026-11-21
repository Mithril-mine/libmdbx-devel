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

namespace {
/* An allocator whose copy constructor throws, to probe the conditional
 * noexcept of the allocator-aware constructions. */
template <typename T> struct throwing_copy_allocator {
  using value_type = T;
  throwing_copy_allocator() noexcept = default;
  throwing_copy_allocator(const throwing_copy_allocator &) { throw ::std::runtime_error("copy is not allowed"); }
  template <typename U> throwing_copy_allocator(const throwing_copy_allocator<U> &) noexcept {}
  T *allocate(size_t n) { return static_cast<T *>(::operator new(n * sizeof(T))); }
  void deallocate(T *ptr, size_t) noexcept { ::operator delete(ptr); }
};
template <typename A, typename B>
bool operator==(const throwing_copy_allocator<A> &, const throwing_copy_allocator<B> &) noexcept {
  return true;
}
template <typename A, typename B>
bool operator!=(const throwing_copy_allocator<A> &, const throwing_copy_allocator<B> &) noexcept {
  return false;
}
} // namespace

TEST(ut_buffer_extra, modality_transitions) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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

TEST(ut_buffer_extra, clear_and_reserve_and_assign_reference_validation) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::buffer<> b(mdbx::slice("Hello"), mdbx::default_allocator());
  ASSERT_EQ(b.size(), 5u);
  /* clear_and_reserve(): an oversize capacity and an inverted headroom
   * relation must throw and leave the buffer untouched */
  EXPECT_THROW(b.clear_and_reserve(mdbx::slice::max_length + 1), std::length_error);
  EXPECT_THROW(b.clear_and_reserve(64, 65), mdbx::bad_value_size);
  EXPECT_EQ(b.size(), 5u);
  EXPECT_EQ(b, mdbx::buffer<>(mdbx::slice("Hello"), mdbx::default_allocator()));
  /* a valid reservation works and keeps the buffer empty */
  b.clear_and_reserve(64, 8);
  EXPECT_EQ(b.size(), 0u);
  /* assign_reference(): a rejected oversize length must throw and leave
   * the buffer untouched */
  EXPECT_THROW(b.assign_reference(static_cast<const void *>("data"), mdbx::slice::max_length + 1), std::length_error);
  EXPECT_EQ(b.size(), 0u);
  b.assign_reference(static_cast<const void *>("data"), 4);
  EXPECT_EQ(b, mdbx::buffer<>(mdbx::slice("data"), mdbx::default_allocator()));
}

TEST(ut_buffer_extra, buffer_pair_spec_alloc_noexcept) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* The allocator-aware constructor is noexcept exactly when copying the
   * allocator cannot throw. */
  using default_pair = mdbx::buffer_pair_spec<mdbx::default_allocator, mdbx::default_capacity_policy>;
  EXPECT_TRUE((::std::is_nothrow_copy_constructible<default_pair::allocator_type>::value));
  EXPECT_TRUE(noexcept(default_pair(default_pair::allocator_type())));

  using throwing_alloc = throwing_copy_allocator<uint64_t>;
  static_assert(!::std::is_nothrow_copy_constructible<throwing_alloc>::value, "the probe allocator must throw on copy");
  using throwing_pair = mdbx::buffer_pair_spec<throwing_copy_allocator<char>, mdbx::default_capacity_policy>;
  EXPECT_FALSE(noexcept(throwing_pair(throwing_alloc())));
}

TEST(ut_buffer_extra, stl_members) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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