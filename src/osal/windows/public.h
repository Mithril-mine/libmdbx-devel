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

MDBX_INTERNAL int osal_mb2w(const char *const src, wchar_t **const pdst);

/*----------------------------------------------------------------------------*/

typedef union osal_srwlock {
  __anonymous_struct_extension__ struct {
    long volatile readerCount;
    long volatile writerCount;
  };
  RTL_SRWLOCK native;
} osal_srwlock_t;

typedef void(WINAPI *osal_srwlock_t_function)(osal_srwlock_t *);
typedef BOOLEAN(WINAPI *osal_srwlock_t_try_function)(osal_srwlock_t *);

#if _WIN32_WINNT < 0x0600 /* prior to Windows Vista */
typedef enum _FILE_INFO_BY_HANDLE_CLASS {
  FileBasicInfo,
  FileStandardInfo,
  FileNameInfo,
  FileRenameInfo,
  FileDispositionInfo,
  FileAllocationInfo,
  FileEndOfFileInfo,
  FileStreamInfo,
  FileCompressionInfo,
  FileAttributeTagInfo,
  FileIdBothDirectoryInfo,
  FileIdBothDirectoryRestartInfo,
  FileIoPriorityHintInfo,
  FileRemoteProtocolInfo,
  MaximumFileInfoByHandleClass
} FILE_INFO_BY_HANDLE_CLASS,
  *PFILE_INFO_BY_HANDLE_CLASS;

typedef struct _FILE_END_OF_FILE_INFO {
  LARGE_INTEGER EndOfFile;
} FILE_END_OF_FILE_INFO, *PFILE_END_OF_FILE_INFO;

#define REMOTE_PROTOCOL_INFO_FLAG_LOOPBACK 0x00000001
#define REMOTE_PROTOCOL_INFO_FLAG_OFFLINE 0x00000002

typedef struct _FILE_REMOTE_PROTOCOL_INFO {
  USHORT StructureVersion;
  USHORT StructureSize;
  DWORD Protocol;
  USHORT ProtocolMajorVersion;
  USHORT ProtocolMinorVersion;
  USHORT ProtocolRevision;
  USHORT Reserved;
  DWORD Flags;
  struct {
    DWORD Reserved[8];
  } GenericReserved;
  struct {
    DWORD Reserved[16];
  } ProtocolSpecificReserved;
} FILE_REMOTE_PROTOCOL_INFO, *PFILE_REMOTE_PROTOCOL_INFO;

#endif /* _WIN32_WINNT < 0x0600 (prior to Windows Vista) */

typedef BOOL(WINAPI *MDBX_GetFileInformationByHandleEx)(_In_ HANDLE hFile,
                                                        _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
                                                        _Out_ LPVOID lpFileInformation, _In_ DWORD dwBufferSize);

typedef BOOL(WINAPI *MDBX_GetVolumeInformationByHandleW)(
  _In_ HANDLE hFile, _Out_opt_ LPWSTR lpVolumeNameBuffer, _In_ DWORD nVolumeNameSize,
  _Out_opt_ LPDWORD lpVolumeSerialNumber, _Out_opt_ LPDWORD lpMaximumComponentLength,
  _Out_opt_ LPDWORD lpFileSystemFlags, _Out_opt_ LPWSTR lpFileSystemNameBuffer, _In_ DWORD nFileSystemNameSize);

typedef DWORD(WINAPI *MDBX_GetFinalPathNameByHandleW)(_In_ HANDLE hFile, _Out_ LPWSTR lpszFilePath,
                                                      _In_ DWORD cchFilePath, _In_ DWORD dwFlags);

typedef BOOL(WINAPI *MDBX_SetFileInformationByHandle)(_In_ HANDLE hFile,
                                                      _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
                                                      _Out_ LPVOID lpFileInformation, _In_ DWORD dwBufferSize);

typedef NTSTATUS(NTAPI *MDBX_NtFsControlFile)(IN HANDLE FileHandle, IN OUT HANDLE Event,
                                              IN OUT PVOID /* PIO_APC_ROUTINE */ ApcRoutine, IN OUT PVOID ApcContext,
                                              OUT PIO_STATUS_BLOCK IoStatusBlock, IN ULONG FsControlCode,
                                              IN OUT PVOID InputBuffer, IN ULONG InputBufferLength,
                                              OUT OPTIONAL PVOID OutputBuffer, IN ULONG OutputBufferLength);

typedef uint64_t(WINAPI *MDBX_GetTickCount64)(void);

#if !defined(_WIN32_WINNT_WIN8) || _WIN32_WINNT < _WIN32_WINNT_WIN8
typedef struct _WIN32_MEMORY_RANGE_ENTRY {
  PVOID VirtualAddress;
  SIZE_T NumberOfBytes;
} WIN32_MEMORY_RANGE_ENTRY, *PWIN32_MEMORY_RANGE_ENTRY;
#endif /* Windows 8.x */

typedef BOOL(WINAPI *MDBX_PrefetchVirtualMemory)(HANDLE hProcess, ULONG_PTR NumberOfEntries,
                                                 PWIN32_MEMORY_RANGE_ENTRY VirtualAddresses, ULONG Flags);

typedef enum _SECTION_INHERIT { ViewShare = 1, ViewUnmap = 2 } SECTION_INHERIT;

typedef NTSTATUS(NTAPI *MDBX_NtExtendSection)(IN HANDLE SectionHandle, IN PLARGE_INTEGER NewSectionSize);

typedef LSTATUS(WINAPI *MDBX_RegGetValueA)(HKEY hkey, LPCSTR lpSubKey, LPCSTR lpValue, DWORD dwFlags, LPDWORD pdwType,
                                           PVOID pvData, LPDWORD pcbData);

typedef long(WINAPI *MDBX_CoCreateGuid)(bin128_t *guid);

NTSYSAPI ULONG RtlRandomEx(PULONG Seed);

typedef BOOL(WINAPI *MDBX_SetFileIoOverlappedRange)(HANDLE FileHandle, PUCHAR OverlappedRangeStart, ULONG Length);

struct libmdbx_imports {
  osal_srwlock_t_function srwl_Init;
  osal_srwlock_t_function srwl_AcquireShared;
  osal_srwlock_t_function srwl_ReleaseShared;
  osal_srwlock_t_function srwl_AcquireExclusive;
  osal_srwlock_t_function srwl_ReleaseExclusive;
  osal_srwlock_t_try_function srwl_TryAcquireExclusive;
  MDBX_NtExtendSection NtExtendSection;
  MDBX_GetFileInformationByHandleEx GetFileInformationByHandleEx;
  MDBX_GetVolumeInformationByHandleW GetVolumeInformationByHandleW;
  MDBX_GetFinalPathNameByHandleW GetFinalPathNameByHandleW;
  MDBX_SetFileInformationByHandle SetFileInformationByHandle;
  MDBX_NtFsControlFile NtFsControlFile;
  MDBX_PrefetchVirtualMemory PrefetchVirtualMemory;
  MDBX_GetTickCount64 GetTickCount64;
  MDBX_RegGetValueA RegGetValueA;
  MDBX_SetFileIoOverlappedRange SetFileIoOverlappedRange;
  MDBX_CoCreateGuid CoCreateGuid;
};

#ifndef __cplusplus
MDBX_INTERNAL void windows_import(void);
#endif /* !__cplusplus */
