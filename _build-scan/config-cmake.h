/* This file is part of the libmdbx amalgamated source code (v0.15.0-19-ge84e2354 at 2026-09-25T19:15:56+00:00),
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

/* #undef MDBX_DEBUG */
/* #undef MDBX_CHECKING */

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
#define MDBX_BUILD_TIMESTAMP "2026-09-25T19:43:17Z"
#endif
#ifndef MDBX_BUILD_TARGET
#define MDBX_BUILD_TARGET "x86_64-ELF-Linux"
#endif
#ifndef MDBX_BUILD_TYPE
#define MDBX_BUILD_TYPE "Release"
#endif
#ifndef MDBX_BUILD_COMPILER
#define MDBX_BUILD_COMPILER "cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0"
#endif
#ifndef MDBX_BUILD_FLAGS
#define MDBX_BUILD_FLAGS " -fexceptions -fno-semantic-interposition -fno-common -ggdb -Wno-unknown-pragmas -ffunction-sections -fdata-sections -Wall -Wextra -O3 -DNDEBUG LIBMDBX_EXPORTS MDBX_BUILD_SHARED_LIBRARY=1 -ffast-math -fvisibility=hidden"
#endif
#ifndef MDBX_BUILD_METADATA
/* #undef MDBX_BUILD_METADATA */
#endif
#define MDBX_BUILD_SOURCERY 05d90aea6920f803d3d7d38c1491266b7699cf29c189ab11c6f37cc112f11392_v0_15_0_19_ge84e2354

/* *INDENT-ON* */
/* clang-format on */
