// > dist-cutoff-begin
#pragma once
#include "decl_slice.h++"
namespace mdbx {
// < dist-cutoff-end

#if MDBX_HAVE_CXX20_CONCEPTS || defined(DOXYGEN)

/** \concept MutableByteProducer
 *  \interface MutableByteProducer
 *  \brief MutableByteProducer C++20 concept */
template <typename T>
concept MutableByteProducer = requires(T a, char array[42]) {
  { a.is_empty() } -> std::same_as<bool>;
  { a.envisage_result_length() } -> std::same_as<size_t>;
  { a.write_bytes(&array[0], size_t(42)) } -> std::same_as<char *>;
};

/** \concept ImmutableByteProducer
 *  \interface ImmutableByteProducer
 *  \brief ImmutableByteProducer C++20 concept */
template <typename T>
concept ImmutableByteProducer = requires(const T &a, char array[42]) {
  { a.is_empty() } -> std::same_as<bool>;
  { a.envisage_result_length() } -> std::same_as<size_t>;
  { a.write_bytes(&array[0], size_t(42)) } -> std::same_as<char *>;
};

/** \concept SliceTranscoder
 *  \interface SliceTranscoder
 *  \brief SliceTranscoder C++20 concept */
template <typename T>
concept SliceTranscoder = ImmutableByteProducer<T> && requires(const slice &source, const T &a) {
  T(source);
  { a.is_erroneous() } -> std::same_as<bool>;
};

#endif /* MDBX_HAVE_CXX20_CONCEPTS */

/// \brief Hexadecimal encoder which satisfy \ref SliceTranscoder concept.
struct LIBMDBX_API to_hex {
  const slice source;
  const bool uppercase = false;
  const unsigned wrap_width = 0;
  MDBX_CXX11_CONSTEXPR to_hex(const slice &source, bool uppercase = false, unsigned wrap_width = 0) noexcept
      : source(source), uppercase(uppercase), wrap_width(wrap_width) {
    MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(SliceTranscoder, to_hex);
  }

  /// \brief Returns a string with a hexadecimal dump of a passed slice.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a hexadecimal dump of a passed slice.
  template <class ALLOCATOR = default_allocator, typename CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> as_buffer(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns the buffer size in bytes needed for hexadecimal
  /// dump of a passed slice.
  MDBX_CXX11_CONSTEXPR size_t envisage_result_length() const noexcept {
    const size_t bytes = source.length() << 1;
    return wrap_width ? bytes + bytes / wrap_width : bytes;
  }

  /// \brief Fills the buffer by hexadecimal dump of a passed slice.
  /// \throws std::length_error if given buffer is too small.
  char *write_bytes(char *dest, size_t dest_size) const;

  /// \brief Output hexadecimal dump of passed slice to the std::ostream.
  /// \throws std::ios_base::failure corresponding to std::ostream::write() behaviour.
  ::std::ostream &output(::std::ostream &out) const;

  /// \brief Checks whether a passed slice is empty,
  /// and therefore there will be no output bytes.
  bool is_empty() const noexcept { return source.empty(); }

  /// \brief Checks whether the content of a passed slice is a valid data
  /// and could be encoded or unexpectedly not.
  bool is_erroneous() const noexcept { return false; }
};

/// \brief [Base58](https://en.wikipedia.org/wiki/Base58) encoder which satisfy
/// \ref SliceTranscoder concept.
struct LIBMDBX_API to_base58 {
  const slice source;
  const unsigned wrap_width = 0;
  MDBX_CXX11_CONSTEXPR
  to_base58(const slice &source, unsigned wrap_width = 0) noexcept : source(source), wrap_width(wrap_width) {
    MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(SliceTranscoder, to_base58);
  }

  /// \brief Returns a string with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of a passed slice.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of a passed slice.
  template <class ALLOCATOR = default_allocator, typename CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> as_buffer(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns the buffer size in bytes needed for
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of passed slice.
  MDBX_CXX11_CONSTEXPR size_t envisage_result_length() const noexcept {
    const size_t bytes = (source.length() * 11 + 7) / 8;
    return wrap_width ? bytes + bytes / wrap_width : bytes;
  }

  /// \brief Fills the buffer by [Base58](https://en.wikipedia.org/wiki/Base58) dump of passed slice.
  /// \throws std::length_error if given buffer is too small.
  char *write_bytes(char *dest, size_t dest_size) const;

  /// \brief Output [Base58](https://en.wikipedia.org/wiki/Base58) dump of passed slice to the std::ostream.
  /// \throws std::ios_base::failure corresponding to std::ostream::write() behaviour.
  ::std::ostream &output(::std::ostream &out) const;

  /// \brief Checks whether a passed slice is empty, and therefore there will be no output bytes.
  bool is_empty() const noexcept { return source.empty(); }

  /// \brief Checks whether the content of a passed slice is a valid data and could be encoded or unexpectedly not.
  bool is_erroneous() const noexcept { return false; }
};

/// \brief [Base64](https://en.wikipedia.org/wiki/Base64) encoder which satisfy
/// \ref SliceTranscoder concept.
struct LIBMDBX_API to_base64 {
  const slice source;
  const unsigned wrap_width = 0;
  MDBX_CXX11_CONSTEXPR
  to_base64(const slice &source, unsigned wrap_width = 0) noexcept : source(source), wrap_width(wrap_width) {
    MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(SliceTranscoder, to_base64);
  }

