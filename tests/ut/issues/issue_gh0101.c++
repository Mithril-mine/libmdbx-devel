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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::slice s; /* empty */
  EXPECT_FALSE(s) << "an empty slice must be false";
  mdbx::slice v = mdbx::slice("x", 1);
  EXPECT_TRUE(v) << "a non-empty slice must be true";
}

TEST(issue_gh0101, buffer_with_integer_length_copies) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string_view sv("abc");
  const buffer h = buffer::hex(sv);
  EXPECT_EQ((size_t)6, h.length()) << "hex of three bytes is six characters";
  EXPECT_EQ(0, memcmp(h.data(), "616263", 6)) << "hex of \"abc\" must be \"616263\"";
}

/* A literal 0 is a null pointer constant: buffer(0, 42) and assign(0, 42)
 * must stay ambiguous rather than use a null pointer with 42 bytes, while
 * nullptr and character pointers are accepted. */
template <typename B, typename = void> struct can_assign_int_int : std::false_type {};
template <typename B>
struct can_assign_int_int<B, decltype(void(std::declval<B &>().assign(0, 42)))> : std::true_type {};
template <typename B, typename = void> struct can_construct_int_int : std::false_type {};
template <typename B> struct can_construct_int_int<B, decltype(void(B(0, 42)))> : std::true_type {};
static_assert(!can_construct_int_int<buffer>::value, "buffer(0, 42) must not compile");
static_assert(!can_assign_int_int<buffer>::value, "assign(0, 42) must not compile");
static_assert(std::is_constructible<buffer, std::nullptr_t, int>::value, "buffer(nullptr, 0) must compile");
static_assert(std::is_constructible<buffer, char *, int>::value, "buffer(char *, int) must compile");

TEST(issue_gh0101, buffer_nullptr_must_be_empty_or_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* `nullptr` is a null pointer constant that exact-matches the new template;
   * it must yield an empty buffer for zero bytes and reject any non-zero
   * length instead of building a reference to address zero. */
  const buffer empty(nullptr, 0);
  EXPECT_TRUE(empty.empty()) << "buffer(nullptr, 0) must be an empty buffer";
  buffer assigned;
  assigned.assign(nullptr, 0);
  EXPECT_TRUE(assigned.empty()) << "assign(nullptr, 0) must yield an empty buffer";
  EXPECT_THROW(buffer(nullptr, 1), mdbx::bad_value_size) << "a null pointer with bytes must be rejected";
  EXPECT_THROW(assigned.assign(nullptr, 1), mdbx::bad_value_size)
      << "assign(nullptr, 1) must be rejected instead of dereferencing address zero";
}

/* A class converting to `const char *` copies `bytes` through its implicit
 * conversion, also when non-copyable or with an lvalue- or rvalue-only one,
 * rather than fall back to `(const char *, bool make_reference)`. */
struct noncopyable_chars {
  noncopyable_chars() = default;
  noncopyable_chars(const noncopyable_chars &) = delete;
  operator const char *() const { return "abcdef"; }
};
struct lvalue_chars {
  operator const char *() & { return "abcdef"; }
};
struct rvalue_chars {
  operator const char *() && { return "abcdef"; }
};
struct explicit_chars {
  operator char *() const { return const_cast<char *>("implicit"); }
  explicit operator const char *() const { return "explicit"; }
};
struct both_chars {
  operator const char *() & { return "lvalue"; }
  operator const char *() && { return "rvalue"; }
};

TEST(issue_gh0101, buffer_from_class_converting_to_chars) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  noncopyable_chars nc;
  lvalue_chars lc;
  const buffer a(nc, 3), b(lc, 3);
  EXPECT_EQ(3u, a.length());
  EXPECT_FALSE(a.is_reference());
  EXPECT_EQ(3u, b.length());
  EXPECT_FALSE(b.is_reference());
  buffer c, d;
  c.assign(nc, 3);
  d.assign(lc, 3);
  EXPECT_EQ(3u, c.length());
  EXPECT_EQ(3u, d.length());

  const buffer e(rvalue_chars{}, 3), f(both_chars{}, 3);
  EXPECT_EQ(3u, e.length());
  EXPECT_EQ(0, memcmp(f.data(), "rva", 3)) << "an rvalue must use the rvalue conversion";
  buffer g;
  g.assign(both_chars{}, 3);
  EXPECT_EQ(0, memcmp(g.data(), "rva", 3)) << "an rvalue must use the rvalue conversion";

  const explicit_chars ec;
  const buffer h(ec, 3);
  EXPECT_EQ(0, memcmp(h.data(), "imp", 3)) << "only implicit conversions, as for a `const char *` parameter";
}

/* A conversion that only the caller can access also copies `bytes`, rather
 * than fall back to `(const char *, bool make_reference)`. */
class private_chars {
  operator const char *() const { return "abcdef"; }

public:
  buffer make() const { return buffer(*this, 3); }
  void assign_to(buffer &b) const { b.assign(*this, 3); }
};

