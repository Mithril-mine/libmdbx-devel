/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-mmap.c (Windows part) — memory mapping: mmap/munmap/mresize/msync
/// and filesystem checks for a mapped device.

#include "../../internals.h"
#include "osal/windows/private.h"
#include "../details.h"

/* We use native NT APIs to setup the memory map, so that we can
 * let the DB file grow incrementally instead of always preallocating
 * the full size. These APIs are defined in <wdm.h> and <ntifs.h>
 * but those headers are meant for driver-level development and
 * conflict with the regular user-level headers, so we explicitly
 * declare them here. Using these APIs also means we must link to
 * ntdll.dll, which is not linked by default in user code. */

extern NTSTATUS NTAPI NtCreateSection(OUT PHANDLE SectionHandle, IN ACCESS_MASK DesiredAccess,
                                      IN OPTIONAL POBJECT_ATTRIBUTES ObjectAttributes,
                                      IN OPTIONAL PLARGE_INTEGER MaximumSize, IN ULONG SectionPageProtection,
                                      IN ULONG AllocationAttributes, IN OPTIONAL HANDLE FileHandle);

extern NTSTATUS NTAPI NtMapViewOfSection(IN HANDLE SectionHandle, IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress,
                                         IN ULONG_PTR ZeroBits, IN SIZE_T CommitSize,
                                         IN OUT OPTIONAL PLARGE_INTEGER SectionOffset, IN OUT PSIZE_T ViewSize,
                                         IN SECTION_INHERIT InheritDisposition, IN ULONG AllocationType,
                                         IN ULONG Win32Protect);

extern NTSTATUS NTAPI NtUnmapViewOfSection(IN HANDLE ProcessHandle, IN OPTIONAL PVOID BaseAddress);

/* Zap: Inconsistent annotation for 'NtClose'... */
MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(28251)
extern NTSTATUS NTAPI NtClose(HANDLE Handle);

extern NTSTATUS NTAPI NtAllocateVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN ULONG_PTR ZeroBits,
                                              IN OUT PSIZE_T RegionSize, IN ULONG AllocationType, IN ULONG Protect);

extern NTSTATUS NTAPI NtFreeVirtualMemory(IN HANDLE ProcessHandle, IN PVOID *BaseAddress, IN OUT PSIZE_T RegionSize,
                                          IN ULONG FreeType);

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

/*----------------------------------------------------------------------------*/

int osal_msync(const osal_mmap_t *map, size_t length, enum osal_syncmode_bits mode_bits) {
  if (!MDBX_MMAP_NEEDS_JOLT && mode_bits == MDBX_SYNC_NONE)
    return MDBX_SUCCESS;

  if (!FlushViewOfFile(map->base, length))
    return (int)GetLastError();
  if ((mode_bits & (MDBX_SYNC_DATA | MDBX_SYNC_IODQ)) && !FlushFileBuffers(map->fd))
    return (int)GetLastError();
  return MDBX_SUCCESS;
}

