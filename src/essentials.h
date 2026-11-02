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

#if IS_WINDOWS && !(defined(__CYGWIN__) || defined(__MINGW__) || defined(__MINGW32__) || defined(__MINGW64__))
/* Protection against leakage/use of POSIX identifiers during build for Windows,
 * which may also be defined as unstable, arbitrary values dependent on the build environment.
 *   git grep -h -w -o 'E[A-Z]\+' | sort | uniq
 */
#undef EACCES
#undef EAFNOSUPPORT
#undef EAGAIN
#undef EBADF
#undef EBADFD
#undef EBUSY
#undef ECHILD
#undef EDEADLK
#undef EDQUOT
#undef EEXIST
#undef EFAULT
#undef EIDRM
#undef EINTR
#undef EINVAL
#undef EIO
#undef ENODATA
#undef ENOENT
#undef ENOFILE
#undef ENOIMPL
#undef ENOLCK
#undef ENOMEM
#undef ENOPROTOOPT
#undef ENOSPC
#undef ENOSUPP
#undef ENOSYS
#undef ENOTBLK
#undef ENOTDIR
#undef ENOTSUP
#undef ENOTTY
#undef EOPNOTSUPP
#undef EOWNERDEAD
#undef EPERM
#undef EPIPE
#undef EREMOTEIO
#undef EROFS
#undef ESRCH
#undef EWOULDBLOCK
#undef EXDEV
#define POSIX_FUSE POSIX_error_codes_should_not_be_used_in_Windows_build
#define EACCES POSIX_FUSE
#define EAFNOSUPPORT POSIX_FUSE
#define EAGAIN POSIX_FUSE
#define EBADF POSIX_FUSE
#define EBADFD POSIX_FUSE
#define EBUSY POSIX_FUSE
#define ECHILD POSIX_FUSE
#define EDEADLK POSIX_FUSE
#define EDQUOT POSIX_FUSE
#define EEXIST POSIX_FUSE
#define EFAULT POSIX_FUSE
#define EIDRM POSIX_FUSE
#define EINTR POSIX_FUSE
extern int mdbx_POSIX_error_codes_should_not_be_used_in_Windows_build(void);
#define EINVAL mdbx_POSIX_error_codes_should_not_be_used_in_Windows_build()
#define EIO POSIX_FUSE
#define ENODATA POSIX_FUSE
#define ENOENT POSIX_FUSE
#define ENOFILE POSIX_FUSE
#define ENOIMPL POSIX_FUSE
#define ENOLCK POSIX_FUSE
#define ENOMEM POSIX_FUSE
#define ENOPROTOOPT POSIX_FUSE
#define ENOSPC POSIX_FUSE
#define ENOSUPP POSIX_FUSE
#define ENOSYS POSIX_FUSE
#define ENOTBLK POSIX_FUSE
#define ENOTDIR POSIX_FUSE
#define ENOTSUP POSIX_FUSE
#define ENOTTY POSIX_FUSE
#define EOPNOTSUPP POSIX_FUSE
#define EOWNERDEAD POSIX_FUSE
#define EPERM POSIX_FUSE
#define EPIPE POSIX_FUSE
#define EREMOTEIO POSIX_FUSE
#define EROFS POSIX_FUSE
#define ESRCH POSIX_FUSE
#define EWOULDBLOCK POSIX_FUSE
#define EXDEV POSIX_FUSE
#endif /* IS_WINDOWS */

#if defined(xMDBX_ALLOY) && !defined(xMDBX_TOOLS)
/* Amalgamated build */
#define MDBX_INTERNAL static
#else
/* Non-amalgamated build */
#define MDBX_INTERNAL
#endif /* xMDBX_ALLOY */

#if MDBX_WITHOUT_MSVC_CRT && !defined(_DEBUG) && defined(_MSC_VER) && !defined(__clang__)
#pragma check_stack(off)
#pragma runtime_checks("scu", off)
#endif /* MDBX_WITHOUT_MSVC_CRT && !_DEBUG */

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

#include "osal/osal.h"

#include "options.h"

#include "atomics-types.h"

#include "layout-dxb.h"
#include "layout-lck.h"

#define MIN_MAPSIZE (MDBX_MIN_PAGESIZE * MIN_PAGENO)
#if IS_WINDOWS
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

#define MEGABYTE ((size_t)1 << 20)
#define GIGABYTE ((size_t)1 << 30)

/*----------------------------------------------------------------------------*/

union logger_union {
  void *ptr;
  MDBX_debug_func fmt;
  MDBX_debug_func_nofmt nofmt;
};

struct libmdbx_globals {
  bin128_t bootid;
  unsigned sys_pagesize, sys_allocation_granularity;
  size_t assume_ram_pages, mmap_limit, reasonable_db_maxsize;
#ifdef AT_UCACHEBSIZE
  unsigned sys_unified_cache_block;
#endif /* AT_UCACHEBSIZE */
  uint8_t sys_pagesize_ln2;
  uint8_t runtime_flags;
  uint8_t loglevel;
#if IS_WINDOWS
  bool running_under_Wine;
#elif defined(__linux__) || defined(__gnu_linux__)
  bool running_on_WSL1 /* Windows Subsystem 1 for Linux */;
#ifdef ENABLE_MEMCHECK
  uint8_t running_on_Valgrind;
#endif /* ENABLE_MEMCHECK */
  uint32_t linux_kernel_version;
#endif /* Linux */
  union logger_union logger;
  osal_fastmutex_t debug_lock;
  size_t logger_buffer_size;
  char *logger_buffer;
  MDBX_panic_func panic_func; /*  Callback for assertion failures */
};

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

extern struct libmdbx_globals globals;
#if IS_WINDOWS
extern struct libmdbx_imports imports;
#endif /* Windows */

static inline unsigned mdbx_running_on_Valgrind(void) {
#ifdef ENABLE_MEMCHECK
  return globals.running_on_Valgrind;
#else
  return 0;
#endif /* ENABLE_MEMCHECK */
}

/* Whether the process is running under Wine; always false on non-Windows.
 * Used to fall back to conservative behavior where Wine's Win32 emulation
 * is incomplete (e.g. LockFileEx byte-range lock semantics). */
static inline bool mdbx_running_under_Wine(void) {
#if IS_WINDOWS
  return globals.running_under_Wine;
#else
  return false;
#endif /* Windows */
}

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
