// > dist-cutoff-begin
#pragma once
#if defined(DOXYGEN) || (defined(__cpp_lib_optional) && __cpp_lib_optional >= 201606L)
#include <optional>
#endif /* __cpp_lib_optional >= 201606L */
#include "decl_exceptions.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief References a data located outside the slice.
///
/// The `slice` is similar in many ways to `std::string_view`, but it
/// implements specific capabilities and manipulates with bytes but
/// not a characters.
///
/// \note A slice is non-owning: it references data allocated elsewhere and
/// never copies or releases it. The referenced data must remain valid as long
/// as the slice is used. Passing a temporary (e.g. a `std::string` converted
/// inline in a call argument, like `txn.insert(map, mdbx::slice(std::to_string(k)), ...)`)
/// is safe: a temporary lives until the end of the full-expression and libmdbx
/// copies the bytes during the call. In contrast, storing such a slice beyond
/// the statement leaves it dangling.
///
/// \copydetails MDBX_val
struct LIBMDBX_API_TYPE slice : public ::MDBX_val {
  /// \todo slice& operator<<(slice&, ...) for reading
  /// \todo key-to-value (parse/unpack) functions
  /// \todo template<class X> key(X); for decoding keys while reading

  enum : size_t { max_length = MDBX_MAXDATASIZE, npos = size_t(-1) };

  /// \brief Create an empty slice.
  MDBX_CXX11_CONSTEXPR slice() noexcept;

  /// \brief Create a slice that refers to [0,bytes-1] of memory bytes pointed by ptr.
  /// \param ptr    The address of the data to be referenced.
  /// \param bytes  The number of bytes to be referenced.
  MDBX_CXX14_CONSTEXPR slice(const void *ptr, size_t bytes);

  /// \brief Create a slice that refers to [begin,end] of memory bytes.
  /// \param begin  The address of the first byte to be referenced.
  /// \param end    The address just after the last byte to be referenced.
  MDBX_CXX14_CONSTEXPR slice(const void *begin, const void *end);

  /// \brief Create a slice that refers to text[0,strlen(text)-1].
  /// \param text  The null-terminated string literal to be referenced.
  template <size_t SIZE> MDBX_CXX14_CONSTEXPR slice(const char (&text)[SIZE]) : slice(text, SIZE - 1) {
    MDBX_CONSTEXPR_ASSERT(SIZE > 0 && text[SIZE - 1] == '\0');
  }
  /// \brief Create a slice that refers to c_str[0,strlen(c_str)-1].
  /// \param c_str  The null-terminated string to be referenced.
  explicit MDBX_CXX17_CONSTEXPR slice(const char *c_str);

  /// \brief Create a slice that refers to the contents of "str".
  /// \note 'explicit' to avoid reference to the temporary std::string instance.
  /// The slice is non-owning: it references the string's data without copying.
  /// Passing a temporary string inline in a call argument is safe (the temporary
  /// lives until the end of the full-expression and libmdbx copies the bytes
  /// during the call), but storing the resulting slice beyond the statement
  /// leaves it dangling — use mdbx::buffer to own a copy.
  /// \param str  The string which content is to be referenced.
  template <class CHAR, class T, class A>
  explicit MDBX_CXX20_CONSTEXPR slice(const ::std::basic_string<CHAR, T, A> &str)
      : slice(str.data(), str.length() * sizeof(CHAR)) {}

  /// \brief Create a slice that refers to the contents of the given \ref MDBX_val.
  /// \param src  The source \ref MDBX_val instance.
  MDBX_CXX14_CONSTEXPR slice(const MDBX_val &src);
  /// \brief Copy constructor.
  /// \param src  The source slice.
  MDBX_CXX11_CONSTEXPR slice(const slice &src) noexcept = default;
  /// \brief Takes over a temporary \ref MDBX_val instance: the slice refers to its
  ///   bytes and the moved-from `src.iov_base` is set to null, while
  ///   `src.iov_len` is kept (matching the library's own moved-from state);
  ///   the slice does not own the bytes.
  /// \param src  The source \ref MDBX_val to be moved from.
  MDBX_CXX14_CONSTEXPR slice(MDBX_val &&src);
  /// \brief Move constructor: the source slice is invalidated.
  /// \param src  The source slice to be moved from.
  MDBX_CXX14_CONSTEXPR slice(slice &&src) noexcept;

#if defined(DOXYGEN) || (defined(__cpp_lib_span) && __cpp_lib_span >= 202002L)
  /// \brief Create a slice that refers to the contents of the given `std::span`.
  /// \param span  The source span of a standard-layout type.
  template <typename POD>
  MDBX_CXX14_CONSTEXPR slice(const ::std::span<POD> &span) : slice(span.data(), span.size_bytes()) {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
  }

  /// \brief Returns a read-only `std::span` view of the referenced data.
  /// \returns The `std::span` of the slice content.
  /// \throws mdbx::bad_value_size if the content size is not a multiple of `sizeof(POD)`.
  template <typename POD> MDBX_CXX14_CONSTEXPR ::std::span<const POD> as_span() const {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() % sizeof(POD) == 0))
      MDBX_CXX20_LIKELY
    return ::std::span<const POD>(static_cast<const POD *>(data()), size() / sizeof(POD));
    throw_bad_value_size();
  }

  /// \brief Returns a mutable `std::span` view of the referenced data.
  /// \returns The `std::span` of the slice content.
  /// \throws mdbx::bad_value_size if the content size is not a multiple of `sizeof(POD)`.
  template <typename POD> MDBX_CXX14_CONSTEXPR ::std::span<POD> as_span() {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() % sizeof(POD) == 0))
      MDBX_CXX20_LIKELY
    return ::std::span<POD>(static_cast<POD *>(data()), size() / sizeof(POD));
    throw_bad_value_size();
  }

  /// \brief Returns a read-only `std::span` of bytes of the referenced data.
  /// \returns The `std::span<const byte>` view.
  MDBX_CXX14_CONSTEXPR ::std::span<const byte> bytes() const { return as_span<const byte>(); }
  /// \brief Returns a mutable `std::span` of bytes of the referenced data.
  /// \returns The `std::span<byte>` view.
  MDBX_CXX14_CONSTEXPR ::std::span<byte> bytes() { return as_span<byte>(); }
  /// \brief Returns a read-only `std::span` of chars of the referenced data.
  /// \returns The `std::span<const char>` view.
  MDBX_CXX14_CONSTEXPR ::std::span<const char> chars() const { return as_span<const char>(); }
  /// \brief Returns a mutable `std::span` of chars of the referenced data.
  /// \returns The `std::span<char>` view.
  MDBX_CXX14_CONSTEXPR ::std::span<char> chars() { return as_span<char>(); }
