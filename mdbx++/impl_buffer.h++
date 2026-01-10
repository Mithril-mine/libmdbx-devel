// > dist-cutoff-begin
#pragma once
#include "decl_buffer.h++"
#include "decl_core.h++"
#include "decl_cursor.h++"
#include "decl_env.h++"
#include "decl_exceptions.h++"
#include "decl_slice.h++"
#include "decl_transcoders.h++"
#include "decl_txn.h++"
namespace mdbx {
// < dist-cutoff-end

template <class ALLOCATOR, typename CAPACITY_POLICY>
MDBX_CXX20_CONSTEXPR buffer<ALLOCATOR, CAPACITY_POLICY>::buffer(const struct slice &src, bool make_reference,
                                                                const allocator_type &alloc)
    : inherited(src), silo_(alloc) {
  if (!make_reference)
    insulate();
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY>::buffer(const txn &transaction, const struct slice &src,
                                                  const allocator_type &alloc)
    : buffer(src, !transaction.is_dirty(src.data()), alloc) {}

template <class ALLOCATOR, typename CAPACITY_POLICY>
inline ::std::ostream &operator<<(::std::ostream &out, const buffer<ALLOCATOR, CAPACITY_POLICY> &it) {
  return (it.is_freestanding() ? out << "buf-" << it.headroom() << "." << it.tailroom() : out << "ref-") << it.slice();
}

#if !(defined(__MINGW__) || defined(__MINGW32__) || defined(__MINGW64__)) || defined(LIBMDBX_EXPORTS)
MDBX_EXTERN_API_TEMPLATE(LIBMDBX_API_TYPE, buffer<legacy_allocator, default_capacity_policy>);

#if MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR
MDBX_EXTERN_API_TEMPLATE(LIBMDBX_API_TYPE, buffer<polymorphic_allocator, default_capacity_policy>);
#endif /* MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR */
#endif /* !MinGW || MDBX_EXPORTS */

/// \brief Default buffer.
using default_buffer = buffer<default_allocator, default_capacity_policy>;

//------------------------------------------------------------------------------

template <class ALLOCATOR, class CAPACITY_POLICY, MDBX_CXX20_CONCEPT(MutableByteProducer, PRODUCER)>
inline buffer<ALLOCATOR, CAPACITY_POLICY> make_buffer(PRODUCER &producer, const ALLOCATOR &alloc) {
  if (MDBX_LIKELY(!producer.is_empty()))
    MDBX_CXX20_LIKELY {
      buffer<ALLOCATOR, CAPACITY_POLICY> result(producer.envisage_result_length(), alloc);
      result.set_end(producer.write_bytes(result.end_char_ptr(), result.tailroom()));
      return result;
    }
  return buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY, MDBX_CXX20_CONCEPT(ImmutableByteProducer, PRODUCER)>
inline buffer<ALLOCATOR, CAPACITY_POLICY> make_buffer(const PRODUCER &producer, const ALLOCATOR &alloc) {
  if (MDBX_LIKELY(!producer.is_empty()))
    MDBX_CXX20_LIKELY {
      buffer<ALLOCATOR, CAPACITY_POLICY> result(producer.envisage_result_length(), alloc);
      result.set_end(producer.write_bytes(result.end_char_ptr(), result.tailroom()));
      return result;
    }
  return buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, MDBX_CXX20_CONCEPT(MutableByteProducer, PRODUCER)>
inline string<ALLOCATOR> make_string(PRODUCER &producer, const ALLOCATOR &alloc) {
  string<ALLOCATOR> result(alloc);
  if (MDBX_LIKELY(!producer.is_empty()))
    MDBX_CXX20_LIKELY {
      result.resize(producer.envisage_result_length());
      result.resize(producer.write_bytes(const_cast<char *>(result.data()), result.capacity()) - result.data());
    }
  return result;
}

template <class ALLOCATOR, MDBX_CXX20_CONCEPT(ImmutableByteProducer, PRODUCER)>
inline string<ALLOCATOR> make_string(const PRODUCER &producer, const ALLOCATOR &alloc) {
  string<ALLOCATOR> result(alloc);
  if (MDBX_LIKELY(!producer.is_empty()))
    MDBX_CXX20_LIKELY {
      result.resize(producer.envisage_result_length());
      result.resize(producer.write_bytes(const_cast<char *>(result.data()), result.capacity()) - result.data());
    }
  return result;
}

template <class ALLOCATOR> string<ALLOCATOR> to_hex::as_string(const ALLOCATOR &alloc) const {
  return make_string<ALLOCATOR>(*this, alloc);
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
buffer<ALLOCATOR, CAPACITY_POLICY> to_hex::as_buffer(const ALLOCATOR &alloc) const {
  return make_buffer<ALLOCATOR, CAPACITY_POLICY>(*this, alloc);
}

template <class ALLOCATOR> string<ALLOCATOR> to_base58::as_string(const ALLOCATOR &alloc) const {
  return make_string<ALLOCATOR>(*this, alloc);
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
buffer<ALLOCATOR, CAPACITY_POLICY> to_base58::as_buffer(const ALLOCATOR &alloc) const {
  return make_buffer<ALLOCATOR, CAPACITY_POLICY>(*this, alloc);
}

template <class ALLOCATOR> string<ALLOCATOR> to_base64::as_string(const ALLOCATOR &alloc) const {
  return make_string<ALLOCATOR>(*this, alloc);
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
buffer<ALLOCATOR, CAPACITY_POLICY> to_base64::as_buffer(const ALLOCATOR &alloc) const {
  return make_buffer<ALLOCATOR, CAPACITY_POLICY>(*this, alloc);
}

template <class ALLOCATOR> string<ALLOCATOR> from_hex::as_string(const ALLOCATOR &alloc) const {
  return make_string<ALLOCATOR>(*this, alloc);
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
buffer<ALLOCATOR, CAPACITY_POLICY> from_hex::as_buffer(const ALLOCATOR &alloc) const {
  return make_buffer<ALLOCATOR, CAPACITY_POLICY>(*this, alloc);
}

template <class ALLOCATOR> string<ALLOCATOR> from_base58::as_string(const ALLOCATOR &alloc) const {
  return make_string<ALLOCATOR>(*this, alloc);
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
buffer<ALLOCATOR, CAPACITY_POLICY> from_base58::as_buffer(const ALLOCATOR &alloc) const {
  return make_buffer<ALLOCATOR, CAPACITY_POLICY>(*this, alloc);
}

template <class ALLOCATOR> string<ALLOCATOR> from_base64::as_string(const ALLOCATOR &alloc) const {
  return make_string<ALLOCATOR>(*this, alloc);
}

template <class ALLOCATOR, typename CAPACITY_POLICY>
buffer<ALLOCATOR, CAPACITY_POLICY> from_base64::as_buffer(const ALLOCATOR &alloc) const {
  return make_buffer<ALLOCATOR, CAPACITY_POLICY>(*this, alloc);
}

template <class ALLOCATOR>
inline string<ALLOCATOR> slice::as_hex_string(bool uppercase, unsigned wrap_width, const ALLOCATOR &alloc) const {
  return to_hex(*this, uppercase, wrap_width).as_string<ALLOCATOR>(alloc);
}

template <class ALLOCATOR>
inline string<ALLOCATOR> slice::as_base58_string(unsigned wrap_width, const ALLOCATOR &alloc) const {
  return to_base58(*this, wrap_width).as_string<ALLOCATOR>(alloc);
}

template <class ALLOCATOR>
inline string<ALLOCATOR> slice::as_base64_string(unsigned wrap_width, const ALLOCATOR &alloc) const {
  return to_base64(*this, wrap_width).as_string<ALLOCATOR>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY> slice::encode_hex(bool uppercase, unsigned wrap_width,
                                                            const ALLOCATOR &alloc) const {
  return to_hex(*this, uppercase, wrap_width).as_buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY> slice::encode_base58(unsigned wrap_width, const ALLOCATOR &alloc) const {
  return to_base58(*this, wrap_width).as_buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY> slice::encode_base64(unsigned wrap_width, const ALLOCATOR &alloc) const {
  return to_base64(*this, wrap_width).as_buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY> slice::hex_decode(bool ignore_spaces, const ALLOCATOR &alloc) const {
  return from_hex(*this, ignore_spaces).as_buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY> slice::base58_decode(bool ignore_spaces, const ALLOCATOR &alloc) const {
  return from_base58(*this, ignore_spaces).as_buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

template <class ALLOCATOR, class CAPACITY_POLICY>
inline buffer<ALLOCATOR, CAPACITY_POLICY> slice::base64_decode(bool ignore_spaces, const ALLOCATOR &alloc) const {
  return from_base64(*this, ignore_spaces).as_buffer<ALLOCATOR, CAPACITY_POLICY>(alloc);
}

//------------------------------------------------------------------------------

MDBX_CXX14_CONSTEXPR intptr_t pair::compare_fast(const pair &a, const pair &b) noexcept {
  const auto diff = slice::compare_fast(a.key, b.key);
  return diff ? diff : slice::compare_fast(a.value, b.value);
}

MDBX_CXX14_CONSTEXPR intptr_t pair::compare_lexicographically(const pair &a, const pair &b) noexcept {
  const auto diff = slice::compare_lexicographically(a.key, b.key);
  return diff ? diff : slice::compare_lexicographically(a.value, b.value);
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator==(const pair &a, const pair &b) noexcept {
  return a.key.length() == b.key.length() && a.value.length() == b.value.length() &&
         memcmp(a.key.data(), b.key.data(), a.key.length()) == 0 &&
         memcmp(a.value.data(), b.value.data(), a.value.length()) == 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator<(const pair &a, const pair &b) noexcept {
  return pair::compare_lexicographically(a, b) < 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator>(const pair &a, const pair &b) noexcept {
  return pair::compare_lexicographically(a, b) > 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator<=(const pair &a, const pair &b) noexcept {
  return pair::compare_lexicographically(a, b) <= 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator>=(const pair &a, const pair &b) noexcept {
  return pair::compare_lexicographically(a, b) >= 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool operator!=(const pair &a, const pair &b) noexcept {
  return a.key.length() != b.key.length() || a.value.length() != b.value.length() ||
         memcmp(a.key.data(), b.key.data(), a.key.length()) != 0 ||
         memcmp(a.value.data(), b.value.data(), a.value.length()) != 0;
}

#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR auto operator<=>(const pair &a, const pair &b) noexcept {
  return pair::compare_lexicographically(a, b);
}
#endif /* __cpp_impl_three_way_comparison */

/// \brief Combines pair of buffers for key and value to hold an operands for certain operations.
template <typename BUFFER>
using buffer_pair = buffer_pair_spec<typename BUFFER::allocator_type, typename BUFFER::reservation_policy>;

/// \brief Default pair of buffers.
using default_buffer_pair = buffer_pair<default_buffer>;

//------------------------------------------------------------------------------

inline ::std::ostream &operator<<(::std::ostream &out, const to_hex &wrapper) { return wrapper.output(out); }
inline ::std::ostream &operator<<(::std::ostream &out, const to_base58 &wrapper) { return wrapper.output(out); }
inline ::std::ostream &operator<<(::std::ostream &out, const to_base64 &wrapper) { return wrapper.output(out); }

MDBX_NOTHROW_PURE_FUNCTION inline bool slice::is_hex(bool ignore_spaces) const noexcept {
  return !from_hex(*this, ignore_spaces).is_erroneous();
}

MDBX_NOTHROW_PURE_FUNCTION inline bool slice::is_base58(bool ignore_spaces) const noexcept {
  return !from_base58(*this, ignore_spaces).is_erroneous();
}

MDBX_NOTHROW_PURE_FUNCTION inline bool slice::is_base64(bool ignore_spaces) const noexcept {
  return !from_base64(*this, ignore_spaces).is_erroneous();
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
