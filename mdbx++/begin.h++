// > dist-cutoff-begin
#ifndef MDBX_CXX_BEGIN_H
#define MDBX_CXX_BEGIN_H
// < dist-cutoff-end

/* Workaround for modern libstdc++ with CLANG < 4.x */
#if defined(__SIZEOF_INT128__) && !defined(__GLIBCXX_TYPE_INT_N_0) && defined(__clang__) && __clang_major__ < 4
#define __GLIBCXX_BITSIZE_INT_N_0 128
#define __GLIBCXX_TYPE_INT_N_0 __int128
#endif /* Workaround for modern libstdc++ with CLANG < 4.x */

#if !defined(__cplusplus) || __cplusplus < 201103L
#if !defined(_MSC_VER) || _MSC_VER < 1900
#error "C++11 compiler or better is required"
#elif _MSC_VER >= 1910
#error "Please add `/Zc:__cplusplus` to MSVC compiler options to enforce it conform ISO C++"
#endif /* MSVC is mad and don't define __cplusplus properly */
#endif /* __cplusplus < 201103L */

#if (defined(_WIN32) || defined(_WIN64)) && MDBX_WITHOUT_MSVC_CRT
#error "CRT is required for C++ API, the MDBX_WITHOUT_MSVC_CRT option must be disabled"
#endif /* Windows */

#ifndef __has_include
#define __has_include(header) (0)
#endif /* __has_include */

#if __has_include(<version>)
#include <version>
#endif /* <version> */

/* Disable min/max macros from C' headers */
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>   // for std::min/max
#include <cassert>     // for assert()
#include <climits>     // for CHAR_BIT
#include <cstring>     // for std::strlen, str:memcmp
#include <exception>   // for std::exception_ptr
#include <ostream>     // for std::ostream
#include <sstream>     // for std::ostringstream
#include <stdexcept>   // for std::invalid_argument
#include <string>      // for std::string
#include <type_traits> // for std::is_pod<>, etc.
#include <utility>     // for std::make_pair
#include <vector>      // for std::vector<> as template args

#if defined(__cpp_lib_memory_resource) && __cpp_lib_memory_resource >= 201603L
#include <memory_resource>
#endif

#if defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L
#include <string_view>
#endif

#ifndef MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM
#ifdef INCLUDE_STD_FILESYSTEM_EXPERIMENTAL
#define MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM 1
#elif defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L && __cplusplus >= 201703L
#define MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM 0
#elif (!defined(_MSC_VER) || __cplusplus >= 201403L ||                                                                 \
       (defined(_MSC_VER) && defined(_SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING) && __cplusplus >= 201403L))
#if defined(__cpp_lib_experimental_filesystem) && __cpp_lib_experimental_filesystem >= 201406L
#define MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM 1
#elif defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L && __has_include(<experimental/filesystem>)
#define MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM 1
#else
#define MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM 0
#endif
#else
#define MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM 0
#endif
#endif /* MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM */

#if MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM
#include <experimental/filesystem>
#elif defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L
#include <filesystem>
#endif

#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#include <span>
#endif

#if !defined(_MSC_VER) || defined(__clang__)
#define MDBX_EXTERN_API_TEMPLATE(API_ATTRIBUTES, ...) extern template class API_ATTRIBUTES __VA_ARGS__
#define MDBX_INSTALL_API_TEMPLATE(API_ATTRIBUTES, ...) template class __VA_ARGS__
#else
#define MDBX_EXTERN_API_TEMPLATE(API_ATTRIBUTES, ...) extern template class __VA_ARGS__
#define MDBX_INSTALL_API_TEMPLATE(API_ATTRIBUTES, ...) template class API_ATTRIBUTES __VA_ARGS__
#endif

#if defined(_MSC_VER) && _MSC_VER >= 1900
#define MDBX_MSVC_DECLSPEC_EMPTY_BASES __declspec(empty_bases)
#else
#define MDBX_MSVC_DECLSPEC_EMPTY_BASES /* nope */
#endif                                 /* _MSC_VER >= 1900 */

#if __cplusplus >= 201103L
#include <chrono>
#include <ratio>
#endif

#include "../mdbx.h"

#if (defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L) ||                                                  \
    (defined(__cpp_lib_endian) && __cpp_lib_endian >= 201907L) ||                                                      \
    (defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L) ||                                                      \
    (defined(__cpp_lib_int_pow2) && __cpp_lib_int_pow2 >= 202002L)