#endif /* __cpp_lib_span >= 202002L */

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  /// \brief Create a slice that refers to the same contents as "string_view"
  /// \param sv  The source string view.
  template <class CHAR, class T>
  MDBX_CXX14_CONSTEXPR slice(const ::std::basic_string_view<CHAR, T> &sv)
      : slice(sv.data(), sv.size() * sizeof(CHAR)) {}

  /// \brief Takes over a temporary string view: the slice refers to its data
  /// and the moved-from view is emptied.
  /// \param sv  The source string view to be moved from.
  template <class CHAR, class T> slice(::std::basic_string_view<CHAR, T> &&sv) : slice(sv) { sv = {}; }
#endif /* __cpp_lib_string_view >= 201606L */

  /// \brief Wraps a null-terminated string literal into a slice.
  /// \param text  The string literal to be referenced.
  /// \returns The slice referring to the literal without the terminating null.
  template <size_t SIZE> static MDBX_CXX14_CONSTEXPR slice wrap(const char (&text)[SIZE]) { return slice(text); }

  /// \brief Wraps an object of a standard-layout type into a slice.
  /// \param pod  The object to be referenced.
  /// \returns The slice referring to the object's bytes.
  template <typename POD> MDBX_CXX14_CONSTEXPR static slice wrap(const POD &pod) {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    return slice(&pod, sizeof(pod));
  }

  /// \brief Assigns the slice to reference [0,bytes-1] of memory pointed by ptr.
  /// \param ptr    The address of the data to be referenced.
  /// \param bytes  The number of bytes to be referenced.
  /// \returns A reference to this instance.
  inline slice &assign(const void *ptr, size_t bytes);
  /// \brief Assigns the slice to reference the same data as the source slice.
  /// \param src  The source slice.
  /// \returns A reference to this instance.
  inline slice &assign(const slice &src) noexcept;
  /// \brief Assigns the slice to reference the contents of the given \ref MDBX_val.
  /// \param src  The source \ref MDBX_val instance.
  /// \returns A reference to this instance.
  inline slice &assign(const ::MDBX_val &src);
  /// \brief Moves the source slice into this one, invalidating the source.
  /// \param src  The source slice to be moved from.
  /// \returns A reference to this instance.
  inline slice &assign(slice &&src) noexcept;
  /// \brief Takes over a temporary \ref MDBX_val instance.
  /// \param src  The source \ref MDBX_val to be moved from.
  /// \returns A reference to this instance.
  inline slice &assign(::MDBX_val &&src);
  /// \brief Assigns the slice to reference the [begin,end) memory range.
  /// \param begin  The address of the first byte to be referenced.
  /// \param end    The address just after the last byte to be referenced.
  /// \returns A reference to this instance.
  inline slice &assign(const void *begin, const void *end);
  /// \brief Assigns the slice to reference the contents of the given string.
  /// \param str  The source string.
  /// \returns A reference to this instance.
  template <class CHAR, class T, class ALLOCATOR> slice &assign(const ::std::basic_string<CHAR, T, ALLOCATOR> &str) {
    return assign(str.data(), str.size() * sizeof(CHAR));
  }
  /// \brief Assigns the slice to reference a null-terminated string.
  /// \param c_str  The source null-terminated string.
  /// \returns A reference to this instance.
  inline slice &assign(const char *c_str);
#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  /// \brief Assigns the slice to reference the contents of the given string view.
  /// \param view  The source string view.
  /// \returns A reference to this instance.
  template <class CHAR, class T> slice &assign(const ::std::basic_string_view<CHAR, T> &view) {
    return assign(view.data(), view.size() * sizeof(CHAR));
  }
  /// \brief Takes over a temporary string view and assigns the slice to its contents.
  /// \param view  The source string view to be moved from.
  /// \returns A reference to this instance.
  template <class CHAR, class T> slice &assign(::std::basic_string_view<CHAR, T> &&view) {
    assign(view);
    view = {};
    return *this;
  }
#endif /* __cpp_lib_string_view >= 201606L */

  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  slice &operator=(const slice &) noexcept = default;
  /// \brief Move assignment: the source slice is invalidated.
  /// \param src  The source slice to be moved from.
  /// \returns A reference to this instance.
  inline slice &operator=(slice &&src) noexcept;
  /// \brief Takes over a temporary \ref MDBX_val instance.
  /// \param src  The source \ref MDBX_val to be moved from.
  /// \returns A reference to this instance.
  inline slice &operator=(::MDBX_val &&src);
  /// \brief Returns the pointer to the underlying \ref MDBX_val instance.
  /// \returns The mutable pointer to this instance as \ref MDBX_val.
  operator MDBX_val *() noexcept { return this; }
  /// \brief Returns the pointer to the underlying \ref MDBX_val instance.
  /// \returns The constant pointer to this instance as \ref MDBX_val.
  operator const MDBX_val *() const noexcept { return this; }

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  /// \brief Assigns the slice to reference the contents of the given string view.
  /// \param view  The source string view.
  /// \returns A reference to this instance.
  template <class CHAR, class T> slice &operator=(const ::std::basic_string_view<CHAR, T> &view) {
    return assign(view);
  }

  /// \brief Takes over a temporary string view and assigns the slice to its contents.
  /// \param view  The source string view to be moved from.
  /// \returns A reference to this instance.
  template <class CHAR, class T> slice &operator=(::std::basic_string_view<CHAR, T> &&view) { return assign(view); }

  /// \brief Return a string_view that references the same data as this slice.
  /// \returns The `std::basic_string_view` of the slice content.
  template <class CHAR = char, class T = ::std::char_traits<CHAR>>
  MDBX_CXX11_CONSTEXPR ::std::basic_string_view<CHAR, T> string_view() const noexcept {
    static_assert(sizeof(CHAR) == 1, "Must be single byte characters");
    return ::std::basic_string_view<CHAR, T>(char_ptr(), length());
  }

  /// \brief Return a string_view that references the same data as this slice.
  /// \returns The `std::basic_string_view` of the slice content.
  template <class CHAR, class T>
  MDBX_CXX11_CONSTEXPR explicit operator ::std::basic_string_view<CHAR, T>() const noexcept {
    return this->string_view<CHAR, T>();
  }
