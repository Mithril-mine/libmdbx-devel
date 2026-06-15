// > dist-cutoff-begin
#pragma once
#include "decl_exceptions.h++"
namespace mdbx {
// < dist-cutoff-end

/// \brief References a data located outside the slice.
///
/// The `slice` is similar in many ways to `std::string_view`, but it
/// implements specific capabilities and manipulates with bytes but
/// not a characters.
///
/// \copydetails MDBX_val
struct LIBMDBX_API_TYPE slice : public ::MDBX_val {
  /// \todo slice& operator<<(slice&, ...) for reading
  /// \todo key-to-value (parse/unpack) functions
  /// \todo template<class X> key(X); for decoding keys while reading

  enum : size_t { max_length = MDBX_MAXDATASIZE };

  /// \brief Create an empty slice.
  MDBX_CXX11_CONSTEXPR slice() noexcept;

  /// \brief Create a slice that refers to [0,bytes-1] of memory bytes pointed by ptr.
  MDBX_CXX14_CONSTEXPR slice(const void *ptr, size_t bytes);

  /// \brief Create a slice that refers to [begin,end] of memory bytes.
  MDBX_CXX14_CONSTEXPR slice(const void *begin, const void *end);

  /// \brief Create a slice that refers to text[0,strlen(text)-1].
  template <size_t SIZE> MDBX_CXX14_CONSTEXPR slice(const char (&text)[SIZE]) : slice(text, SIZE - 1) {
    MDBX_CONSTEXPR_ASSERT(SIZE > 0 && text[SIZE - 1] == '\0');
  }
  /// \brief Create a slice that refers to c_str[0,strlen(c_str)-1].
  explicit MDBX_CXX17_CONSTEXPR slice(const char *c_str);

  /// \brief Create a slice that refers to the contents of "str".
  /// \note 'explicit' to avoid reference to the temporary std::string instance.
  template <class CHAR, class T, class A>
  explicit MDBX_CXX20_CONSTEXPR slice(const ::std::basic_string<CHAR, T, A> &str)
      : slice(str.data(), str.length() * sizeof(CHAR)) {}

  MDBX_CXX14_CONSTEXPR slice(const MDBX_val &src);
  MDBX_CXX11_CONSTEXPR slice(const slice &) noexcept = default;
  MDBX_CXX14_CONSTEXPR slice(MDBX_val &&src);
  MDBX_CXX14_CONSTEXPR slice(slice &&src) noexcept;

#if defined(DOXYGEN) || (defined(__cpp_lib_span) && __cpp_lib_span >= 202002L)
  template <typename POD> MDBX_CXX14_CONSTEXPR slice(const ::std::span<POD> &span) : slice(span.begin(), span.end()) {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
  }

  template <typename POD> MDBX_CXX14_CONSTEXPR ::std::span<const POD> as_span() const {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() % sizeof(POD) == 0))
      MDBX_CXX20_LIKELY
    return ::std::span<const POD>(static_cast<const POD *>(data()), size() / sizeof(POD));
    throw_bad_value_size();
  }

  template <typename POD> MDBX_CXX14_CONSTEXPR ::std::span<POD> as_span() {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() % sizeof(POD) == 0))
      MDBX_CXX20_LIKELY
    return ::std::span<POD>(static_cast<POD *>(data()), size() / sizeof(POD));
    throw_bad_value_size();
  }

  MDBX_CXX14_CONSTEXPR ::std::span<const byte> bytes() const { return as_span<const byte>(); }
  MDBX_CXX14_CONSTEXPR ::std::span<byte> bytes() { return as_span<byte>(); }
  MDBX_CXX14_CONSTEXPR ::std::span<const char> chars() const { return as_span<const char>(); }
  MDBX_CXX14_CONSTEXPR ::std::span<char> chars() { return as_span<char>(); }
