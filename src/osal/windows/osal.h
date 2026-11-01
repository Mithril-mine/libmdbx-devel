/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file osal.h (Windows part) — platform-specific types and macros of the
/// OSAL layer. Included by osal/osal.h; not for direct inclusion elsewhere.

#pragma once

#define HAVE_SYS_STAT_H
#define HAVE_SYS_TYPES_H
typedef HANDLE osal_thread_t;
typedef unsigned osal_thread_key_t;
#define MAP_FAILED nullptr
#define HIGH_DWORD(v) ((DWORD)((sizeof(v) > 4) ? ((uint64_t)(v) >> 32) : 0))
#define THREAD_CALL WINAPI
#define THREAD_RESULT DWORD
typedef struct {
  HANDLE mutex;
  HANDLE event[2];
} osal_condpair_t;
typedef CRITICAL_SECTION osal_fastmutex_t;

#if MDBX_WITHOUT_MSVC_CRT

#ifndef osal_malloc
static inline void *osal_malloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes); }
#endif /* osal_malloc */

#ifndef osal_calloc
static inline void *osal_calloc(size_t nelem, size_t size) {
  return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, nelem * size);
}
#endif /* osal_calloc */

#ifndef osal_realloc
static inline void *osal_realloc(void *ptr, size_t bytes) {
  return ptr ? HeapReAlloc(GetProcessHeap(), 0, ptr, bytes) : HeapAlloc(GetProcessHeap(), 0, bytes);
}
#endif /* osal_realloc */

#ifndef osal_free
static inline void osal_free(void *ptr) { HeapFree(GetProcessHeap(), 0, ptr); }
#endif /* osal_free */

#define osal_alloca(size) _alloca(size)

/*> dist-cutoff-begin */
#if defined(MDBX_PROBES)
/* Concrete platform allocators used by the probe-bus itself (avoids infinite
 * recursion through the osal_* redirect defined below). */
#define osal_malloc_raw(bytes) HeapAlloc(GetProcessHeap(), 0, (bytes))
#define osal_calloc_raw(nelem, size) HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (nelem) * (size))
#define osal_realloc_raw(ptr, bytes)                                                                                   \
  ((ptr) ? HeapReAlloc(GetProcessHeap(), 0, (ptr), (bytes)) : HeapAlloc(GetProcessHeap(), 0, (bytes)))
#define osal_free_raw(ptr) HeapFree(GetProcessHeap(), 0, (ptr))
#endif /* MDBX_PROBES */
/*< dist-cutoff-end */

#else /* MDBX_WITHOUT_MSVC_CRT */

#define osal_malloc malloc
#define osal_calloc calloc
#define osal_realloc realloc
#define osal_free free
#define osal_strdup _strdup

/*> dist-cutoff-begin */
#if defined(MDBX_PROBES)
#define osal_malloc_raw malloc
#define osal_calloc_raw calloc
#define osal_realloc_raw realloc
#define osal_free_raw free
#endif /* MDBX_PROBES */
/*< dist-cutoff-end */

#endif /* MDBX_WITHOUT_MSVC_CRT */

#ifndef snprintf
#define snprintf _snprintf /* ntdll */
#endif

#ifndef vsnprintf
#define vsnprintf _vsnprintf /* ntdll */
#endif

#ifndef strcasecmp
#define strcasecmp _stricmp /* ntdll */
#endif

#ifndef strncasecmp
#define strncasecmp _strnicmp /* ntdll */
#endif

typedef wchar_t pathchar_t;
#define MDBX_PRIsPATH "ls"

/* clock-related state initialized by osal_ctor(), used by osal-misc (monotime) */
extern LARGE_INTEGER performance_frequency;
extern uint64_t monotime_limit;
/* boot-id of the current OS instance (see osal-bootid.c) */
__cold bin128_t osal_bootid(void);

/* WOF/compressed-volume and NTSTATUS declarations used by osal-mmap.c and osal-fs.c */
#ifndef WOF_CURRENT_VERSION
typedef struct _WOF_EXTERNAL_INFO {
  DWORD Version;
  DWORD Provider;
} WOF_EXTERNAL_INFO, *PWOF_EXTERNAL_INFO;
#endif /* WOF_CURRENT_VERSION */

#ifndef WIM_PROVIDER_CURRENT_VERSION
#define WIM_PROVIDER_HASH_SIZE 20

typedef struct _WIM_PROVIDER_EXTERNAL_INFO {
  DWORD Version;
  DWORD Flags;
  LARGE_INTEGER DataSourceId;
  BYTE ResourceHash[WIM_PROVIDER_HASH_SIZE];
} WIM_PROVIDER_EXTERNAL_INFO, *PWIM_PROVIDER_EXTERNAL_INFO;
#endif /* WIM_PROVIDER_CURRENT_VERSION */

#ifndef FILE_PROVIDER_CURRENT_VERSION
typedef struct _FILE_PROVIDER_EXTERNAL_INFO_V1 {
  ULONG Version;
  ULONG Algorithm;
  ULONG Flags;
} FILE_PROVIDER_EXTERNAL_INFO_V1, *PFILE_PROVIDER_EXTERNAL_INFO_V1;
#endif /* FILE_PROVIDER_CURRENT_VERSION */

#ifndef STATUS_OBJECT_NOT_EXTERNALLY_BACKED
#define STATUS_OBJECT_NOT_EXTERNALLY_BACKED ((NTSTATUS)0xC000046DL)
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif

#ifndef FILE_DEVICE_FILE_SYSTEM
#define FILE_DEVICE_FILE_SYSTEM 0x00000009
#endif

#ifndef FSCTL_GET_EXTERNAL_BACKING
#define FSCTL_GET_EXTERNAL_BACKING CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 196, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef ERROR_NOT_CAPABLE
#define ERROR_NOT_CAPABLE 775L
#endif
/* NTSTATUS -> errno mapping (see osal-misc.c) */
__cold MDBX_INTERNAL int osal_ntstatus2errcode(NTSTATUS status);
