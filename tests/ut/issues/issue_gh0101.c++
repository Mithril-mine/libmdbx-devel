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
  const private_chars pc;
  const buffer c = pc.make();
  EXPECT_EQ(3u, c.length());
  EXPECT_FALSE(c.is_reference());
  buffer d;
  pc.assign_to(d);
  EXPECT_EQ(3u, d.length());
}