#include <bit>
#elif !(defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__))
#if defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && defined(__BIG_ENDIAN)
#define __ORDER_LITTLE_ENDIAN__ __LITTLE_ENDIAN
#define __ORDER_BIG_ENDIAN__ __BIG_ENDIAN
#define __BYTE_ORDER__ __BYTE_ORDER
#elif defined(_BYTE_ORDER) && defined(_LITTLE_ENDIAN) && defined(_BIG_ENDIAN)
#define __ORDER_LITTLE_ENDIAN__ _LITTLE_ENDIAN
#define __ORDER_BIG_ENDIAN__ _BIG_ENDIAN
#define __BYTE_ORDER__ _BYTE_ORDER
#else
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#if defined(__LITTLE_ENDIAN__) || (defined(_LITTLE_ENDIAN) && !defined(_BIG_ENDIAN)) || defined(__ARMEL__) ||          \
    defined(__THUMBEL__) || defined(__AARCH64EL__) || defined(__MIPSEL__) || defined(_MIPSEL) || defined(__MIPSEL) ||  \
    defined(_M_ARM) || defined(_M_ARM64) || defined(__e2k__) || defined(__elbrus_4c__) || defined(__elbrus_8c__) ||    \
    defined(__bfin__) || defined(__BFIN__) || defined(__ia64__) || defined(_IA64) || defined(__IA64__) ||              \
    defined(__ia64) || defined(_M_IA64) || defined(__itanium__) || defined(__ia32__) || defined(__CYGWIN__) ||         \
    defined(_WIN64) || defined(_WIN32) || defined(__TOS_WIN__) || defined(__WINDOWS__)
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#elif defined(__BIG_ENDIAN__) || (defined(_BIG_ENDIAN) && !defined(_LITTLE_ENDIAN)) || defined(__ARMEB__) ||           \
    defined(__THUMBEB__) || defined(__AARCH64EB__) || defined(__MIPSEB__) || defined(_MIPSEB) || defined(__MIPSEB) ||  \
    defined(__m68k__) || defined(M68000) || defined(__hppa__) || defined(__hppa) || defined(__HPPA__) ||               \
    defined(__sparc__) || defined(__sparc) || defined(__370__) || defined(__THW_370__) || defined(__s390__) ||         \
    defined(__s390x__) || defined(__SYSC_ZARCH__)
#define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
#endif
#endif
#endif /* Byte Order */

/** Workaround for old compilers without properly support for `C++17 constexpr` */
#if defined(DOXYGEN)
#define MDBX_CXX17_CONSTEXPR constexpr
#elif defined(__cpp_constexpr) && __cpp_constexpr >= 201603L &&                                                        \
    ((defined(_MSC_VER) && _MSC_VER >= 1915) || (defined(__clang__) && __clang_major__ > 5) ||                         \
     (defined(__GNUC__) && __GNUC__ > 7) || (!defined(__GNUC__) && !defined(__clang__) && !defined(_MSC_VER)))
#define MDBX_CXX17_CONSTEXPR constexpr
#else
#define MDBX_CXX17_CONSTEXPR inline
#endif /* MDBX_CXX17_CONSTEXPR */

/** Workaround for old compilers without properly support for C++20 `constexpr`. */
#if defined(DOXYGEN)
#define MDBX_CXX20_CONSTEXPR constexpr
#elif defined(__cpp_lib_is_constant_evaluated) && __cpp_lib_is_constant_evaluated >= 201811L &&                        \
    defined(__cpp_lib_constexpr_string) && __cpp_lib_constexpr_string >= 201907L
#define MDBX_CXX20_CONSTEXPR constexpr
#else
#define MDBX_CXX20_CONSTEXPR inline
#endif /* MDBX_CXX20_CONSTEXPR */

#if CONSTEXPR_ENUM_FLAGS_OPERATIONS || defined(DOXYGEN)
#define MDBX_CXX01_CONSTEXPR_ENUM MDBX_CXX01_CONSTEXPR
#define MDBX_CXX11_CONSTEXPR_ENUM MDBX_CXX11_CONSTEXPR
#define MDBX_CXX14_CONSTEXPR_ENUM MDBX_CXX14_CONSTEXPR
#define MDBX_CXX17_CONSTEXPR_ENUM MDBX_CXX17_CONSTEXPR
#define MDBX_CXX20_CONSTEXPR_ENUM MDBX_CXX20_CONSTEXPR
#else
#define MDBX_CXX01_CONSTEXPR_ENUM inline
#define MDBX_CXX11_CONSTEXPR_ENUM inline
#define MDBX_CXX14_CONSTEXPR_ENUM inline
#define MDBX_CXX17_CONSTEXPR_ENUM inline
#define MDBX_CXX20_CONSTEXPR_ENUM inline
#endif /* CONSTEXPR_ENUM_FLAGS_OPERATIONS */

