/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#define LIBMDBX_INTERNALS
#define MDBX_DEPRECATED

#ifdef _MSC_VER
#if _MSC_VER > 1800
#pragma warning(disable : 4464) /* relative include path contains '..' */
#endif
#pragma warning(disable : 4996) /* The POSIX name is deprecated... */
#endif                          /* _MSC_VER (warnings) */

#ifdef MDBX_CONFIG_H
#include MDBX_CONFIG_H
#endif

#include "preface.h"

#if defined(xMDBX_ALLOY) && !defined(xMDBX_TOOLS)
/* Amalgamated build */
#define MDBX_INTERNAL static
#else
/* Non-amalgamated build */
#define MDBX_INTERNAL
#endif /* xMDBX_ALLOY */

#include "../mdbx.h"

/*----------------------------------------------------------------------------*/
/* Basic constants and types */

typedef struct iov_ctx iov_ctx_t;

typedef union bin128 {
  __anonymous_struct_extension__ struct {
    uint64_t x, y;
  };
  __anonymous_struct_extension__ struct {
    uint32_t a, b, c, d;
  };
  __anonymous_struct_extension__ struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t l, h;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    uint64_t h, l;
#else
#error "FIXME: Unsupported byte order"
#endif /* __BYTE_ORDER__ */
  };

#if defined(__SIZEOF_INT128__)
#define MDBX_HAVE_NATIVE_U128 1
  __int128_t i128;
  __uint128_t u128;
#else
#define MDBX_HAVE_NATIVE_U128 0
#endif
} bin128_t;

#include "osal.h"

#include "options.h"

#include "atomics-types.h"

#include "layout-dxb.h"
#include "layout-lck.h"

#define MIN_MAPSIZE (MDBX_MIN_PAGESIZE * MIN_PAGENO)
#if defined(_WIN32) || defined(_WIN64)
#define MAX_MAPSIZE32 UINT32_C(0x38000000)
#else
#define MAX_MAPSIZE32 UINT32_C(0x7f000000)
#endif
#define MAX_MAPSIZE64 ((MAX_PAGENO + 1) * (uint64_t)MDBX_MAX_PAGESIZE)

#if MDBX_WORDBITS >= 64
#define MAX_MAPSIZE MAX_MAPSIZE64
#define PAGELIST_LIMIT ((size_t)MAX_PAGENO)
#else
#define MAX_MAPSIZE MAX_MAPSIZE32
#define PAGELIST_LIMIT (MAX_MAPSIZE32 / MDBX_MIN_PAGESIZE)
#endif /* MDBX_WORDBITS */

#define MDBX_GOLD_RATIO_DBL 1.6180339887498948482
#define MEGABYTE ((size_t)1 << 20)

/*----------------------------------------------------------------------------*/

union logger_union {
  void *ptr;
  MDBX_debug_func *fmt;
  MDBX_debug_func_nofmt *nofmt;
};

struct libmdbx_globals {
  bin128_t bootid;
  unsigned sys_pagesize, sys_allocation_granularity;
#ifdef AT_UCACHEBSIZE
  unsigned sys_unified_cache_block;
#endif /* AT_UCACHEBSIZE */
  uint8_t sys_pagesize_ln2;
  uint8_t runtime_flags;
  uint8_t loglevel;
#if defined(_WIN32) || defined(_WIN64)
  bool running_under_Wine;
#elif defined(__linux__) || defined(__gnu_linux__)
  bool running_on_WSL1 /* Windows Subsystem 1 for Linux */;
  uint32_t linux_kernel_version;
#endif /* Linux */
  union logger_union logger;
  osal_fastmutex_t debug_lock;
  size_t logger_buffer_size;
  char *logger_buffer;
};

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern struct libmdbx_globals globals;
#if defined(_WIN32) || defined(_WIN64)
extern struct libmdbx_imports imports;
#endif /* Windows */

#include "logging_and_debug.h"

#include "utils.h"

#include "pnl.h"

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define mdbx_sourcery_anchor XCONCAT(mdbx_sourcery_, MDBX_BUILD_SOURCERY)
#if defined(xMDBX_TOOLS)
extern LIBMDBX_API const char *const mdbx_sourcery_anchor;
#endif

#define MDBX_IS_ERROR(rc) ((rc) != MDBX_RESULT_TRUE && (rc) != MDBX_RESULT_FALSE)
