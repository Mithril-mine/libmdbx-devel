/* This file is part of the libmdbx amalgamated source code (v0.15.0-258-g639a884b at 2026-09-24T16:31:01+00:00),
 * it is the template for libmdbx's config.h
 ******************************************************************************/

/* *INDENT-OFF* */
/* clang-format off */

/* #undef LTO_ENABLED */
/* #undef ENABLE_MEMCHECK */
/* #undef ENABLE_GPROF */
/* #undef ENABLE_GCOV */
/* #undef ENABLE_DTRACE */
/* #undef ENABLE_SYSTEMTAP */
/* #undef ENABLE_ASAN */
/* #undef ENABLE_UBSAN */

/* Internal debugging */
#define MDBX_DEBUG_SPILLING 0
#define MDBX_DEBUG_SEARCH_DISPATCHING 0
#define MDBX_DEBUG_SEARCH_BRANCHLESS 0

/* allowing override MDBX_BUILD_CXX=OFF to build the C++ API in "private" mode only for tests */
#if !defined(MDBX_BUILD_TEST) && !defined(MDBX_BUILD_CXX)
#define MDBX_BUILD_CXX /* using MDBX_BUILD_CXX CMake's option */ 1
#endif

#define MDBX_DEBUG 1
#define MDBX_CHECKING 2

#define MDBX_TXN_CHECKOWNER 1
#define MDBX_ENV_CHECKPID_AUTO
#ifndef MDBX_ENV_CHECKPID_AUTO
#define MDBX_ENV_CHECKPID 0
#endif
#define MDBX_LOCKING_AUTO
#ifndef MDBX_LOCKING_AUTO
/* #undef MDBX_LOCKING */
#endif
#define MDBX_TRUST_RTC_AUTO
#ifndef MDBX_TRUST_RTC_AUTO
#define MDBX_TRUST_RTC 0
#endif
#define MDBX_DISABLE_VALIDATION 0
#define MDBX_AVOID_MSYNC 0
#define MDBX_ENABLE_REFUND 1
#define MDBX_ENABLE_BIGFOOT 1
#define MDBX_ENABLE_PGOP_STAT 1
#define MDBX_ENABLE_PGET_STAT 1
#define MDBX_ENABLE_PROFGC 0
#define MDBX_ENABLE_DBI_SPARSE 1
#define MDBX_ENABLE_DBI_LOCKFREE 1
#define MDBX_ENABLE_FAKE_NESTED_READONLY_TRANSACTIONS 0

/* Windows */
/* allowing override MDBX_WITHOUT_MSVC_CRT=ON to build the C++ API in "private" mode only for tests */
#if defined(MDBX_BUILD_TEST) || !defined(MDBX_BUILD_CXX) || MDBX_BUILD_CXX
#define MDBX_WITHOUT_MSVC_CRT /* hardcoded zero */ 0
#else
#define MDBX_WITHOUT_MSVC_CRT /* using MDBX_WITHOUT_MSVC_CRT CMake's option */ 0
#endif /* MDBX_WITHOUT_MSVC_CRT */
#define MDBX_NATIVE_SEH 0

/* MacOS & iOS */
#define MDBX_APPLE_SPEED_INSTEADOF_DURABILITY 0

/* POSIX */
#define MDBX_DISABLE_GNU_SOURCE 0

#define MDBX_USE_OFDLOCKS_AUTO
#ifndef MDBX_USE_OFDLOCKS_AUTO
#define MDBX_USE_OFDLOCKS 0
#endif /* MDBX_USE_OFDLOCKS */

#define MDBX_MMAP_NEEDS_JOLT_AUTO
#ifndef MDBX_MMAP_NEEDS_JOLT_AUTO
#define MDBX_MMAP_NEEDS_JOLT 0
#endif /* MDBX_MMAP_NEEDS_JOLT */

#define MDBX_USE_MINCORE 1

#define MDBX_USE_FALLOCATE_AUTO
#ifndef MDBX_USE_FALLOCATE_AUTO
#define MDBX_USE_FALLOCATE 0
#endif /* MDBX_USE_FALLOCATE */

/* Build Info */
#ifndef MDBX_BUILD_TIMESTAMP
#define MDBX_BUILD_TIMESTAMP "2026-09-24T18:03:33Z"
#endif
#ifndef MDBX_BUILD_TARGET
#define MDBX_BUILD_TARGET "x86_64-ELF-Linux"
#endif
#ifndef MDBX_BUILD_TYPE
#define MDBX_BUILD_TYPE "Debug"
#endif
#ifndef MDBX_BUILD_COMPILER
#define MDBX_BUILD_COMPILER "Ubuntu clang version 18.1.3 (1ubuntu1)"
#endif
#ifndef MDBX_BUILD_FLAGS
#define MDBX_BUILD_FLAGS " -fexceptions -fno-common -ggdb -Wno-unknown-pragmas -ffunction-sections -fdata-sections -Wall -Wextra -Werror -fcxx-exceptions -frtti -g LIBMDBX_EXPORTS MDBX_BUILD_SHARED_LIBRARY=1 -ffast-math -fvisibility=hidden"
#endif
#ifndef MDBX_BUILD_METADATA
/* #undef MDBX_BUILD_METADATA */
#endif
#define MDBX_BUILD_SOURCERY 5487ef225e0f412f1b86034eaebc6e6a1241b96208c89f255f7112a8e8409cf4_v0_15_0_258_g639a884b

/* *INDENT-ON* */
/* clang-format on */