/** Workaround for old compilers without support assertion inside `constexpr` functions. */
#if defined(CONSTEXPR_ASSERT)
#define MDBX_CONSTEXPR_ASSERT(expr) CONSTEXPR_ASSERT(expr)
#elif defined(NDEBUG) && (!defined(MDBX_CHECKING) || !MDBX_CHECKING) && (!defined(MDBX_DEBUG) || !MDBX_DEBUG)
#define MDBX_CONSTEXPR_ASSERT(expr) void(0)
#else
#define MDBX_CONSTEXPR_ASSERT(expr) ((expr) ? void(0) : [] { assert(!#expr); }())
#endif /* MDBX_CONSTEXPR_ASSERT */

/** Workaround for old compilers without properly support for C++20 `if constexpr`. */
#if defined(DOXYGEN)
#define MDBX_IF_CONSTEXPR constexpr
#elif defined(__cpp_if_constexpr) && __cpp_if_constexpr >= 201606L
#define MDBX_IF_CONSTEXPR constexpr
#else
#define MDBX_IF_CONSTEXPR
#endif /* MDBX_IF_CONSTEXPR */

#if defined(DOXYGEN) || (__has_cpp_attribute(fallthrough) && (!defined(__clang__) || __clang__ > 4)) ||                \
    __cplusplus >= 201703L
#define MDBX_CXX17_FALLTHROUGH [[fallthrough]]
#else
#define MDBX_CXX17_FALLTHROUGH
#endif /* MDBX_CXX17_FALLTHROUGH */

#if defined(DOXYGEN) || (__has_cpp_attribute(likely) >= 201803L && (!defined(__GNUC__) || __GNUC__ > 9))
#define MDBX_CXX20_LIKELY [[likely]]
#else
#define MDBX_CXX20_LIKELY
#endif /* MDBX_CXX20_LIKELY */

#ifndef MDBX_CXX20_UNLIKELY
#if defined(DOXYGEN) || (__has_cpp_attribute(unlikely) >= 201803L && (!defined(__GNUC__) || __GNUC__ > 9))
#define MDBX_CXX20_UNLIKELY [[unlikely]]
#else
#define MDBX_CXX20_UNLIKELY
#endif
#endif /* MDBX_CXX20_UNLIKELY */

#ifndef MDBX_HAVE_CXX20_CONCEPTS
#if defined(__cpp_concepts) && __cpp_concepts >= 202002L && defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L
#include <concepts>
#define MDBX_HAVE_CXX20_CONCEPTS 1
#elif defined(DOXYGEN)
#define MDBX_HAVE_CXX20_CONCEPTS 1
#else
#define MDBX_HAVE_CXX20_CONCEPTS 0
#endif /* <concepts> */
#endif /* MDBX_HAVE_CXX20_CONCEPTS */

#ifndef MDBX_CXX20_CONCEPT
#if MDBX_HAVE_CXX20_CONCEPTS || defined(DOXYGEN)
#define MDBX_CXX20_CONCEPT(CONCEPT, NAME) CONCEPT NAME
#else
#define MDBX_CXX20_CONCEPT(CONCEPT, NAME) typename NAME
#endif
#endif /* MDBX_CXX20_CONCEPT */

#ifndef MDBX_ASSERT_CXX20_CONCEPT_SATISFIED
#if MDBX_HAVE_CXX20_CONCEPTS || defined(DOXYGEN)
#define MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(CONCEPT, TYPE) static_assert(CONCEPT<TYPE>)
#else
#define MDBX_ASSERT_CXX20_CONCEPT_SATISFIED(CONCEPT, NAME)                                                             \
  static_assert(true, MDBX_STRINGIFY(CONCEPT) "<" MDBX_STRINGIFY(TYPE) ">")
#endif
#endif /* MDBX_ASSERT_CXX20_CONCEPT_SATISFIED */