TEST(issue_gh0101, buffer_from_private_conversion) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const private_chars pc;
  const buffer c = pc.make();
  EXPECT_EQ(3u, c.length());
  EXPECT_FALSE(c.is_reference());
  buffer d;
  pc.assign_to(d);
  EXPECT_EQ(3u, d.length());
}

/* Issue #142: a null pointer with a non-zero length must be rejected by the
 * owning buffer and the non-owning slice alike (bad_value_size, as the
 * `buffer(nullptr, N)` guard added by #140 does), while the zero-length
 * spellings stay valid. The MDBX_RESERVE blank form is only reachable through
 * the explicit slice::blank_for_put_reserve() factory. */
TEST(issue_gh0101, null_pointer_with_nonzero_length_is_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* zero-length null spellings stay valid */
  const buffer empty1(nullptr, 0);
  EXPECT_TRUE(empty1.empty());
  buffer empty2;
  empty2.assign(nullptr, 0);
  EXPECT_TRUE(empty2.empty());
  const mdbx::slice ns(nullptr, size_t(0));
  EXPECT_TRUE(ns.empty());
  EXPECT_TRUE(ns.is_valid());

  /* non-zero-length null spellings are rejected, not turned into a dangling
   * non-empty reference (undefined behavior on use) */
  EXPECT_THROW(buffer(nullptr, 42), mdbx::bad_value_size);
  buffer b;
  EXPECT_THROW(b.assign(nullptr, 42), mdbx::bad_value_size);
  EXPECT_THROW(buffer(nullptr, 42, false), mdbx::bad_value_size);
  EXPECT_THROW(mdbx::slice(nullptr, size_t(42)), mdbx::bad_value_size);
  mdbx::slice s;
  EXPECT_THROW(s.assign(nullptr, size_t(42)), mdbx::bad_value_size);

  /* the rejected operations must not corrupt the object */
  EXPECT_TRUE(b.empty());
  EXPECT_TRUE(s.empty());
}

/* Issue #142: the MDBX_RESERVE blank form is spelled explicitly and stays
 * usable. */
TEST(issue_gh0101, slice_reserve_factory) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::slice blank = mdbx::slice::blank_for_put_reserve(42);
  EXPECT_EQ(42u, blank.length());
  EXPECT_EQ(nullptr, blank.data());
  EXPECT_FALSE(blank.is_valid()) << "a reserve blank is intentionally invalid until the C API fills it";

  /* zero-length reserve is just a null slice and stays valid */
  const mdbx::slice zero = mdbx::slice::blank_for_put_reserve(0);
  EXPECT_TRUE(zero.is_valid());
  EXPECT_TRUE(zero.empty());

  /* an oversize reserve request must still be rejected, exactly as the old
   * `slice(nullptr, N)` spelling did via check_length() */
  EXPECT_THROW(mdbx::slice::blank_for_put_reserve(size_t(MDBX_MAXDATASIZE) + 1), std::length_error)
      << "blank_for_put_reserve() must validate the length against MDBX_MAXDATASIZE";
}

TEST(issue_gh0101, invalidated_slice_can_rebind) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  mdbx::slice s("abc", 3);
  ASSERT_EQ(3u, s.length());
  s.invalidate();
  EXPECT_FALSE(s.is_valid()) << "invalidate() keeps the length but nulls the address";
  EXPECT_NO_THROW(s.assign("xyz", 3)) << "a nulled slice must be re-bindable via assign()";
  EXPECT_EQ(0, memcmp(s.data(), "xyz", 3));
}

/* Issue #144: try_as_pod() reports a length mismatch as nullopt instead of
 * throwing, while as_pod() keeps the throwing behavior for exact sizes. */
TEST(issue_gh0101, slice_try_as_pod) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
#if defined(__cpp_lib_optional) && __cpp_lib_optional >= 201606L
  const mdbx::slice eight("abcdefgh", 8);
  ASSERT_EQ(8u, sizeof(uint64_t));
  ASSERT_TRUE(eight.try_as_pod<uint64_t>().has_value()) << "an 8-byte slice must convert to a uint64_t";
  EXPECT_EQ(uint64_t(0x6867666564636261ULL), eight.try_as_pod<uint64_t>().value())
      << "little-endian bytes must be copied as-is";

  const mdbx::slice short_s("abc", 3);
  EXPECT_FALSE(short_s.try_as_pod<uint64_t>().has_value()) << "a 3-byte slice cannot be a uint64_t";

  EXPECT_EQ(uint64_t(0x6867666564636261ULL), eight.as_pod<uint64_t>())
      << "as_pod() keeps working for the exact-size case";
  EXPECT_THROW(short_s.as_pod<uint64_t>(), mdbx::bad_value_size) << "as_pod() with a wrong size still throws";
#else
  GTEST_SKIP() << "try_as_pod() requires <optional> (C++17)";
#endif /* __cpp_lib_optional >= 201606L */
}