  /// \brief Returns a string with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of a passed slice.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of a passed slice.
  template <class ALLOCATOR = default_allocator, typename CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> as_buffer(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns the buffer size in bytes needed for
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of passed slice.
  MDBX_CXX11_CONSTEXPR size_t envisage_result_length() const noexcept {
    const size_t bytes = (source.length() + 2) / 3 * 4;
    return wrap_width ? bytes + bytes / wrap_width : bytes;
  }

  /// \brief Fills the buffer by [Base64](https://en.wikipedia.org/wiki/Base64)
  /// dump of passed slice.
  /// \throws std::length_error if given buffer is too small.
  char *write_bytes(char *dest, size_t dest_size) const;

  /// \brief Output [Base64](https://en.wikipedia.org/wiki/Base64)
  /// dump of passed slice to the std::ostream.
  /// \throws std::ios_base::failure corresponding to std::ostream::write() behaviour.
  ::std::ostream &output(::std::ostream &out) const;

  /// \brief Checks whether a passed slice is empty,
  /// and therefore there will be no output bytes.
  bool is_empty() const noexcept { return source.empty(); }

  /// \brief Checks whether the content of a passed slice is a valid data
  /// and could be encoded or unexpectedly not.
  bool is_erroneous() const noexcept { return false; }
};

/// \brief Hexadecimal decoder which satisfy \ref SliceTranscoder concept.
struct LIBMDBX_API from_hex {
  const slice source;
  const bool ignore_spaces = false;
  MDBX_CXX11_CONSTEXPR from_hex(const slice &source, bool ignore_spaces = false) noexcept
      : source(source), ignore_spaces(ignore_spaces) {
    MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(SliceTranscoder, from_hex);
  }

  /// \brief Decodes hexadecimal dump from a passed slice to returned string.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes hexadecimal dump from a passed slice to returned buffer.
  template <class ALLOCATOR = default_allocator, typename CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> as_buffer(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns the number of bytes needed for conversion
  /// hexadecimal dump from a passed slice to decoded data.
  MDBX_CXX11_CONSTEXPR size_t envisage_result_length() const noexcept { return source.length() >> 1; }

  /// \brief Fills the destination with data decoded from hexadecimal dump from a passed slice.
  /// \throws std::length_error if given buffer is too small.
  char *write_bytes(char *dest, size_t dest_size) const;

  /// \brief Checks whether a passed slice is empty, and therefore there will be no output bytes.
  bool is_empty() const noexcept { return source.empty(); }

  /// \brief Checks whether the content of a passed slice is a valid hexadecimal
  /// dump, and therefore there could be decoded or not.
  bool is_erroneous() const noexcept;
};

/// \brief [Base58](https://en.wikipedia.org/wiki/Base58) decoder which satisfy
/// \ref SliceTranscoder concept.
struct LIBMDBX_API from_base58 {
  const slice source;
  const bool ignore_spaces = false;
  MDBX_CXX11_CONSTEXPR from_base58(const slice &source, bool ignore_spaces = false) noexcept
      : source(source), ignore_spaces(ignore_spaces) {
    MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(SliceTranscoder, from_base58);
  }

  /// \brief Decodes [Base58](https://en.wikipedia.org/wiki/Base58) dump from a
  /// passed slice to returned string.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes [Base58](https://en.wikipedia.org/wiki/Base58) dump from a
  /// passed slice to returned buffer.
  template <class ALLOCATOR = default_allocator, typename CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> as_buffer(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns the number of bytes needed for conversion
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump from a passed slice to decoded data.
  MDBX_CXX11_CONSTEXPR size_t envisage_result_length() const noexcept {
    return source.length() /* могут быть все нули кодируемые один-к-одному */;
  }

  /// \brief Fills the destination with data decoded from
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump from a passed slice.
  /// \throws std::length_error if given buffer is too small.
  char *write_bytes(char *dest, size_t dest_size) const;

  /// \brief Checks whether a passed slice is empty, and therefore there will be no output bytes.
  bool is_empty() const noexcept { return source.empty(); }

  /// \brief Checks whether the content of a passed slice is a valid
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump, and therefore there could be decoded or not.
  bool is_erroneous() const noexcept;
};

/// \brief [Base64](https://en.wikipedia.org/wiki/Base64) decoder which satisfy
/// \ref SliceTranscoder concept.
struct LIBMDBX_API from_base64 {
  const slice source;
  const bool ignore_spaces = false;
  MDBX_CXX11_CONSTEXPR from_base64(const slice &source, bool ignore_spaces = false) noexcept
      : source(source), ignore_spaces(ignore_spaces) {
    MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(SliceTranscoder, from_base64);
  }

  /// \brief Decodes [Base64](https://en.wikipedia.org/wiki/Base64) dump from a
  /// passed slice to returned string.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes [Base64](https://en.wikipedia.org/wiki/Base64) dump from a
  /// passed slice to returned buffer.
  template <class ALLOCATOR = default_allocator, typename CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> as_buffer(const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns the number of bytes needed for conversion
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump from a passed slice to decoded data.
  MDBX_CXX11_CONSTEXPR size_t envisage_result_length() const noexcept { return (source.length() + 3) / 4 * 3; }

  /// \brief Fills the destination with data decoded from
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump from a passed slice.
  /// \throws std::length_error if given buffer is too small.
  char *write_bytes(char *dest, size_t dest_size) const;

  /// \brief Checks whether a passed slice is empty,
  /// and therefore there will be no output bytes.
  bool is_empty() const noexcept { return source.empty(); }

  /// \brief Checks whether the content of a passed slice is a valid
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump, and therefore there
  /// could be decoded or not.
  bool is_erroneous() const noexcept;
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