#ifdef _MSC_VER
#pragma warning(push, 4)
#pragma warning(disable : 4127) /* conditional expression is constant */
#pragma warning(disable : 4251) /* 'std::FOO' needs to have dll-interface to                                           \
                                   be used by clients of 'mdbx::BAR' */
#pragma warning(disable : 4275) /* non dll-interface 'std::FOO' used as                                                \
                                   base for dll-interface 'mdbx::BAR' */
/* MSVC is mad and can generate this warning for its own intermediate
 * automatically generated code, which becomes unreachable after some kinds of
 * optimization (copy elision, etc). */
#pragma warning(disable : 4702) /* unreachable code */
#endif                          /* _MSC_VER (warnings) */

#if defined(__LCC__) && __LCC__ >= 126
#pragma diagnostic push
#if __LCC__ < 127
#pragma diag_suppress 3058 /* workaround: call to is_constant_evaluated()                                              \
                              appearing in a constant expression `true` */
#pragma diag_suppress 3060 /* workaround: call to is_constant_evaluated()                                              \
                              appearing in a constant expression `false` */
#endif
#endif /* E2K LCC (warnings) */

//------------------------------------------------------------------------------
/// \brief The libmdbx C++ API namespace
/// \ingroup cxx_api
namespace mdbx {

/// \defgroup cxx_api C++ API
/// @{

/// \brief The byte-like type that don't presumes aliases for pointers as does the `char`.
/// \details Essentially, to enable all kinds of an compiler optimization, we need just
/// the `unsigned char * restrict` type in C99 terms, i.e. the non-aliasing pointer to `unsigned char`.
/// However, C++ still doesn't have `restrict` keyword not `non-aliases` type attribute, but a char-pointers may be
/// aliased.
///
/// On the other hand, while `uint8_t` is provided and `CHAR_BIT = 8` the `char8_t *` actually act the same as the C99
/// `unsigned char * restrict`. So using `char8_t` should not be an issue, since both the `CHAR_BIT = 8` and `uint8_t
/// `are required.
///
/// At the same time, the approach of using `char8_t` has several advantages:
///  - the `restrict` attribute is defined on level of the base type and is inherited by any derived pointer type;
///  - some compilers treat `__restrict` as an attribute of an instance of a type (i.e. a variable, a specific
///    pointer), but not a type attribute.
///
/// Nonetheless, I should think about switching to the `uint8_t * __restrict__` for byte pointers.
/// \note Functions whose signature depends on the `mdbx::byte` type must be strictly defined as inline!
#if defined(DOXYGEN) || (defined(__cpp_char8_t) && __cpp_char8_t >= 201811)
using byte = char8_t;
#else
// Avoid `std::byte` since it doesn't add features but inconvenient restrictions.
using byte = unsigned char;
#endif /* __cpp_char8_t >= 201811*/

#if defined(__cpp_lib_endian) && __cpp_lib_endian >= 201907L
using endian = ::std::endian;
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
enum class endian { little = __ORDER_LITTLE_ENDIAN__, big = __ORDER_BIG_ENDIAN__, native = __BYTE_ORDER__ };
#else
#error "Please use a C++ compiler provides byte order information or C++20 support"
#endif /* Byte Order enum */

/// \copydoc MDBX_version_info
using version_info = ::MDBX_version_info;
/// \brief Returns libmdbx version information.
MDBX_CXX11_CONSTEXPR const version_info &get_version() noexcept;
/// \copydoc MDBX_build_info
using build_info = ::MDBX_build_info;
/// \brief Returns libmdbx build information.
MDBX_CXX11_CONSTEXPR const build_info &get_build() noexcept;

/// \brief constexpr-enabled strlen().
static MDBX_CXX17_CONSTEXPR size_t strlen(const char *c_str) noexcept;

/// \brief constexpr-enabled memcpy().
static MDBX_CXX20_CONSTEXPR void *memcpy(void *dest, const void *src, size_t bytes) noexcept;
/// \brief constexpr-enabled memcmp().
static MDBX_CXX20_CONSTEXPR int memcmp(const void *a, const void *b, size_t bytes) noexcept;

/// \brief Legacy allocator
/// but it is recommended to use \ref polymorphic_allocator.
using legacy_allocator = ::std::string::allocator_type;

/// \brief Defined as `1` if the `mdbx::polymorphic_allocator` is available.
/// \see The `<memory_resource>` Standard C++17 library header
#if defined(DOXYGEN)
#define MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR 1
#elif !defined(MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR)
#if defined(__cpp_lib_memory_resource) && __cpp_lib_memory_resource >= 201603L &&                                      \
    (!defined(_GLIBCXX_USE_CXX11_ABI) || _GLIBCXX_USE_CXX11_ABI)
#define MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR 1
#else
#define MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR 0
#endif
#endif /* MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR */

#if defined(DOXYGEN) || MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR
/// \brief Default polymorphic allocator for modern code.
using polymorphic_allocator = ::std::pmr::string::allocator_type;
using default_allocator = polymorphic_allocator;
#else
using default_allocator = legacy_allocator;
#endif /* MDBX_CXX_HAS_POLYMORPHIC_ALLOCATOR */

struct slice;
struct default_capacity_policy;
template <class ALLOCATOR = default_allocator, class CAPACITY_POLICY = default_capacity_policy>
class MDBX_MSVC_DECLSPEC_EMPTY_BASES buffer;
class env;
class env_managed;
class txn;
class txn_managed;
class cursor;
class cursor_managed;

/// \brief Transaction ID and MVCC-snapshot number.
using txnid = uint64_t;

/// \brief Default single-byte string.
template <class ALLOCATOR = default_allocator>
using string = ::std::basic_string<char, ::std::char_traits<char>, ALLOCATOR>;

using filehandle = ::mdbx_filehandle_t;
#if MDBX_USING_CXX_EXPERIMETAL_FILESYSTEM
#ifdef _MSC_VER
namespace filesystem = ::std::experimental::filesystem::v1;
#else
namespace filesystem = ::std::experimental::filesystem;
#endif
#define MDBX_STD_FILESYSTEM_PATH ::mdbx::filesystem::path
#elif defined(DOXYGEN) ||                                                                                              \
    (defined(__cpp_lib_filesystem) && __cpp_lib_filesystem >= 201703L && defined(__cpp_lib_string_view) &&             \
     __cpp_lib_string_view >= 201606L &&                                                                               \
     (!defined(__MAC_OS_X_VERSION_MIN_REQUIRED) || __MAC_OS_X_VERSION_MIN_REQUIRED >= 101500) &&                       \
     (!defined(__IPHONE_OS_VERSION_MIN_REQUIRED) || __IPHONE_OS_VERSION_MIN_REQUIRED >= 130100)) &&                    \
        (!defined(_MSC_VER) || __cplusplus >= 201703L)
namespace filesystem = ::std::filesystem;
/// \brief Defined if `mdbx::filesystem::path` is available.
/// \details If defined, it is always `mdbx::filesystem::path`,
/// which in turn can be refs to `std::filesystem::path` or `std::experimental::filesystem::path`.
/// Nonetheless `MDBX_STD_FILESYSTEM_PATH` not defined if the `::mdbx::path`
/// is fallbacked to c `std::string` or `std::wstring`.
#define MDBX_STD_FILESYSTEM_PATH ::mdbx::filesystem::path
#endif /* MDBX_STD_FILESYSTEM_PATH */

#ifdef MDBX_STD_FILESYSTEM_PATH
using path = MDBX_STD_FILESYSTEM_PATH;
using path_string = MDBX_STD_FILESYSTEM_PATH::string_type;
#elif defined(_WIN32) || defined(_WIN64)
using path = ::std::wstring;
using path_string = path;
#else
using path = ::std::string;
using path_string = path;
#endif /* mdbx::path */
using path_char = path_string::value_type;

#if defined(__SIZEOF_INT128__) || (defined(_INTEGRAL_MAX_BITS) && _INTEGRAL_MAX_BITS >= 128)
#ifndef MDBX_U128_TYPE
#define MDBX_U128_TYPE __uint128_t
#endif /* MDBX_U128_TYPE */
#ifndef MDBX_I128_TYPE
#define MDBX_I128_TYPE __int128_t
#endif /* MDBX_I128_TYPE */
#endif /* __SIZEOF_INT128__ || _INTEGRAL_MAX_BITS >= 128 */

#if __cplusplus >= 201103L || defined(DOXYGEN)
/// \brief Duration in 1/65536 units of second.
using duration = ::std::chrono::duration<unsigned, ::std::ratio<1, 65536>>;
#endif /* Duration for C++11 */

// > dist-cutoff-begin
} // namespace mdbx
#endif /* MDBX_CXX_BEGIN_H */
// < dist-cutoff-end