int osal_mmap(const int flags, osal_mmap_t *map, size_t size, const size_t limit, const unsigned options,
              const pathchar_t *pathname4logging) {
  ASSERT(size <= limit);
  map->limit = 0;
  map->current = 0;
  map->base = nullptr;
  map->filesize = 0;
  map->section = nullptr;

  int err = osal_check_fs_local(map->fd, flags);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (globals.running_under_Wine)
      NOTICE("%s", "Please use native Linux application or WSL at least, instead of trouble-full Wine!");
    switch (err) {
    case MDBX_RESULT_TRUE:
#if MDBX_ENABLE_NON_READONLY_EXPORT
      WARNING("%" MDBX_PRIsPATH " is exported via NFS, avoid using the file on a remote side!", pathname4logging);
      break;
#else
      ERROR("%" MDBX_PRIsPATH " is exported via NFS", pathname4logging);
      return MDBX_EREMOTE;
#endif /* MDBX_PROHIBIT_NON_READONLY_EXPORT */
    case MDBX_EREMOTE:
      ERROR("%" MDBX_PRIsPATH " is on a remote file system, the %s is required", pathname4logging, "MDBX_EXCLUSIVE");
      __fallthrough /* fall through */;
    default:
      return LOG_IFERR(err);
    }
  }

  err = check_mmap_limit(limit);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if ((flags & MDBX_RDONLY) == 0 && (options & MMAP_OPTION_SETLENGTH) != 0) {
    err = osal_fsetsize(map->fd, size);
    VERBOSE("osal_fsetsize %zu, err %d", size, err);
    if (err != MDBX_SUCCESS)
      return LOG_IFERR(err);
    map->filesize = size;
  } else {
    err = osal_filesize(map->fd, &map->filesize);
    VERBOSE("filesize %" PRIu64 ", err %d", map->filesize, err);
    if (err != MDBX_SUCCESS)
      return LOG_IFERR(err);
    if (map->filesize < size) {
      WARNING("file size (%zu) less than requested for mapping (%zu)", (size_t)map->filesize, size);
      size = (size_t)map->filesize;
    }
  }

  LARGE_INTEGER SectionSize;
  SectionSize.QuadPart = size;
  err = NtCreateSection(&map->section,
                        /* DesiredAccess */
                        (flags & MDBX_WRITEMAP)
                            ? SECTION_QUERY | SECTION_MAP_READ | SECTION_EXTEND_SIZE | SECTION_MAP_WRITE
                            : SECTION_QUERY | SECTION_MAP_READ | SECTION_EXTEND_SIZE,
                        /* ObjectAttributes */ nullptr,
                        /* MaximumSize (InitialSize) */ &SectionSize,
                        /* SectionPageProtection */
                        (flags & MDBX_RDONLY) ? PAGE_READONLY : PAGE_READWRITE,
                        /* AllocationAttributes */ SEC_RESERVE, map->fd);
  if (!NT_SUCCESS(err))
    return osal_ntstatus2errcode(err);

  SIZE_T ViewSize = (flags & MDBX_RDONLY) ? 0 : globals.running_under_Wine ? size : limit;
  err = NtMapViewOfSection(map->section, GetCurrentProcess(), &map->base,
                           /* ZeroBits */ 0,
                           /* CommitSize */ 0,
                           /* SectionOffset */ nullptr, &ViewSize,
                           /* InheritDisposition */ ViewUnmap,
                           /* AllocationType */ (flags & MDBX_RDONLY) ? 0 : MEM_RESERVE,
                           /* Win32Protect */
                           (flags & MDBX_WRITEMAP) ? PAGE_READWRITE : PAGE_READONLY);
  if (!NT_SUCCESS(err)) {
    NtClose(map->section);
    map->section = 0;
    map->base = nullptr;
    return osal_ntstatus2errcode(err);
  }
  ASSERT(map->base != MAP_FAILED);

  map->current = (size_t)SectionSize.QuadPart;
  map->limit = ViewSize;

  VALGRIND_MAKE_MEM_DEFINED(map->base, map->current);
  MDBX_ASAN_UNPOISON_MEMORY_REGION(map->base, map->current);
  return MDBX_SUCCESS;
}

void osal_munmap(osal_mmap_t *map) {
  VALGRIND_MAKE_MEM_NOACCESS(map->base, map->current);
  /* Unpoisoning is required for ASAN to avoid false-positive diagnostic
   * when this memory will re-used by malloc or another mmapping.
   * See https://libmdbx.dqdkfa.ru/dead-github/pull/93#issuecomment-613687203 */
  MDBX_ASAN_UNPOISON_MEMORY_REGION(map->base,
                                   (map->filesize && map->filesize < map->limit) ? map->filesize : map->limit);
  if (map->section) {
    NtClose(map->section);
    map->section = 0;
  }
  NTSTATUS err = NtUnmapViewOfSection(GetCurrentProcess(), map->base);
  if (!NT_SUCCESS(err))
    ERROR("Unexpected NtUnmapViewOfSection(%p, %zi) error %d", map->base, map->limit, osal_ntstatus2errcode(err));

  map->limit = 0;
  map->current = 0;
  map->base = nullptr;
}