#endif /* __cpp_lib_span >= 202002L */

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  /// \brief Create a slice that refers to the same contents as "string_view"
  template <class CHAR, class T>
  MDBX_CXX14_CONSTEXPR slice(const ::std::basic_string_view<CHAR, T> &sv) : slice(sv.data(), sv.data() + sv.length()) {}

  template <class CHAR, class T> slice(::std::basic_string_view<CHAR, T> &&sv) : slice(sv) { sv = {}; }
#endif /* __cpp_lib_string_view >= 201606L */

  template <size_t SIZE> static MDBX_CXX14_CONSTEXPR slice wrap(const char (&text)[SIZE]) { return slice(text); }

  template <typename POD> MDBX_CXX14_CONSTEXPR static slice wrap(const POD &pod) {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    return slice(&pod, sizeof(pod));
  }

  inline slice &assign(const void *ptr, size_t bytes);
  inline slice &assign(const slice &src) noexcept;
  inline slice &assign(const ::MDBX_val &src);
  inline slice &assign(slice &&src) noexcept;
  inline slice &assign(::MDBX_val &&src);
  inline slice &assign(const void *begin, const void *end);
  template <class CHAR, class T, class ALLOCATOR> slice &assign(const ::std::basic_string<CHAR, T, ALLOCATOR> &str) {
    return assign(str.data(), str.length() * sizeof(CHAR));
  }
  inline slice &assign(const char *c_str);
#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T> slice &assign(const ::std::basic_string_view<CHAR, T> &view) {
    return assign(view.begin(), view.end());
  }
  template <class CHAR, class T> slice &assign(::std::basic_string_view<CHAR, T> &&view) {
    assign(view);
    view = {};
    return *this;
  }
#endif /* __cpp_lib_string_view >= 201606L */

  slice &operator=(const slice &) noexcept = default;
  inline slice &operator=(slice &&src) noexcept;
  inline slice &operator=(::MDBX_val &&src);
  operator MDBX_val *() noexcept { return this; }
  operator const MDBX_val *() const noexcept { return this; }

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T> slice &operator=(const ::std::basic_string_view<CHAR, T> &view) {
    return assign(view);
  }

  template <class CHAR, class T> slice &operator=(::std::basic_string_view<CHAR, T> &&view) { return assign(view); }

  /// \brief Return a string_view that references the same data as this slice.
  template <class CHAR = char, class T = ::std::char_traits<CHAR>>
  MDBX_CXX11_CONSTEXPR ::std::basic_string_view<CHAR, T> string_view() const noexcept {
    static_assert(sizeof(CHAR) == 1, "Must be single byte characters");
    return ::std::basic_string_view<CHAR, T>(char_ptr(), length());
  }

  /// \brief Return a string_view that references the same data as this slice.
  template <class CHAR, class T>
  MDBX_CXX11_CONSTEXPR explicit operator ::std::basic_string_view<CHAR, T>() const noexcept {
    return this->string_view<CHAR, T>();
  }