#endif /* __cpp_lib_string_view >= 201606L */

  /// \brief Copies the slice content into a `std::basic_string`.
  /// \param alloc  The allocator for the resulting string.
  /// \returns The string with a copy of the slice content.
  template <class CHAR = char, class T = ::std::char_traits<CHAR>, class ALLOCATOR = default_allocator>
  MDBX_CXX20_CONSTEXPR ::std::basic_string<CHAR, T, ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const {
    static_assert(sizeof(CHAR) == 1, "Must be single byte characters");
    return ::std::basic_string<CHAR, T, ALLOCATOR>(char_ptr(), length(), alloc);
  }

  /// \brief Copies the slice content into a `std::basic_string`.
  /// \returns The string with a copy of the slice content.
  template <class CHAR, class T, class ALLOCATOR>
  MDBX_CXX20_CONSTEXPR explicit operator ::std::basic_string<CHAR, T, ALLOCATOR>() const {
    return as_string<CHAR, T, ALLOCATOR>();
  }

  /// \brief Returns a string with a hexadecimal dump of the slice content.
  /// \param uppercase   Use uppercase letters for hexadecimal digits.
  /// \param wrap_width  The width to wrap lines with a newline, zero means no wrapping.
  /// \param alloc       The allocator for the resulting string.
  /// \returns The string with the hexadecimal dump.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_hex_string(bool uppercase = false, unsigned wrap_width = 0,
                                         const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a string with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the slice content.
  /// \param wrap_width  The width to wrap lines with a newline, zero means no wrapping.
  /// \param alloc       The allocator for the resulting string.
  /// \returns The string with the Base58 dump.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_base58_string(unsigned wrap_width = 0, const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a string with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of the slice content.
  /// \param wrap_width  The width to wrap lines with a newline, zero means no wrapping.
  /// \param alloc       The allocator for the resulting string.
  /// \returns The string with the Base64 dump.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_base64_string(unsigned wrap_width = 0, const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a hexadecimal dump of the slice content.
  /// \param uppercase   Use uppercase letters for hexadecimal digits.
  /// \param wrap_width  The width to wrap lines with a newline, zero means no wrapping.
  /// \param alloc       The allocator for the resulting buffer.
  /// \returns The buffer with the hexadecimal dump.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> encode_hex(bool uppercase = false, unsigned wrap_width = 0,
                                                       const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the slice content.
  /// \param wrap_width  The width to wrap lines with a newline, zero means no wrapping.
  /// \param alloc       The allocator for the resulting buffer.
  /// \returns The buffer with the Base58 dump.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> encode_base58(unsigned wrap_width = 0,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of the slice content.
  /// \param wrap_width  The width to wrap lines with a newline, zero means no wrapping.
  /// \param alloc       The allocator for the resulting buffer.
  /// \returns The buffer with the Base64 dump.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> encode_base64(unsigned wrap_width = 0,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes hexadecimal dump from the slice content to returned buffer.
  /// \param ignore_spaces  Skip whitespaces while decoding.
  /// \param alloc          The allocator for the resulting buffer.
  /// \returns The buffer with the decoded data.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> hex_decode(bool ignore_spaces = false,
                                                       const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes [Base58](https://en.wikipedia.org/wiki/Base58) dump
  /// from the slice content to returned buffer.
  /// \param ignore_spaces  Skip whitespaces while decoding.
  /// \param alloc          The allocator for the resulting buffer.
  /// \returns The buffer with the decoded data.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> base58_decode(bool ignore_spaces = false,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes [Base64](https://en.wikipedia.org/wiki/Base64) dump
  /// from the slice content to returned buffer.
  /// \param ignore_spaces  Skip whitespaces while decoding.
  /// \param alloc          The allocator for the resulting buffer.
  /// \returns The buffer with the decoded data.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> base64_decode(bool ignore_spaces = false,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Checks whether the content of the slice is printable.
  /// \param [in] disable_utf8 By default if `disable_utf8` is `false` function
  /// checks that content bytes are printable ASCII-7 characters or a valid UTF8
  /// sequences. Otherwise, if `disable_utf8` is `true` function checks that
  /// content bytes are printable extended 8-bit ASCII codes.
  /// \returns `true` if the content is printable.
  MDBX_NOTHROW_PURE_FUNCTION bool is_printable(bool disable_utf8 = false) const noexcept;

  /// \brief Checks whether the content of the slice is a hexadecimal dump.
  /// \param [in] ignore_spaces If `true` function will skips spaces surrounding
  /// (before, between and after) a encoded bytes. However, spaces should not
  /// break a pair of characters encoding a single byte.
  /// \returns `true` if the content is a valid hexadecimal dump.
  MDBX_NOTHROW_PURE_FUNCTION inline bool is_hex(bool ignore_spaces = false) const noexcept;

  /// \brief Checks whether the content of the slice is a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump.
  /// \param [in] ignore_spaces If `true` function will skips spaces surrounding
  /// (before, between and after) a encoded bytes. However, spaces should not
  /// break a code group of characters.
  /// \returns `true` if the content is a valid Base58 dump.
  MDBX_NOTHROW_PURE_FUNCTION inline bool is_base58(bool ignore_spaces = false) const noexcept;

  /// \brief Checks whether the content of the slice is a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump.
  /// \param [in] ignore_spaces If `true` function will skips spaces surrounding
  /// (before, between and after) a encoded bytes. However, spaces should not
  /// break a code group of characters.
  /// \returns `true` if the content is a valid Base64 dump.
  MDBX_NOTHROW_PURE_FUNCTION inline bool is_base64(bool ignore_spaces = false) const noexcept;

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  /// \brief Swaps the slice content with the given string view.
  /// \param view  The string view to swap with.
  template <class CHAR, class T> void swap(::std::basic_string_view<CHAR, T> &view) noexcept {
    static_assert(sizeof(CHAR) == 1, "Must be single byte characters");
    const auto temp = ::std::basic_string_view<CHAR, T>(*this);
    *this = view;
    view = temp;
  }
#endif /* __cpp_lib_string_view >= 201606L */

  /// \brief Swaps this slice with another one.
  /// \param other  The slice to swap with.
  MDBX_CXX20_CONSTEXPR void swap(slice &other) noexcept {
    auto temp = other;
    other = *this;
    *this = temp;
  }

  /// \brief Returns casted to pointer to byte an address of data.
  /// \returns The read-only pointer to the first byte.
  MDBX_CXX11_CONSTEXPR const byte *byte_ptr() const noexcept;
  /// \brief Returns casted to pointer to byte an address of data.
  /// \returns The mutable pointer to the first byte.
  MDBX_CXX11_CONSTEXPR byte *byte_ptr() noexcept;

  /// \brief Returns casted to pointer to byte an end of data.
  /// \returns The read-only pointer just after the last byte.
  MDBX_CXX11_CONSTEXPR const byte *end_byte_ptr() const noexcept;
  /// \brief Returns casted to pointer to byte an end of data.
  /// \returns The mutable pointer just after the last byte.
  MDBX_CXX11_CONSTEXPR byte *end_byte_ptr() noexcept;

  /// \brief Returns casted to pointer to char an address of data.
  /// \returns The read-only pointer to the first byte.
  MDBX_CXX11_CONSTEXPR const char *char_ptr() const noexcept;
  /// \brief Returns casted to pointer to char an address of data.
  /// \returns The mutable pointer to the first byte.
  MDBX_CXX11_CONSTEXPR char *char_ptr() noexcept;

  /// \brief Returns casted to pointer to char an end of data.
  /// \returns The read-only pointer just after the last byte.
  MDBX_CXX11_CONSTEXPR const char *end_char_ptr() const noexcept;
  /// \brief Returns casted to pointer to char an end of data.
  /// \returns The mutable pointer just after the last byte.
  MDBX_CXX11_CONSTEXPR char *end_char_ptr() noexcept;

  /// \brief Return a pointer to the beginning of the referenced data.
  /// \returns The read-only pointer to the data.
  MDBX_CXX11_CONSTEXPR const void *data() const noexcept;
  /// \brief Return a pointer to the beginning of the referenced data.
  /// \returns The mutable pointer to the data.
  MDBX_CXX11_CONSTEXPR void *data() noexcept;

  /// \brief Return a pointer to the end of the referenced data.
  /// \details An iterator-style pointer (one past the last byte).
  /// \returns The read-only pointer just after the last byte.
  MDBX_CXX11_CONSTEXPR const byte *end() const noexcept;
  /// \copydoc end() const
  MDBX_CXX11_CONSTEXPR byte *end() noexcept;

  /// \brief Returns the number of bytes.
  /// \returns The length of the referenced data in bytes.
  MDBX_CXX11_CONSTEXPR size_t length() const noexcept;

  /// \brief Set slice length.
  /// \param bytes  The new length of the slice.
  /// \returns A reference to this instance.
  MDBX_CXX14_CONSTEXPR slice &set_length(size_t bytes);

  /// \brief Sets the length by specifying the end of the slice data.
  /// \param ptr  The address just after the last byte of the slice.
  /// \returns A reference to this instance.
  MDBX_CXX14_CONSTEXPR slice &set_end(const void *ptr);

  /// \brief Checks whether the slice is empty.
  /// \returns `true` if the slice length is zero.
  MDBX_CXX11_CONSTEXPR bool empty() const noexcept;

  /// \brief Checks whether the slice data pointer is nullptr.
  /// \returns `true` if the slice refers to nothing.
  MDBX_CXX11_CONSTEXPR bool is_null() const noexcept;

  /// \brief Returns the number of bytes.
  /// \returns The length of the referenced data in bytes.
  MDBX_CXX11_CONSTEXPR size_t size() const noexcept;

  /// \brief Returns true if slice is not empty.
  /// \returns `true` if the slice is not null.
  MDBX_CXX11_CONSTEXPR operator bool() const noexcept;
  /// \brief Same as the const version; keeps `if (s)` away from `operator MDBX_val *()`.
  /// \returns `true` if the slice is not null.
  MDBX_CXX14_CONSTEXPR operator bool() noexcept { return !is_null(); }

  /// \brief Invalidates the slice by nullifying the data pointer while
  /// preserving the length; makes \ref is_valid() return `false` unless the
  /// slice was already of zero length.
  MDBX_CXX14_CONSTEXPR void invalidate() noexcept;

  /// \brief Makes the slice empty and referencing to nothing.
  MDBX_CXX14_CONSTEXPR void clear() noexcept;

  /// \brief Drops the first "n" bytes from this slice.
  /// \param n  The number of bytes to drop.
  /// \pre REQUIRES: `n <= size()`
  inline void remove_prefix(size_t n) noexcept;

  /// \brief Drops the last "n" bytes from this slice.
  /// \param n  The number of bytes to drop.
  /// \pre REQUIRES: `n <= size()`
  inline void remove_suffix(size_t n) noexcept;

  /// \brief Drops the first "n" bytes from this slice.
  /// \param n  The number of bytes to drop.
  /// \throws std::out_of_range if `n > size()`
  inline void safe_remove_prefix(size_t n);

  /// \brief Drops the last "n" bytes from this slice.
  /// \param n  The number of bytes to drop.
  /// \throws std::out_of_range if `n > size()`
  inline void safe_remove_suffix(size_t n);

  /// \brief Checks if the data starts with the given prefix.
  /// \param prefix  The expected prefix.
  /// \returns `true` if the slice starts with the given prefix.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool starts_with(const slice &prefix) const noexcept;

  /// \brief Checks if the data ends with the given suffix.
  /// \param suffix  The expected suffix.
  /// \returns `true` if the slice ends with the given suffix.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool ends_with(const slice &suffix) const noexcept;

  /// \brief Returns the nth byte in the referenced data.
  /// \param n  The index of the byte.
  /// \returns The nth byte.
  /// \pre REQUIRES: `n < size()`
  MDBX_CXX11_CONSTEXPR byte operator[](size_t n) const noexcept;

  /// \brief Returns the nth byte in the referenced data with bounds checking.
  /// \param n  The index of the byte.
  /// \returns The nth byte.
  /// \throws std::out_of_range if `n >= size()`
  MDBX_CXX11_CONSTEXPR byte at(size_t n) const;

  /// \brief Returns the first "n" bytes of the slice.
  /// \param n  The number of bytes.
  /// \returns The sub-slice of the first `n` bytes.
  /// \pre REQUIRES: `n <= size()`
  MDBX_CXX14_CONSTEXPR slice head(size_t n) const noexcept;

  /// \brief Returns the last "n" bytes of the slice.
  /// \param n  The number of bytes.
  /// \returns The sub-slice of the last `n` bytes.
  /// \pre REQUIRES: `n <= size()`
  MDBX_CXX14_CONSTEXPR slice tail(size_t n) const noexcept;

  /// \brief Returns the middle "n" bytes of the slice.
  /// \param from  The offset of the first byte.
  /// \param n     The number of bytes.
  /// \returns The sub-slice of `n` bytes starting at `from`.
  /// \pre REQUIRES: `from + n <= size()`
  MDBX_CXX14_CONSTEXPR slice middle(size_t from, size_t n) const noexcept;

  /// \brief Returns an iterator to the beginning of the referenced data.
  /// \returns The read-only pointer to the first byte.
  MDBX_CXX11_CONSTEXPR const byte *begin() const noexcept;
  /// \copydoc begin() const
  MDBX_CXX11_CONSTEXPR byte *begin() noexcept;

  /// \brief Returns the first byte of the referenced data.
  /// \returns The first byte.
  /// \pre REQUIRES: `!empty()`
  MDBX_CXX11_CONSTEXPR byte front() const noexcept { return byte_ptr()[0]; }

  /// \brief Returns the last byte of the referenced data.
  /// \returns The last byte.
  /// \pre REQUIRES: `!empty()`
  MDBX_CXX11_CONSTEXPR byte back() const noexcept { return byte_ptr()[size() - 1]; }

  /// \brief Returns a sub-slice of [pos, pos+count) clamped to the end.
  /// \param pos    The offset of the first byte of the sub-slice.
  /// \param count  The number of bytes, `npos` means "up to the end".
  /// \returns The sub-slice.
  /// \throws std::out_of_range if `pos > size()`.
  MDBX_CXX14_CONSTEXPR slice substr(size_t pos, size_t count = npos) const {
    if (MDBX_UNLIKELY(pos > size()))
      MDBX_CXX20_UNLIKELY throw_out_range();
    return middle(pos, (::std::min)(count, size() - pos));
  }

  /// \brief Finds the first occurrence of `needle` starting at `pos`.
  /// \param needle  The sub-slice to search for.
  /// \param pos     The position to start the search from.
  /// \returns The position of the first match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find(const slice &needle, size_t pos = 0) const noexcept;
  /// \brief Finds the first occurrence of the byte `c` starting at `pos`.
  /// \param c    The byte to search for.
  /// \param pos  The position to start the search from.
  /// \returns The position of the first match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find(byte c, size_t pos = 0) const noexcept;

  /// \brief Finds the last occurrence of `needle` not after `pos`.
  /// \param needle  The sub-slice to search for.
  /// \param pos     The position to search backwards from, `npos` means the end.
  /// \returns The position of the last match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t rfind(const slice &needle, size_t pos = npos) const noexcept;
  /// \brief Finds the last occurrence of the byte `c` not after `pos`.
  /// \param c    The byte to search for.
  /// \param pos  The position to search backwards from, `npos` means the end.
  /// \returns The position of the last match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t rfind(byte c, size_t pos = npos) const noexcept;

  /// \brief Finds the first byte matching any byte of `chars` starting at `pos`.
  /// \param chars  The set of bytes to search for.
  /// \param pos    The position to start the search from.
  /// \returns The position of the first match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_first_of(const slice &chars,
                                                                       size_t pos = 0) const noexcept;
  /// \brief Finds the first occurrence of the byte `c` starting at `pos`.
  /// \param c    The byte to search for.
  /// \param pos  The position to start the search from.
  /// \returns The position of the first match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_first_of(byte c, size_t pos = 0) const noexcept;

  /// \brief Finds the last byte matching any byte of `chars` not after `pos`.
  /// \param chars  The set of bytes to search for.
  /// \param pos    The position to search backwards from, `npos` means the end.
  /// \returns The position of the last match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_last_of(const slice &chars,
                                                                      size_t pos = npos) const noexcept;
  /// \brief Finds the last occurrence of the byte `c` not after `pos`.
  /// \param c    The byte to search for.
  /// \param pos  The position to search backwards from, `npos` means the end.
  /// \returns The position of the last match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_last_of(byte c, size_t pos = npos) const noexcept;

  /// \brief Finds the first byte not matching any byte of `chars` starting at `pos`.
  /// \param chars  The set of bytes to avoid.
  /// \param pos    The position to start the search from.
  /// \returns The position of the first match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_first_not_of(const slice &chars,
                                                                           size_t pos = 0) const noexcept;
  /// \brief Finds the first byte not equal to `c` starting at `pos`.
  /// \param c    The byte to avoid.
  /// \param pos  The position to start the search from.
  /// \returns The position of the first match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_first_not_of(byte c, size_t pos = 0) const noexcept;

  /// \brief Finds the last byte not matching any byte of `chars` not after `pos`.
  /// \param chars  The set of bytes to avoid.
  /// \param pos    The position to search backwards from, `npos` means the end.
  /// \returns The position of the last match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_last_not_of(const slice &chars,
                                                                          size_t pos = npos) const noexcept;
  /// \brief Finds the last byte not equal to `c` not after `pos`.
  /// \param c    The byte to avoid.
  /// \param pos  The position to search backwards from, `npos` means the end.
  /// \returns The position of the last match or `npos`.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t find_last_not_of(byte c, size_t pos = npos) const noexcept;

  /// \brief Checks whether the referenced data contains `needle` as a sub-slice.
  /// \param needle  The sub-slice to search for.
  /// \returns `true` if the slice contains the given sub-slice.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool contains(const slice &needle) const noexcept {
    return find(needle) != npos;
  }

  /// \brief Lexicographical three-way comparison with another slice.
  /// \param other  The slice to compare with.
  /// \returns `0` if equal, `<0` if this slice is less, `>0` if greater.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR intptr_t compare(const slice &other) const noexcept {
    return compare_lexicographically(*this, other);
  }

  /// \brief Returns the first "n" bytes of the slice.
  /// \param n  The number of bytes.
  /// \returns The sub-slice of the first `n` bytes.
  /// \throws std::out_of_range if `n > size()`
  MDBX_CXX14_CONSTEXPR slice safe_head(size_t n) const;

  /// \brief Returns the last "n" bytes of the slice.
  /// \param n  The number of bytes.
  /// \returns The sub-slice of the last `n` bytes.
  /// \throws std::out_of_range if `n > size()`
  MDBX_CXX14_CONSTEXPR slice safe_tail(size_t n) const;

  /// \brief Returns the middle "n" bytes of the slice.
  /// \param from  The offset of the first byte.
  /// \param n     The number of bytes.
  /// \returns The sub-slice of `n` bytes starting at `from`.
  /// \throws std::out_of_range if `from + n > size()`
  MDBX_CXX14_CONSTEXPR slice safe_middle(size_t from, size_t n) const;

  /// \brief Returns the hash value of referenced data.
  /// \attention Function implementation and returned hash values may change
  /// from version to version, and in future the t1ha3 will be used here.
  /// Therefore values obtained from this function shouldn't be persisted
  /// anywhere.
  /// \returns The hash value of the referenced data.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t hash_value() const noexcept;

  /// \brief Three-way fast non-lexicographically length-based comparison.
  /// \details Firstly compares length and if it equal then compare content
  /// lexicographically. \return value:
  ///  `== 0` if `a` the same as `b`;
  ///   `< 0` if `a` shorter than `b`,
  ///             or the same length and lexicographically less than `b`;
  ///   `> 0` if `a` longer than `b`,
  ///             or the same length and lexicographically greater than `b`.
  /// \param a  The left-hand slice.
  /// \param b  The right-hand slice.
  /// \returns The three-way comparison result.
  MDBX_NOTHROW_PURE_FUNCTION static MDBX_CXX14_CONSTEXPR intptr_t compare_fast(const slice &a, const slice &b) noexcept;

  /// \brief Three-way lexicographically comparison.
  /// \return value:
  ///  `== 0` if `a` lexicographically equal `b`;
  ///   `< 0` if `a` lexicographically less than `b`;
  ///   `> 0` if `a` lexicographically greater than `b`.
  /// \param a  The left-hand slice.
  /// \param b  The right-hand slice.
  /// \returns The three-way comparison result.
  MDBX_NOTHROW_PURE_FUNCTION static MDBX_CXX14_CONSTEXPR intptr_t compare_lexicographically(const slice &a,
                                                                                            const slice &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator==(const slice &a, const slice &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator<(const slice &a, const slice &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator>(const slice &a, const slice &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator<=(const slice &a, const slice &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator>=(const slice &a, const slice &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator!=(const slice &a, const slice &b) noexcept;
#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
  friend MDBX_CXX14_CONSTEXPR auto operator<=>(const slice &a, const slice &b) noexcept;
#endif /* __cpp_impl_three_way_comparison */

  /// \brief Checks the slice is valid: returns `false` only if it has a null
  /// data pointer and a non-zero length.
  /// \returns `true` if the slice is valid.
  MDBX_CXX11_CONSTEXPR bool is_valid() const noexcept { return !(iov_base == nullptr && iov_len != 0); }

  /// \brief Builds an invalid slice that has a non-zero length and refers to
  /// a null address.
  /// \returns The invalid slice instance.
  MDBX_CXX14_CONSTEXPR static slice invalid() noexcept {
    return slice(/* using special constructor without length checking */ ~size_t(0));
  }

  /// \brief Build a null slice which zero length and refers to null address.
  /// \returns The null slice instance.
  MDBX_CXX14_CONSTEXPR static slice null() noexcept { return slice(nullptr, size_t(0)); }

  /// \brief Builds a "blank" slice for a \ref MDBX_RESERVE allocation: it has
  /// a null data pointer but the requested non-zero length, which the C API
  /// fills in on return from `txn::insert_reserve()`,
  /// `txn::upsert_reserve()` and friends.
  /// \details Since a null pointer with a non-zero length is rejected by the
  /// ordinary \ref slice() constructor and `assign()` (issue #142), this
  /// explicit factory is the only way to spell the reserve form; it builds the
  /// blank via the special private constructor (like \ref invalid()) but still
  /// validates `length` against \ref MDBX_MAXDATASIZE, so an oversize request
  /// throws `std::length_error` exactly as the old `slice(nullptr, N)`
  /// spelling did.
  /// \param length  The requested length of the blank slice.
  /// \returns The blank slice for a reserve allocation.
  /// \throws std::length_error if `length` exceeds the maximum value size.
  MDBX_CXX14_CONSTEXPR static slice blank_for_put_reserve(size_t length) {
    return slice(/* validates length, then uses the unchecked special ctor */ check_length(length));
  }

  /// \brief Reinterprets the slice content as an object of a standard-layout type.
  /// \returns A copy of the object of type `POD` read from the slice bytes.
  /// \throws mdbx::bad_value_size if the slice length does not exactly match `sizeof(POD)`.
  template <typename POD> MDBX_CXX14_CONSTEXPR POD as_pod() const {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() == sizeof(POD)))
      MDBX_CXX20_LIKELY {
        POD _as_pod_local;
        memcpy(&_as_pod_local, data(), sizeof(_as_pod_local));
        return _as_pod_local;
      }
    throw_bad_value_size();
  }

#if defined(DOXYGEN) || (defined(__cpp_lib_optional) && __cpp_lib_optional >= 201606L)
  /// \brief Non-throwing counterpart of `as_pod()`: returns `std::nullopt`
  /// when the slice length does not exactly match `sizeof(POD)`.
  /// \note The returned object is a copy of the referenced bytes, so the slice
  /// need not outlive the result (issue #144).
  /// \returns The object of type `POD` or `std::nullopt` on a size mismatch.
  template <typename POD> MDBX_CXX14_CONSTEXPR ::std::optional<POD> try_as_pod() const {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() == sizeof(POD))) {
      POD _as_pod_local;
      memcpy(&_as_pod_local, data(), sizeof(_as_pod_local));
      return _as_pod_local;
    }
    return ::std::nullopt;
  }
#endif /* __cpp_lib_optional >= 201606L */

#ifdef MDBX_U128_TYPE
  /// \brief Reinterprets the slice as a 128-bit unsigned integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR MDBX_U128_TYPE as_uint128() const { return as_pod<MDBX_U128_TYPE>(); }
#endif /* MDBX_U128_TYPE */
  /// \brief Reinterprets the slice as a 64-bit unsigned integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR uint64_t as_uint64() const { return as_pod<uint64_t>(); }
  /// \brief Reinterprets the slice as a 32-bit unsigned integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR uint32_t as_uint32() const { return as_pod<uint32_t>(); }
  /// \brief Reinterprets the slice as a 16-bit unsigned integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR uint16_t as_uint16() const { return as_pod<uint16_t>(); }
  /// \brief Reinterprets the slice as an 8-bit unsigned integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR uint8_t as_uint8() const { return as_pod<uint8_t>(); }

#ifdef MDBX_I128_TYPE
  /// \brief Reinterprets the slice as a 128-bit signed integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR MDBX_I128_TYPE as_int128() const { return as_pod<MDBX_I128_TYPE>(); }
#endif /* MDBX_I128_TYPE */
  /// \brief Reinterprets the slice as a 64-bit signed integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR int64_t as_int64() const { return as_pod<int64_t>(); }
  /// \brief Reinterprets the slice as a 32-bit signed integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR int32_t as_int32() const { return as_pod<int32_t>(); }
  /// \brief Reinterprets the slice as a 16-bit signed integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR int16_t as_int16() const { return as_pod<int16_t>(); }
  /// \brief Reinterprets the slice as an 8-bit signed integer.
  /// \returns The value; throws mdbx::bad_value_size on a size mismatch.
  MDBX_CXX14_CONSTEXPR int8_t as_int8() const { return as_pod<int8_t>(); }

#ifdef MDBX_U128_TYPE
  /// \brief Reinterprets the slice as a 128-bit unsigned integer, adapting
  /// the value when the slice is shorter than 16 bytes.
  /// \returns The value adapted to the actual slice length.
  MDBX_U128_TYPE as_uint128_adapt() const;
#endif /* MDBX_U128_TYPE */
  /// \brief Reinterprets the slice as a 64-bit unsigned integer, adapting
  /// the value when the slice is shorter than 8 bytes.
  /// \returns The value adapted to the actual slice length.
  uint64_t as_uint64_adapt() const;
  /// \brief Reinterprets the slice as a 32-bit unsigned integer, adapting
  /// the value when the slice is shorter than 4 bytes.
  /// \returns The value adapted to the actual slice length.
  uint32_t as_uint32_adapt() const;
  /// \brief Reinterprets the slice as a 16-bit unsigned integer, adapting
  /// the value when the slice is shorter than 2 bytes.
  /// \returns The value adapted to the actual slice length.
  uint16_t as_uint16_adapt() const;
  /// \brief Reinterprets the slice as an 8-bit unsigned integer.
  /// \returns The value adapted to the actual slice length.
  uint8_t as_uint8_adapt() const;

#ifdef MDBX_I128_TYPE
  /// \brief Reinterprets the slice as a 128-bit signed integer, adapting
  /// the value when the slice is shorter than 16 bytes.
  /// \returns The value adapted to the actual slice length.
  MDBX_I128_TYPE as_int128_adapt() const;
#endif /* MDBX_I128_TYPE */
  /// \brief Reinterprets the slice as a 64-bit signed integer, adapting
  /// the value when the slice is shorter than 8 bytes.
  /// \returns The value adapted to the actual slice length.
  int64_t as_int64_adapt() const;
  /// \brief Reinterprets the slice as a 32-bit signed integer, adapting
  /// the value when the slice is shorter than 4 bytes.
  /// \returns The value adapted to the actual slice length.
  int32_t as_int32_adapt() const;
  /// \brief Reinterprets the slice as a 16-bit signed integer, adapting
  /// the value when the slice is shorter than 2 bytes.
  /// \returns The value adapted to the actual slice length.
  int16_t as_int16_adapt() const;
  /// \brief Reinterprets the slice as an 8-bit signed integer.
  /// \returns The value adapted to the actual slice length.
  int8_t as_int8_adapt() const;

protected:
  /// \brief A special constructor without length checking, for \ref invalid()
  /// and \ref blank_for_put_reserve().
  /// \param invalid_length  The length to be set with a null data pointer.
  MDBX_CXX11_CONSTEXPR slice(size_t invalid_length) noexcept : ::MDBX_val({nullptr, invalid_length}) {}
};

/// \brief Combines data slice with boolean flag to represent result of certain operations.
struct value_result {
  slice value; ///< \brief The resulting data slice.
  bool done;   ///< \brief `true` when the operation has found/produced the value.
  /// \brief Constructs the result.
  /// \param value  The resulting data slice.
  /// \param done   `true` when the operation has found/produced the value.
  value_result(const slice &value, bool done) noexcept : value(value), done(done) {}
  /// \brief Copy constructor.
  value_result(const value_result &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  value_result &operator=(const value_result &) noexcept = default;
  /// \brief Checks whether the value was found/produced.
  /// \returns The \ref done flag.
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept {
    MDBX_INLINE_API_ASSERT(!done || value.is_valid());
    return done;
  }
};

/// \brief Combines pair of slices for key and value to represent result of certain operations.
struct pair {
  /// \brief An alias for `std::pair<slice, slice>`.
  using stl_pair = std::pair<slice, slice>;
  slice key;   ///< \brief The key part of the pair.
  slice value; ///< \brief The value part of the pair.
  /// \brief Constructs the pair from the given key and value slices.
  /// \param key    The key part.
  /// \param value  The value part.
  MDBX_CXX11_CONSTEXPR pair(const slice &key, const slice &value) noexcept : key(key), value(value) {}
  /// \brief Constructs the pair from a `std::pair` of slices.
  /// \param couple  The source `std::pair`.
  MDBX_CXX11_CONSTEXPR pair(const stl_pair &couple) noexcept : key(couple.first), value(couple.second) {}
  /// \brief Converts the pair to a `std::pair` of slices.
  /// \returns The `std::pair` with the same key and value.
  MDBX_CXX11_CONSTEXPR operator stl_pair() const noexcept { return stl_pair(key, value); }
  /// \brief Copy constructor.
  pair(const pair &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  pair &operator=(const pair &) noexcept = default;
  /// \brief Move assignment operator.
  /// \param couple  The source pair to be moved from.
  /// \returns A reference to this instance.
  pair &operator=(pair &&couple) noexcept {
    key.assign(std::move(couple.key));
    value.assign(std::move(couple.value));
    return *this;
  }
  /// \brief Checks whether the pair is not empty.
  /// \returns `true` if the key (and the value) slices are not null.
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept {
    MDBX_INLINE_API_ASSERT(bool(key) == bool(value));
    return key;
  }
  /// \brief Builds an invalid pair of invalid slices.
  /// \returns The invalid pair instance.
  MDBX_CXX14_CONSTEXPR static pair invalid() noexcept { return pair(slice::invalid(), slice::invalid()); }

  /// \brief Assigns the pair from a `std::pair` of slices.
  /// \param couple  The source `std::pair`.
  /// \returns A reference to this instance.
  pair &operator=(const stl_pair &couple) {
    key.assign(couple.first);
    value.assign(couple.second);
    return *this;
  }
  /// \brief Move-assigns the pair from a temporary `std::pair` of slices.
  /// \param couple  The source `std::pair` to be moved from.
  /// \returns A reference to this instance.
  pair &operator=(stl_pair &&couple) {
    key.assign(std::move(couple.first));
    value.assign(std::move(couple.second));
    return *this;
  }

  /// \brief Three-way fast non-lexicographically length-based comparison.
  /// \param a  The left-hand pair.
  /// \param b  The right-hand pair.
  /// \returns The three-way comparison result.
  MDBX_NOTHROW_PURE_FUNCTION static MDBX_CXX14_CONSTEXPR intptr_t compare_fast(const pair &a, const pair &b) noexcept;

  /// \brief Three-way lexicographically comparison.
  /// \param a  The left-hand pair.
  /// \param b  The right-hand pair.
  /// \returns The three-way comparison result.
  MDBX_NOTHROW_PURE_FUNCTION static MDBX_CXX14_CONSTEXPR intptr_t compare_lexicographically(const pair &a,
                                                                                            const pair &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator==(const pair &a, const pair &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator<(const pair &a, const pair &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator>(const pair &a, const pair &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator<=(const pair &a, const pair &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator>=(const pair &a, const pair &b) noexcept;
  friend MDBX_CXX14_CONSTEXPR bool operator!=(const pair &a, const pair &b) noexcept;
#if defined(__cpp_impl_three_way_comparison) && __cpp_impl_three_way_comparison >= 201907L
  friend MDBX_CXX14_CONSTEXPR auto operator<=>(const pair &a, const pair &b) noexcept;
#endif /* __cpp_impl_three_way_comparison */
};

/// \brief Combines pair of slices for key and value with boolean flag to
/// represent result of certain operations.
struct pair_result : public pair {
  bool done; ///< \brief `true` when the operation has found/produced the pair.
  /// \brief Constructs an empty (not done) result with an invalid pair.
  MDBX_CXX11_CONSTEXPR pair_result() noexcept : pair(pair::invalid()), done(false) {}
  /// \brief Constructs the result.
  /// \param key    The key part.
  /// \param value  The value part.
  /// \param done   `true` when the operation has found/produced the pair.
  MDBX_CXX11_CONSTEXPR pair_result(const slice &key, const slice &value, bool done) noexcept
      : pair(key, value), done(done) {}
  /// \brief Copy constructor.
  pair_result(const pair_result &) noexcept = default;
  /// \brief Copy assignment operator.
  /// \returns A reference to this instance.
  pair_result &operator=(const pair_result &) noexcept = default;
  /// \brief Checks whether the pair was found/produced.
  /// \returns The \ref done flag.
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept {
    MDBX_INLINE_API_ASSERT(!done || (bool(key) && bool(value)));
    return done;
  }
};

/// \brief Value-to-Key functions to avoid using custom comparators.
///
/// \details These build keys which are comparable without a custom comparator,
/// see \ref ::mdbx_key_from_double() and the `value2key` group. The returned
/// integers should be stored with \ref key_mode::ordinal.
/// \param json_integer  The value to be converted.
/// \returns The 8-byte key comparable with keys made from `double` values.
/// \see ::mdbx_key_from_jsonInteger()
inline uint64_t key_from_jsonInteger(int64_t json_integer) noexcept {
  return ::mdbx_key_from_jsonInteger(json_integer);
}
/// \param ieee754_64bit  The value to be converted.
/// \returns The 8-byte key that keeps the original value order.
/// \see ::mdbx_key_from_double()
inline uint64_t key_from_double(double ieee754_64bit) noexcept { return ::mdbx_key_from_double(ieee754_64bit); }
/// \param ieee754_64bit  The address of the value to be converted.
/// \returns The 8-byte key that keeps the original value order.
/// \see ::mdbx_key_from_ptrdouble()
inline uint64_t key_from_ptrdouble(const double *const ieee754_64bit) noexcept {
  return ::mdbx_key_from_ptrdouble(ieee754_64bit);
}
/// \param ieee754_32bit  The value to be converted.
/// \returns The 4-byte key that keeps the original value order.
/// \see ::mdbx_key_from_float()
inline uint32_t key_from_float(float ieee754_32bit) noexcept { return ::mdbx_key_from_float(ieee754_32bit); }
/// \param ieee754_32bit  The address of the value to be converted.
/// \returns The 4-byte key that keeps the original value order.
/// \see ::mdbx_key_from_ptrfloat()
inline uint32_t key_from_ptrfloat(const float *const ieee754_32bit) noexcept {
  return ::mdbx_key_from_ptrfloat(ieee754_32bit);
}
/// \param i64  The value to be converted.
/// \returns The 8-byte key that keeps the original value order.
/// \see ::mdbx_key_from_int64()
inline uint64_t key_from_int64(int64_t i64) noexcept { return ::mdbx_key_from_int64(i64); }
/// \param i32  The value to be converted.
/// \returns The 4-byte key that keeps the original value order.
/// \see ::mdbx_key_from_int32()
inline uint32_t key_from_int32(int32_t i32) noexcept { return ::mdbx_key_from_int32(i32); }

/// \brief Key-to-Value functions to avoid using custom comparators.
/// \param key  The key made by \ref key_from_double().
/// \returns The original `double` value.
/// \see ::mdbx_double_from_key()
inline double double_from_key(const slice &key) noexcept { return ::mdbx_double_from_key(key); }
/// \param key  The key made by \ref key_from_float().
/// \returns The original `float` value.
/// \see ::mdbx_float_from_key()
inline float float_from_key(const slice &key) noexcept { return ::mdbx_float_from_key(key); }
/// \param key  The key made by \ref key_from_int32().
/// \returns The original `int32_t` value.
/// \see ::mdbx_int32_from_key()
inline int32_t int32_from_key(const slice &key) noexcept { return ::mdbx_int32_from_key(key); }
/// \param key  The key made by \ref key_from_int64().
/// \returns The original `int64_t` value.
/// \see ::mdbx_int64_from_key()
inline int64_t int64_from_key(const slice &key) noexcept { return ::mdbx_int64_from_key(key); }
/// \param key  The key made by \ref key_from_jsonInteger().
/// \returns The original `int64_t` JSON-number value.
/// \see ::mdbx_jsonInteger_from_key()
inline int64_t jsonInteger_from_key(const slice &key) noexcept { return ::mdbx_jsonInteger_from_key(key); }

/// \brief Dumps the given value to a string: as-is when printable (all bytes
/// in the range 0x20..0x7E), otherwise as a hexadecimal dump.
/// \param value  The value to be dumped.
/// \returns `<empty>` for an empty value, `<nullptr.N>` for a non-empty value
/// with a null data pointer.
/// \see ::mdbx_dump_val()
inline ::std::string dump_val(const slice &value) {
  if (value.empty())
    return "<empty>";
  ::std::string result(value.length() * 2 + 4, '\0');
  const char *const ptr = ::mdbx_dump_val(&value, &result[0], result.size());
  if (!ptr)
    return result;
  result.assign(ptr);
  return result;
}

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