int osal_mresize(const int flags, osal_mmap_t *map, size_t size, size_t limit) {
  int rc = osal_filesize(map->fd, &map->filesize);
  VERBOSE("flags 0x%x, size %zu, limit %zu, filesize %" PRIu64, flags, size, limit, map->filesize);
  ASSERT(size <= limit);
  if (rc != MDBX_SUCCESS) {
    map->filesize = 0;
    return rc;
  }

  ASSERT(size != map->current || limit != map->limit || size < map->filesize);

  NTSTATUS status;
  LARGE_INTEGER SectionSize;
  int err;

  if (limit == map->limit && size > map->current) {
    if ((flags & MDBX_RDONLY) && map->filesize >= size) {
      map->current = size;
      return MDBX_SUCCESS;
    } else if (!(flags & MDBX_RDONLY) &&
               /* workaround for Wine */ imports.NtExtendSection) {
      /* growth rw-section */
      SectionSize.QuadPart = size;
      status = imports.NtExtendSection(map->section, &SectionSize);
      if (!NT_SUCCESS(status))
        return osal_ntstatus2errcode(status);
      map->current = size;
      if (map->filesize < size)
        map->filesize = size;
      return MDBX_SUCCESS;
    }
  }

  if (limit > map->limit) {
    err = check_mmap_limit(limit);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    /* check ability of address space for growth before unmap */
    PVOID BaseAddress = (PBYTE)map->base + map->limit;
    SIZE_T RegionSize = limit - map->limit;
    status = NtAllocateVirtualMemory(GetCurrentProcess(), &BaseAddress, 0, &RegionSize, MEM_RESERVE, PAGE_NOACCESS);
    if (status == (NTSTATUS) /* STATUS_CONFLICTING_ADDRESSES */ 0xC0000018)
      return MDBX_UNABLE_EXTEND_MAPSIZE;
    if (!NT_SUCCESS(status))
      return osal_ntstatus2errcode(status);

    status = NtFreeVirtualMemory(GetCurrentProcess(), &BaseAddress, &RegionSize, MEM_RELEASE);
    if (!NT_SUCCESS(status))
      return osal_ntstatus2errcode(status);
  }

  /* Windows unable:
   *  - shrink a mapped file;
   *  - change size of mapped view;
   *  - extend read-only mapping;
   * Therefore we should unmap/map entire section. */
  if ((flags & MDBX_MRESIZE_MAY_UNMAP) == 0) {
    if (size <= map->current && limit == map->limit)
      return MDBX_SUCCESS;
    /* MDBX_EPERM here means "the requested resize cannot be done without
     * un-mapping, and un-mapping is not permitted in this context" — i.e. an
     * OS/system limitation, NOT a shortage of resources (address space
     * exhaustion is reported separately as MDBX_UNABLE_EXTEND_MAPSIZE, and a
     * deferred shrink/remap caused by readers holding the remap-lock shared is
     * decided by the caller in dxb_resize(); see issue #154). */
    return MDBX_EPERM;
  }

  /* Unpoisoning is required for ASAN to avoid false-positive diagnostic
   * when this memory will re-used by malloc or another mmapping.
   * See https://libmdbx.dqdkfa.ru/dead-github/pull/93#issuecomment-613687203 */
  MDBX_ASAN_UNPOISON_MEMORY_REGION(map->base, map->limit);
  status = NtUnmapViewOfSection(GetCurrentProcess(), map->base);
  if (!NT_SUCCESS(status))
    return osal_ntstatus2errcode(status);
  /* close and zeroed old section handle */
  status = NtClose(map->section);
  map->section = nullptr;
  PVOID ReservedAddress = nullptr;
  SIZE_T ReservedSize = limit;

  if (!NT_SUCCESS(status)) {
  bailout_ntstatus:
    err = osal_ntstatus2errcode(status);
    map->base = nullptr;
    map->current = map->limit = 0;
    if (map->section) {
      NtClose(map->section);
      map->section = nullptr;
    }
    if (ReservedAddress) {
      ReservedSize = 0;
      status = NtFreeVirtualMemory(GetCurrentProcess(), &ReservedAddress, &ReservedSize, MEM_RELEASE);
      ASSERT(NT_SUCCESS(status));
      (void)status;
    }
    return err;
  }

retry_file_and_section:
  /* resizing of the file may take a while,
   * therefore we reserve address space to avoid occupy it by other threads */
  ReservedAddress = map->base;
  status = NtAllocateVirtualMemory(GetCurrentProcess(), &ReservedAddress, 0, &ReservedSize, MEM_RESERVE, PAGE_NOACCESS);
  if (!NT_SUCCESS(status)) {
    ReservedAddress = nullptr;
    if (status != (NTSTATUS) /* STATUS_CONFLICTING_ADDRESSES */ 0xC0000018)
      goto bailout_ntstatus /* no way to recovery */;

    if (flags & MDBX_MRESIZE_MAY_MOVE)
      /* the base address could be changed */
      map->base = nullptr;
  }

  if ((flags & MDBX_RDONLY) == 0 && map->filesize != size) {
    err = osal_fsetsize(map->fd, size);
    if (err == MDBX_SUCCESS)
      map->filesize = size;
    /* ignore error, because Windows unable shrink file
     * that already mapped (by another process) */
  }

  SectionSize.QuadPart = size;
  status = NtCreateSection(&map->section,
                           /* DesiredAccess */
                           (flags & MDBX_WRITEMAP)
                               ? SECTION_QUERY | SECTION_MAP_READ | SECTION_EXTEND_SIZE | SECTION_MAP_WRITE
                               : SECTION_QUERY | SECTION_MAP_READ | SECTION_EXTEND_SIZE,
                           /* ObjectAttributes */ nullptr,
                           /* MaximumSize (InitialSize) */ &SectionSize,
                           /* SectionPageProtection */
                           (flags & MDBX_RDONLY) ? PAGE_READONLY : PAGE_READWRITE,
                           /* AllocationAttributes */ SEC_RESERVE, map->fd);

  if (!NT_SUCCESS(status))
    goto bailout_ntstatus;

  if (ReservedAddress) {
    /* release reserved address space */
    ReservedSize = 0;
    status = NtFreeVirtualMemory(GetCurrentProcess(), &ReservedAddress, &ReservedSize, MEM_RELEASE);
    ReservedAddress = nullptr;
    if (!NT_SUCCESS(status))
      goto bailout_ntstatus;
  }

retry_mapview:;
  SIZE_T ViewSize = (flags & MDBX_RDONLY) ? size : limit;
  status = NtMapViewOfSection(map->section, GetCurrentProcess(), &map->base,
                              /* ZeroBits */ 0,
                              /* CommitSize */ 0,
                              /* SectionOffset */ nullptr, &ViewSize,
                              /* InheritDisposition */ ViewUnmap,
                              /* AllocationType */ (flags & MDBX_RDONLY) ? 0 : MEM_RESERVE,
                              /* Win32Protect */
                              (flags & MDBX_WRITEMAP) ? PAGE_READWRITE : PAGE_READONLY);

  if (!NT_SUCCESS(status)) {
    if (status == (NTSTATUS) /* STATUS_CONFLICTING_ADDRESSES */ 0xC0000018 && map->base &&
        (flags & MDBX_MRESIZE_MAY_MOVE) != 0) {
      /* try remap at another base address */
      map->base = nullptr;
      goto retry_mapview;
    }
    NtClose(map->section);
    map->section = nullptr;

    if (map->base && (size != map->current || limit != map->limit)) {
      /* try remap with previously size and limit,
       * but will return MDBX_UNABLE_EXTEND_MAPSIZE on success */
      rc = (limit > map->limit) ? MDBX_UNABLE_EXTEND_MAPSIZE : MDBX_EPERM;
      size = map->current;
      ReservedSize = limit = map->limit;
      goto retry_file_and_section;
    }

    /* no way to recovery */
    goto bailout_ntstatus;
  }
  ASSERT(map->base != MAP_FAILED);

  map->current = (size_t)SectionSize.QuadPart;
  map->limit = ViewSize;

  MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6287);
  ASSERT(rc != MDBX_SUCCESS || (map->base != nullptr && map->base != MAP_FAILED && map->current == size &&
                                map->limit == limit && map->filesize >= size));
  return rc;
}