#endif /* __cpp_lib_string_view >= 201606L */

  template <class CHAR = char, class T = ::std::char_traits<CHAR>, class ALLOCATOR = default_allocator>
  MDBX_CXX20_CONSTEXPR ::std::basic_string<CHAR, T, ALLOCATOR> as_string(const ALLOCATOR &alloc = ALLOCATOR()) const {
    static_assert(sizeof(CHAR) == 1, "Must be single byte characters");
    return ::std::basic_string<CHAR, T, ALLOCATOR>(char_ptr(), length(), alloc);
  }

  template <class CHAR, class T, class ALLOCATOR>
  MDBX_CXX20_CONSTEXPR explicit operator ::std::basic_string<CHAR, T, ALLOCATOR>() const {
    return as_string<CHAR, T, ALLOCATOR>();
  }

  /// \brief Returns a string with a hexadecimal dump of the slice content.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_hex_string(bool uppercase = false, unsigned wrap_width = 0,
                                         const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a string with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the slice content.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_base58_string(unsigned wrap_width = 0, const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a string with a
  /// [Base58](https://en.wikipedia.org/wiki/Base64) dump of the slice content.
  template <class ALLOCATOR = default_allocator>
  inline string<ALLOCATOR> as_base64_string(unsigned wrap_width = 0, const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a hexadecimal dump of the slice content.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> encode_hex(bool uppercase = false, unsigned wrap_width = 0,
                                                       const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump of the slice content.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> encode_base58(unsigned wrap_width = 0,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Returns a buffer with a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump of the slice content.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> encode_base64(unsigned wrap_width = 0,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes hexadecimal dump from the slice content to returned buffer.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> hex_decode(bool ignore_spaces = false,
                                                       const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes [Base58](https://en.wikipedia.org/wiki/Base58) dump
  /// from the slice content to returned buffer.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> base58_decode(bool ignore_spaces = false,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Decodes [Base64](https://en.wikipedia.org/wiki/Base64) dump
  /// from the slice content to returned buffer.
  template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
  inline buffer<ALLOCATOR, CAPACITY_POLICY> base64_decode(bool ignore_spaces = false,
                                                          const ALLOCATOR &alloc = ALLOCATOR()) const;

  /// \brief Checks whether the content of the slice is printable.
  /// \param [in] disable_utf8 By default if `disable_utf8` is `false` function
  /// checks that content bytes are printable ASCII-7 characters or a valid UTF8
  /// sequences. Otherwise, if `disable_utf8` is `true` function checks that
  /// content bytes are printable extended 8-bit ASCII codes.
  MDBX_NOTHROW_PURE_FUNCTION bool is_printable(bool disable_utf8 = false) const noexcept;

  /// \brief Checks whether the content of the slice is a hexadecimal dump.
  /// \param [in] ignore_spaces If `true` function will skips spaces surrounding
  /// (before, between and after) a encoded bytes. However, spaces should not
  /// break a pair of characters encoding a single byte.
  MDBX_NOTHROW_PURE_FUNCTION inline bool is_hex(bool ignore_spaces = false) const noexcept;

  /// \brief Checks whether the content of the slice is a
  /// [Base58](https://en.wikipedia.org/wiki/Base58) dump.
  /// \param [in] ignore_spaces If `true` function will skips spaces surrounding
  /// (before, between and after) a encoded bytes. However, spaces should not
  /// break a code group of characters.
  MDBX_NOTHROW_PURE_FUNCTION inline bool is_base58(bool ignore_spaces = false) const noexcept;

  /// \brief Checks whether the content of the slice is a
  /// [Base64](https://en.wikipedia.org/wiki/Base64) dump.
  /// \param [in] ignore_spaces If `true` function will skips spaces surrounding
  /// (before, between and after) a encoded bytes. However, spaces should not
  /// break a code group of characters.
  MDBX_NOTHROW_PURE_FUNCTION inline bool is_base64(bool ignore_spaces = false) const noexcept;

#if defined(DOXYGEN) || (defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L)
  template <class CHAR, class T> void swap(::std::basic_string_view<CHAR, T> &view) noexcept {
    static_assert(sizeof(CHAR) == 1, "Must be single byte characters");
    const auto temp = ::std::basic_string_view<CHAR, T>(*this);
    *this = view;
    view = temp;
  }
#endif /* __cpp_lib_string_view >= 201606L */

  /// \brief Returns casted to pointer to byte an address of data.
  MDBX_CXX11_CONSTEXPR const byte *byte_ptr() const noexcept;
  MDBX_CXX11_CONSTEXPR byte *byte_ptr() noexcept;

  /// \brief Returns casted to pointer to byte an end of data.
  MDBX_CXX11_CONSTEXPR const byte *end_byte_ptr() const noexcept;
  MDBX_CXX11_CONSTEXPR byte *end_byte_ptr() noexcept;

  /// \brief Returns casted to pointer to char an address of data.
  MDBX_CXX11_CONSTEXPR const char *char_ptr() const noexcept;
  MDBX_CXX11_CONSTEXPR char *char_ptr() noexcept;

  /// \brief Returns casted to pointer to char an end of data.
  MDBX_CXX11_CONSTEXPR const char *end_char_ptr() const noexcept;
  MDBX_CXX11_CONSTEXPR char *end_char_ptr() noexcept;

  /// \brief Return a pointer to the beginning of the referenced data.
  MDBX_CXX11_CONSTEXPR const void *data() const noexcept;
  MDBX_CXX11_CONSTEXPR void *data() noexcept;

  /// \brief Return a pointer to the ending of the referenced data.
  MDBX_CXX11_CONSTEXPR const void *end() const noexcept;
  MDBX_CXX11_CONSTEXPR void *end() noexcept;

  /// \brief Returns the number of bytes.
  MDBX_CXX11_CONSTEXPR size_t length() const noexcept;

  /// \brief Set slice length.
  MDBX_CXX14_CONSTEXPR slice &set_length(size_t bytes);

  /// \brief Sets the length by specifying the end of the slice data.
  MDBX_CXX14_CONSTEXPR slice &set_end(const void *ptr);

  /// \brief Checks whether the slice is empty.
  MDBX_CXX11_CONSTEXPR bool empty() const noexcept;

  /// \brief Checks whether the slice data pointer is nullptr.
  MDBX_CXX11_CONSTEXPR bool is_null() const noexcept;

  /// \brief Returns the number of bytes.
  MDBX_CXX11_CONSTEXPR size_t size() const noexcept;

  /// \brief Returns true if slice is not empty.
  MDBX_CXX11_CONSTEXPR operator bool() const noexcept;

  /// \brief Depletes content of slice and make it invalid.
  MDBX_CXX14_CONSTEXPR void invalidate() noexcept;

  /// \brief Makes the slice empty and referencing to nothing.
  MDBX_CXX14_CONSTEXPR void clear() noexcept;

  /// \brief Drops the first "n" bytes from this slice.
  /// \pre REQUIRES: `n <= size()`
  inline void remove_prefix(size_t n) noexcept;

  /// \brief Drops the last "n" bytes from this slice.
  /// \pre REQUIRES: `n <= size()`
  inline void remove_suffix(size_t n) noexcept;

  /// \brief Drops the first "n" bytes from this slice.
  /// \throws std::out_of_range if `n > size()`
  inline void safe_remove_prefix(size_t n);

  /// \brief Drops the last "n" bytes from this slice.
  /// \throws std::out_of_range if `n > size()`
  inline void safe_remove_suffix(size_t n);

  /// \brief Checks if the data starts with the given prefix.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool starts_with(const slice &prefix) const noexcept;

  /// \brief Checks if the data ends with the given suffix.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR bool ends_with(const slice &suffix) const noexcept;

  /// \brief Returns the nth byte in the referenced data.
  /// \pre REQUIRES: `n < size()`
  MDBX_CXX11_CONSTEXPR byte operator[](size_t n) const noexcept;

  /// \brief Returns the nth byte in the referenced data with bounds checking.
  /// \throws std::out_of_range if `n >= size()`
  MDBX_CXX11_CONSTEXPR byte at(size_t n) const;

  /// \brief Returns the first "n" bytes of the slice.
  /// \pre REQUIRES: `n <= size()`
  MDBX_CXX14_CONSTEXPR slice head(size_t n) const noexcept;

  /// \brief Returns the last "n" bytes of the slice.
  /// \pre REQUIRES: `n <= size()`
  MDBX_CXX14_CONSTEXPR slice tail(size_t n) const noexcept;

  /// \brief Returns the middle "n" bytes of the slice.
  /// \pre REQUIRES: `from + n <= size()`
  MDBX_CXX14_CONSTEXPR slice middle(size_t from, size_t n) const noexcept;

  /// \brief Returns the first "n" bytes of the slice.
  /// \throws std::out_of_range if `n >= size()`
  MDBX_CXX14_CONSTEXPR slice safe_head(size_t n) const;

  /// \brief Returns the last "n" bytes of the slice.
  /// \throws std::out_of_range if `n >= size()`
  MDBX_CXX14_CONSTEXPR slice safe_tail(size_t n) const;

  /// \brief Returns the middle "n" bytes of the slice.
  /// \throws std::out_of_range if `from + n >= size()`
  MDBX_CXX14_CONSTEXPR slice safe_middle(size_t from, size_t n) const;

  /// \brief Returns the hash value of referenced data.
  /// \attention Function implementation and returned hash values may changed
  /// version to version, and in future the t1ha3 will be used here. Therefore
  /// values obtained from this function shouldn't be persisted anywhere.
  MDBX_NOTHROW_PURE_FUNCTION MDBX_CXX14_CONSTEXPR size_t hash_value() const noexcept;

  /// \brief Three-way fast non-lexicographically length-based comparison.
  /// \details Firstly compares length and if it equal then compare content
  /// lexicographically. \return value:
  ///  `== 0` if `a` the same as `b`;
  ///   `< 0` if `a` shorter than `b`,
  ///             or the same length and lexicographically less than `b`;
  ///   `> 0` if `a` longer than `b`,
  ///             or the same length and lexicographically great than `b`.
  MDBX_NOTHROW_PURE_FUNCTION static MDBX_CXX14_CONSTEXPR intptr_t compare_fast(const slice &a, const slice &b) noexcept;

  /// \brief Three-way lexicographically comparison.
  /// \return value:
  ///  `== 0` if `a` lexicographically equal `b`;
  ///   `< 0` if `a` lexicographically less than `b`;
  ///   `> 0` if `a` lexicographically great than `b`.
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

  /// \brief Checks the slice is not refers to null address or has zero length.
  MDBX_CXX11_CONSTEXPR bool is_valid() const noexcept { return !(iov_base == nullptr && iov_len != 0); }

  /// \brief Build an invalid slice which non-zero length and refers to null address.
  MDBX_CXX14_CONSTEXPR static slice invalid() noexcept {
    return slice(/* using special constructor without length checking */ ~size_t(0));
  }

  /// \brief Build a null slice which zero length and refers to null address.
  MDBX_CXX14_CONSTEXPR static slice null() noexcept { return slice(nullptr, size_t(0)); }

  template <typename POD> MDBX_CXX14_CONSTEXPR POD as_pod() const {
    static_assert(::std::is_standard_layout<POD>::value && !::std::is_pointer<POD>::value,
                  "Must be a standard layout type!");
    if (MDBX_LIKELY(size() == sizeof(POD)))
      MDBX_CXX20_LIKELY {
        POD r;
        memcpy(&r, data(), sizeof(r));
        return r;
      }
    throw_bad_value_size();
  }

#ifdef MDBX_U128_TYPE
  MDBX_CXX14_CONSTEXPR MDBX_U128_TYPE as_uint128() const { return as_pod<MDBX_U128_TYPE>(); }
#endif /* MDBX_U128_TYPE */
  MDBX_CXX14_CONSTEXPR uint64_t as_uint64() const { return as_pod<uint64_t>(); }
  MDBX_CXX14_CONSTEXPR uint32_t as_uint32() const { return as_pod<uint32_t>(); }
  MDBX_CXX14_CONSTEXPR uint16_t as_uint16() const { return as_pod<uint16_t>(); }
  MDBX_CXX14_CONSTEXPR uint8_t as_uint8() const { return as_pod<uint8_t>(); }

#ifdef MDBX_I128_TYPE
  MDBX_CXX14_CONSTEXPR MDBX_I128_TYPE as_int128() const { return as_pod<MDBX_I128_TYPE>(); }
#endif /* MDBX_I128_TYPE */
  MDBX_CXX14_CONSTEXPR int64_t as_int64() const { return as_pod<int64_t>(); }
  MDBX_CXX14_CONSTEXPR int32_t as_int32() const { return as_pod<int32_t>(); }
  MDBX_CXX14_CONSTEXPR int16_t as_int16() const { return as_pod<int16_t>(); }
  MDBX_CXX14_CONSTEXPR int8_t as_int8() const { return as_pod<int8_t>(); }

#ifdef MDBX_U128_TYPE
  MDBX_U128_TYPE as_uint128_adapt() const;
#endif /* MDBX_U128_TYPE */
  uint64_t as_uint64_adapt() const;
  uint32_t as_uint32_adapt() const;
  uint16_t as_uint16_adapt() const;
  uint8_t as_uint8_adapt() const;

#ifdef MDBX_I128_TYPE
  MDBX_I128_TYPE as_int128_adapt() const;
#endif /* MDBX_I128_TYPE */
  int64_t as_int64_adapt() const;
  int32_t as_int32_adapt() const;
  int16_t as_int16_adapt() const;
  int8_t as_int8_adapt() const;

protected:
  MDBX_CXX11_CONSTEXPR slice(size_t invalid_length) noexcept : ::MDBX_val({nullptr, invalid_length}) {}
};

/// \brief Combines data slice with boolean flag to represent result of certain operations.
struct value_result {
  slice value;
  bool done;
  value_result(const slice &value, bool done) noexcept : value(value), done(done) {}
  value_result(const value_result &) noexcept = default;
  value_result &operator=(const value_result &) noexcept = default;
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept {
    MDBX_INLINE_API_ASSERT(!done || bool(value));
    return done;
  }
};

/// \brief Combines pair of slices for key and value to represent result of certain operations.
struct pair {
  using stl_pair = std::pair<slice, slice>;
  slice key, value;
  MDBX_CXX11_CONSTEXPR pair(const slice &key, const slice &value) noexcept : key(key), value(value) {}
  MDBX_CXX11_CONSTEXPR pair(const stl_pair &couple) noexcept : key(couple.first), value(couple.second) {}
  MDBX_CXX11_CONSTEXPR operator stl_pair() const noexcept { return stl_pair(key, value); }
  pair(const pair &) noexcept = default;
  pair &operator=(const pair &) noexcept = default;
  pair &operator=(pair &&couple) noexcept {
    key.assign(std::move(couple.key));
    value.assign(std::move(couple.value));
    return *this;
  }
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept {
    MDBX_INLINE_API_ASSERT(bool(key) == bool(value));
    return key;
  }
  MDBX_CXX14_CONSTEXPR static pair invalid() noexcept { return pair(slice::invalid(), slice::invalid()); }

  pair &operator=(const stl_pair &couple) {
    key.assign(couple.first);
    value.assign(couple.second);
    return *this;
  }
  pair &operator=(stl_pair &&couple) {
    key.assign(std::move(couple.first));
    value.assign(std::move(couple.second));
    return *this;
  }

  /// \brief Three-way fast non-lexicographically length-based comparison.
  MDBX_NOTHROW_PURE_FUNCTION static MDBX_CXX14_CONSTEXPR intptr_t compare_fast(const pair &a, const pair &b) noexcept;

  /// \brief Three-way lexicographically comparison.
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
  bool done;
  MDBX_CXX11_CONSTEXPR pair_result() noexcept : pair(pair::invalid()), done(false) {}
  MDBX_CXX11_CONSTEXPR pair_result(const slice &key, const slice &value, bool done) noexcept
      : pair(key, value), done(done) {}
  pair_result(const pair_result &) noexcept = default;
  pair_result &operator=(const pair_result &) noexcept = default;
  MDBX_CXX14_CONSTEXPR operator bool() const noexcept {
    MDBX_INLINE_API_ASSERT(!done || (bool(key) && bool(value)));
    return done;
  }
};

// > dist-cutoff-begin
} // namespace mdbx
// < dist-cutoff-end
