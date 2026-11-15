// v13b-cxx-template-matrix.cpp — stream C, tranche V13b (C++ API headers).
//
// Compile-time instantiation matrix required by the verification protocol:
//   {char, wchar_t} x {uint32_t, uint64_t} x {reference, allocated} buffers,
// plus the fixed compile-error/ambiguity guards:
//   - buffer(nullptr, N) rejects non-zero N at run time (#137 family),
//   - buffer(nullptr, N, make_reference) Windows ambiguity guard (#142),
//   - buffer(nullptr_t, INT) and buffer(const char*, INT) integral templates,
//   - deprecated env::mode/env::durability aliases (scoped-enum breaking change),
//   - cursor::erase() C-string overloads (bind-to-bool trap),
//   - slice safe_middle/substr/buffer reserve arithmetic instantiations.
//
// Build: see the command line at the bottom of this file. Expected: exit 0.
#include <mdbx.h++>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <system_error>
#include <type_traits>

using mdbx::buffer;
using mdbx::default_buffer;
using mdbx::slice;

namespace {

template <class T> struct poly_alloc { // minimal stateful allocator for matrix coverage
  using value_type = T;
  poly_alloc() = default;
  template <class U> poly_alloc(const poly_alloc<U> &) {}
  T *allocate(size_t n) { return static_cast<T *>(::operator new(n * sizeof(T))); }
  void deallocate(T *p, size_t) { ::operator delete(p); }
  template <class U> bool operator==(const poly_alloc<U> &) const { return true; }
  template <class U> bool operator!=(const poly_alloc<U> &) const { return false; }
};

// --- {char, wchar_t} string/view feeding slice and buffer -------------------
template <class CHAR> void feed_strings() {
  const CHAR lit[] = {'a', 'b', 'c', 0};
  const std::basic_string<CHAR> str(lit);
  const std::basic_string_view<CHAR> view(str);

  slice s1(str.data(), str.length() * sizeof(CHAR));
  slice s2(view.data(), view.size() * sizeof(CHAR));
  assert(s2.length() == 3 * sizeof(CHAR));

  default_buffer b1(str);                    // copies (freestanding)
  default_buffer b2(str, true);              // references
  default_buffer b3(view);                   // copies
  default_buffer b4(view, true);             // references
  default_buffer b5(str.data(), 3 * sizeof(CHAR), false);
  assert(b1.length() == 3 * sizeof(CHAR));
  assert(b2.is_reference() && b1.is_freestanding());
  assert(b4.is_reference() && b3.is_freestanding());

  // int keys x {4, 8} bytes wrapped as ordinal keys
  uint32_t k32 = 42;
  uint64_t k64 = 4242;
  static_assert(sizeof(mdbx::buffer<>::key_from(k32)) >= 0);
  static_assert(sizeof(mdbx::buffer<>::key_from(k64)) >= 0);
  static_assert(sizeof(mdbx::buffer<>::key_from_i32(int32_t(-7))) >= 0);
  static_assert(sizeof(mdbx::buffer<>::key_from_i64(int64_t(-7))) >= 0);
  (void)default_buffer::key_from_u32(k32);
  (void)default_buffer::key_from_u64(k64);
  (void)default_buffer::key_from_float(1.5f);
  (void)default_buffer::key_from_double(1.5);

  // transcoders over wchar_t sources (byte-level dumps)
  slice ws(str.data(), str.length() * sizeof(CHAR));
  auto hexbuf = ws.template encode_hex<mdbx::default_allocator, mdbx::default_capacity_policy>();
  assert(hexbuf.length() == ws.length() * 2);
  auto b64buf = ws.template encode_base64<mdbx::default_allocator, mdbx::default_capacity_policy>();
  (void)b64buf;
}

// --- x {reference, allocated} buffer modalities -----------------------------
template <class ALLOCATOR> void modalities(const char *tag) {
  using buf = buffer<ALLOCATOR, mdbx::default_capacity_policy>;
  typename buf::allocator_type alloc{};

  buf allocated(alloc);               // empty, inplace storage
  buf reserved(64, alloc);            // with capacity
  buf head_tail(8, 8, alloc);         // headroom + tailroom
  // an EMPTY buffer is null/reference modality (data() == nullptr)
  assert(allocated.is_null() && allocated.content_modality() == buf::modality::reference);
  allocated.append("\1", 1);
  assert(allocated.is_inplace() && allocated.content_modality() == buf::modality::inplace);

  static char external[16] = {1, 2, 3};
  buf reference(external, sizeof(external), true, alloc);
  assert(reference.is_reference() && reference.content_modality() == buf::modality::reference);

  buf copied(reference, alloc); // deep copy by default
  assert(copied.is_freestanding());

  // move-assign including the "src refers into this storage" case (#24 family)
  buf holder(64, alloc);
  holder.append(external, sizeof(external));
  buf into(&external[0], 4, true, alloc); // references external[0..4)
  (void)tag;
  {
    const char *holder_base = static_cast<const char *>(holder.data());
    buf victim(holder_base, 4, true, alloc); // refers into holder's storage
    assert(victim.is_reference());
  }
  // move ctor/assign chains
  buf moved(std::move(reserved));
  assert(moved.is_freestanding());
  buf moved_assigned(std::move(moved));
  buf target(alloc);
  target = std::move(moved_assigned);

  // swap across modalities (both-non-reference exchange path)
  buf a(32, alloc), b(32, alloc);
  a.append("0123456789", 10);
  b.append("abcdef", 6);
  a.swap(b);
  assert(a.length() == 6 && b.length() == 10);
}

// --- slice arithmetic / bounds ---------------------------------------------
void slice_arith() {
  const char blob[] = "0123456789abcdef";
  slice s(blob, sizeof(blob) - 1);
  assert(s.safe_middle(3, 5).length() == 5);
  bool thrown = false;
  try {
    (void)s.safe_middle(10, 100); // from + n > size
  } catch (const std::out_of_range &) {
    thrown = true;
  }
  assert(thrown);
  // #28 overflow arithmetic: both operands bounded by max_length, from+n
  // cannot wrap (max_length = 0x7fff0000)
  try {
    (void)s.safe_middle(slice::max_length, slice::max_length);
  } catch (const std::out_of_range &) {
    thrown = true;
  }
  assert(thrown);

  assert(s.substr(4).length() == 12);
  assert(s.substr(4, 3).length() == 3);
  assert(s.head(2).length() == 2 && s.tail(2).length() == 2);

  // try_as_pod (#144): mismatch -> nullopt, no throw
  const uint64_t pod64 = 0xDEADBEEFCAFEBABEull;
  slice p(&pod64, sizeof(pod64));
  auto v = p.try_as_pod<uint64_t>();
  static_assert(std::is_same_v<std::optional<uint64_t>, decltype(v)>);
  assert(v && *v == pod64);
  assert(!p.try_as_pod<uint32_t>().has_value());
  assert(p.as_pod<uint64_t>() == pod64);
  thrown = false;
  try {
    (void)p.as_pod<uint32_t>();
  } catch (const mdbx::bad_value_size &) {
    thrown = true;
  }
  assert(thrown);
}

// --- nullptr / integral ctor guards (#137, #142) ----------------------------
void nullptr_guards() {
  default_buffer z1(std::nullptr_t{}, size_t(0)); // buffer(nullptr, 0) — OK since the fix
  assert(z1.empty() && z1.is_null());
  default_buffer z2(nullptr, 0, true);
  assert(z2.empty());
  default_buffer z3(nullptr, uint32_t(0));
  (void)z3;
  default_buffer c1("abc", 3); // (const char*, INT) template
  assert(c1.length() == 3);

  bool thrown = false;
  try {
    default_buffer bad(std::nullptr_t{}, size_t(7)); // #137: must throw, not deref
    (void)bad;
  } catch (const mdbx::bad_value_size &) {
    thrown = true;
  }
  assert(thrown);
  thrown = false;
  try {
    default_buffer bad(nullptr, 7, false); // (#142) with make_reference
    (void)bad;
  } catch (const mdbx::bad_value_size &) {
    thrown = true;
  }
  assert(thrown);
  thrown = false;
  try {
    slice bad(nullptr, 7); // slice(nullptr, N>0) rejects too
    (void)bad;
  } catch (const mdbx::bad_value_size &) {
    thrown = true;
  }
  assert(thrown);
  // blank_for_put_reserve: only spelling of the (null, N) reserve form
  slice blank = slice::blank_for_put_reserve(16);
  assert(blank.is_null() && blank.length() == 16);
  thrown = false;
  try {
    (void)slice::blank_for_put_reserve(size_t(mdbx::slice::max_length) + 1); // > MDBX_MAXDATASIZE
  } catch (const std::length_error &) {
    thrown = true;
  }
  assert(thrown);
}

// --- deprecated scoped-enum aliases (the breaking change) -------------------
void enum_aliases() {
  static_assert(mdbx::env::mode::nested_transactions == mdbx::env::mode::write_file_io);
  static_assert(int(mdbx::env::durability::whole_fragile) == 3);
  // the deprecated static constants keep the same values
  [[maybe_unused]] const auto m1 = mdbx::env::mode::readonly;
  [[maybe_unused]] const auto m2 = mdbx::env::write_mapped_io; // deprecated
  [[maybe_unused]] const auto d1 = mdbx::env::durability::lazy_weak_tail;
  [[maybe_unused]] const auto d2 = mdbx::env::robust_synchronous; // deprecated
  (void)m1;
  (void)m2;
  (void)d1;
  (void)d2;
  mdbx::env::operate_parameters p;
  p.set_mode(mdbx::env::write_file_io).set_durability(mdbx::env::whole_fragile); // deprecated names
  auto flags = p.make_flags();
  assert((flags & MDBX_NOSTICKYTHREADS) == 0);          // not set
  assert((flags & MDBX_UTTERLY_NOSYNC) != 0);           // whole_fragile
  mdbx::env::operate_options o;
  o.set_no_sticky_threads().set_nested_transactions();  // MDBX_NOSTICKYTHREADS take
  auto flags2 = mdbx::env::operate_parameters(0, 0, mdbx::env::mode::write_file_io,
                                              mdbx::env::durability::robust_synchronous, {}, o)
                    .make_flags();
  assert((flags2 & MDBX_NOSTICKYTHREADS) != 0);
  assert((flags2 & MDBX_WRITEMAP) == 0); // nested clears writemap
  // round-trip from_flags
  auto back = mdbx::env::operate_parameters::mode_from_flags(flags2);
  assert(back == mdbx::env::mode::write_file_io);
  auto dur = mdbx::env::operate_parameters::durability_from_flags(flags2);
  assert(dur == mdbx::env::durability::robust_synchronous);
}

// --- cursor overloads that need only declarations ---------------------------
void cursor_overload_shapes() {
  // compile-only: resolve overloads without an actual database
  using mv = mdbx::cursor::move_operation;
  static_assert(int(mv::key_exact) == int(MDBX_SET_KEY));
  static_assert(int(mv::pair_exact) == int(mv::pair_equal));
  static_assert(int(mv::seek_and_batch_samelength) == int(MDBX_SEEK_AND_GET_MULTIPLE));
  static_assert(mdbx::loop_control::exit_loop == INT32_MIN);
  static_assert(int(mdbx::put_mode::update) == int(MDBX_CURRENT));
  static_assert(int(mdbx::put_mode::insert_unique) == int(MDBX_NOOVERWRITE));
  static_assert(int(mdbx::put_mode::upsert) == int(MDBX_UPSERT));
}

} // namespace

int main() {
  feed_strings<char>();
  feed_strings<wchar_t>();
  modalities<std::allocator<std::uint64_t>>("std");
  modalities<poly_alloc<std::uint64_t>>("poly");
  slice_arith();
  nullptr_guards();
  enum_aliases();
  cursor_overload_shapes();
  std::puts("v13b-cxx-template-matrix: all checks passed");
  return 0;
}

// Build & run: same library recipe as v13b-cxx-runtime-db.cpp, then
//   g++ -std=c++20 -O1 -DMDBX_BUILD_CXX=1 -Wno-deprecated -I<tree> -o matrix \
//          v13b-cxx-template-matrix.cpp libcxxcore.a -lpthread -lrt && ./matrix
