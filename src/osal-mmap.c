/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-mmap.c — memory mapping: mmap/munmap/mresize/msync and
/// filesystem checks for a mapped device.

#include "internals.h"

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

int osal_msync(const osal_mmap_t *map, size_t length, enum osal_syncmode_bits mode_bits) {
  if (!MDBX_MMAP_NEEDS_JOLT && mode_bits == MDBX_SYNC_NONE)
    return MDBX_SUCCESS;

#if IS_WINDOWS
  if (!FlushViewOfFile(map->base, length))
    return (int)GetLastError();
  if ((mode_bits & (MDBX_SYNC_DATA | MDBX_SYNC_IODQ)) && !FlushFileBuffers(map->fd))
    return (int)GetLastError();
#else
#if defined(__linux__) || defined(__gnu_linux__)
    /* Since Linux 2.6.19, MS_ASYNC is in fact a no-op. The kernel properly
     * tracks dirty pages and flushes ones as necessary. */
    //
    // However, this behavior may be changed in custom kernels,
    // so just leave such optimization to the libc discretion.
    // NOTE: The MDBX_MMAP_NEEDS_JOLT must be defined to 1 for such cases.
    //
    // ASSERT(mdbx.linux_kernel_version > 0x02061300);
    // if (mode_bits <= MDBX_SYNC_KICK)
    //   return MDBX_SUCCESS;
#endif /* Linux */
  if (msync(map->base, length, (mode_bits & MDBX_SYNC_DATA) ? MS_SYNC : MS_ASYNC))
    return errno;
  if ((mode_bits & MDBX_SYNC_SIZE) && fsync(map->fd))
    return errno;
#endif
  return MDBX_SUCCESS;
}

int osal_check_fs_rdonly(mdbx_filehandle_t handle, const pathchar_t *pathname, int err) {
#if IS_WINDOWS
  (void)pathname;
  (void)err;
  if (!imports.GetVolumeInformationByHandleW)
    return MDBX_ENOSYS;
  DWORD unused, flags;
  if (!imports.GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, &unused, &flags, nullptr, 0))
    return (int)GetLastError();
  if ((flags & FILE_READ_ONLY_VOLUME) == 0)
    return MDBX_EACCESS;
#else
  struct statvfs info;
  if (err != MDBX_ENOFILE) {
    if (statvfs(pathname, &info) == 0)
      return (info.f_flag & ST_RDONLY) ? MDBX_SUCCESS : err;
    if (errno != MDBX_ENOFILE)
      return errno;
  }
  if (fstatvfs(handle, &info))
    return errno;
  if ((info.f_flag & ST_RDONLY) == 0)
    return (err == MDBX_ENOFILE) ? MDBX_EACCESS : err;
#endif /* !Windows */
  return MDBX_SUCCESS;
}

int osal_check_fs_incore(mdbx_filehandle_t handle) {
#if IS_WINDOWS
  (void)handle;
#else
  struct statfs statfs_info;
  if (fstatfs(handle, &statfs_info))
    return errno;

#if defined(__OpenBSD__)
  const unsigned type = 0;
#else
  const unsigned type = statfs_info.f_type;
#endif
  switch (type) {
  case 0x28cd3d45 /* CRAMFS_MAGIC */:
  case 0x858458f6 /* RAMFS_MAGIC */:
  case 0x01021994 /* TMPFS_MAGIC */:
  case 0x73717368 /* SQUASHFS_MAGIC */:
  case 0x7275 /* ROMFS_MAGIC */:
    return MDBX_RESULT_TRUE;
  }

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__BSD__) || defined(__bsdi__) ||    \
    defined(__DragonFly__) || defined(__APPLE__) || defined(__MACH__) || defined(MFSNAMELEN) ||                        \
    defined(MFSTYPENAMELEN) || defined(VFS_NAMELEN)
  const char *const name = statfs_info.f_fstypename;
  const size_t name_len = sizeof(statfs_info.f_fstypename);
#else
  const char *const name = "";
  const size_t name_len = 0;
#endif
  if (name_len) {
    if (strncasecmp("tmpfs", name, 6) == 0 || strncasecmp("mfs", name, 4) == 0 || strncasecmp("ramfs", name, 6) == 0 ||
        strncasecmp("romfs", name, 6) == 0)
      return MDBX_RESULT_TRUE;
  }
#endif /* !Windows */

  return MDBX_RESULT_FALSE;
}

int osal_check_fs_local(mdbx_filehandle_t handle, int flags) {
#if IS_WINDOWS
  if (globals.running_under_Wine && !(flags & MDBX_EXCLUSIVE))
    return ERROR_NOT_CAPABLE /* workaround for Wine */;

  if (GetFileType(handle) != FILE_TYPE_DISK)
    return ERROR_FILE_OFFLINE;

  static const char msg_note[] = "To avoid DB corruption, data loss please, performance degradation, BSOD or resources "
                                 "leaks in the OS avoid use database on a";
  if (imports.GetFileInformationByHandleEx) {
    FILE_REMOTE_PROTOCOL_INFO RemoteProtocolInfo;
    if (imports.GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &RemoteProtocolInfo,
                                             sizeof(RemoteProtocolInfo))) {
      if ((RemoteProtocolInfo.Flags & REMOTE_PROTOCOL_INFO_FLAG_OFFLINE) && !(flags & MDBX_RDONLY)) {
        ERROR("%s remote or shared volumes: remote protocol id 0x%08lX, version %u.%u.%u, flags 0x%lx", msg_note,
              RemoteProtocolInfo.Protocol, RemoteProtocolInfo.ProtocolMajorVersion,
              RemoteProtocolInfo.ProtocolMajorVersion, RemoteProtocolInfo.ProtocolRevision,
              (unsigned long)RemoteProtocolInfo.Flags);
        return ERROR_FILE_OFFLINE;
      }
      if (!(RemoteProtocolInfo.Flags & REMOTE_PROTOCOL_INFO_FLAG_LOOPBACK) ||
          !(F_ISSET(flags, MDBX_EXCLUSIVE | MDBX_RDONLY) || F_ISSET(flags, MDBX_EXCLUSIVE | MDBX_UTTERLY_NOSYNC))) {
        ERROR("%s remote or shared volumes: remote protocol id 0x%08lX, version %u.%u.%u, flags 0x%lx", msg_note,
              RemoteProtocolInfo.Protocol, RemoteProtocolInfo.ProtocolMajorVersion,
              RemoteProtocolInfo.ProtocolMajorVersion, RemoteProtocolInfo.ProtocolRevision,
              (unsigned long)RemoteProtocolInfo.Flags);
        return MDBX_EREMOTE;
      }
      WARNING("%s remote or shared volumes: remote protocol id 0x%08lX, version %u.%u.%u, flags 0x%lx", msg_note,
              RemoteProtocolInfo.Protocol, RemoteProtocolInfo.ProtocolMajorVersion,
              RemoteProtocolInfo.ProtocolMajorVersion, RemoteProtocolInfo.ProtocolRevision,
              (unsigned long)RemoteProtocolInfo.Flags);
    }
  }

  if (imports.NtFsControlFile) {
    NTSTATUS rc;
    struct {
      WOF_EXTERNAL_INFO wof_info;
      union {
        WIM_PROVIDER_EXTERNAL_INFO wim_info;
        FILE_PROVIDER_EXTERNAL_INFO_V1 file_info;
      };
      size_t reserved_for_microsoft_madness[42];
    } GetExternalBacking_OutputBuffer;
    IO_STATUS_BLOCK StatusBlock;
    rc = imports.NtFsControlFile(handle, nullptr, nullptr, nullptr, &StatusBlock, FSCTL_GET_EXTERNAL_BACKING, nullptr,
                                 0, &GetExternalBacking_OutputBuffer, sizeof(GetExternalBacking_OutputBuffer));
    if (NT_SUCCESS(rc)) {
      if (!F_ISSET(flags, MDBX_EXCLUSIVE | MDBX_RDONLY) && !F_ISSET(flags, MDBX_EXCLUSIVE | MDBX_UTTERLY_NOSYNC)) {
        ERROR("%s compressed/WOF-enabled or layered volumes: WOF-version %lu, provider %lu", msg_note,
              (unsigned long)GetExternalBacking_OutputBuffer.wof_info.Version,
              (unsigned long)GetExternalBacking_OutputBuffer.wof_info.Provider);
        return MDBX_EREMOTE;
      }
      WARNING("%s compressed/WOF-enabled or layered volumes: WOF-version %lu, provider %lu", msg_note,
              (unsigned long)GetExternalBacking_OutputBuffer.wof_info.Version,
              (unsigned long)GetExternalBacking_OutputBuffer.wof_info.Provider);
    } else if (rc != STATUS_OBJECT_NOT_EXTERNALLY_BACKED && rc != STATUS_INVALID_DEVICE_REQUEST &&
               rc != STATUS_NOT_SUPPORTED)
      return osal_ntstatus2errcode(rc);
  }

  if (imports.GetVolumeInformationByHandleW && imports.GetFinalPathNameByHandleW) {
    WCHAR *PathBuffer = osal_malloc(sizeof(WCHAR) * INT16_MAX);
    if (!PathBuffer)
      return MDBX_ENOMEM;

    int rc = MDBX_SUCCESS;
    DWORD VolumeSerialNumber, FileSystemFlags;
    if (!imports.GetVolumeInformationByHandleW(handle, PathBuffer, INT16_MAX, &VolumeSerialNumber, nullptr,
                                               &FileSystemFlags, nullptr, 0)) {
      rc = (int)GetLastError();
      goto bailout;
    }

    if ((flags & MDBX_RDONLY) == 0) {
      if (FileSystemFlags & (FILE_SEQUENTIAL_WRITE_ONCE | FILE_READ_ONLY_VOLUME | FILE_VOLUME_IS_COMPRESSED)) {
        rc = MDBX_EREMOTE;
        goto bailout;
      }
    }

    if (imports.GetFinalPathNameByHandleW(handle, PathBuffer, INT16_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_NT)) {
      if (_wcsnicmp(PathBuffer, L"\\Device\\Mup\\", 12) == 0) {
        if (!(flags & MDBX_EXCLUSIVE)) {
          rc = MDBX_EREMOTE;
          goto bailout;
        }
      }
    }

    if (F_ISSET(flags, MDBX_RDONLY | MDBX_EXCLUSIVE) && (FileSystemFlags & FILE_READ_ONLY_VOLUME)) {
      /* without-LCK (exclusive readonly) mode for DB on a read-only volume */
      goto bailout;
    }

    if (imports.GetFinalPathNameByHandleW(handle, PathBuffer, INT16_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)) {
      UINT DriveType = GetDriveTypeW(PathBuffer);
      if (DriveType == DRIVE_NO_ROOT_DIR && _wcsnicmp(PathBuffer, L"\\\\?\\", 4) == 0 &&
          _wcsnicmp(PathBuffer + 5, L":\\", 2) == 0) {
        PathBuffer[7] = 0;
        DriveType = GetDriveTypeW(PathBuffer + 4);
      }
      switch (DriveType) {
      case DRIVE_CDROM:
        if (flags & MDBX_RDONLY)
          break;
      // fall through
      case DRIVE_UNKNOWN:
      case DRIVE_NO_ROOT_DIR:
      case DRIVE_REMOTE:
      default:
        if (!(flags & MDBX_EXCLUSIVE))
          rc = MDBX_EREMOTE;
      // fall through
      case DRIVE_REMOVABLE:
      case DRIVE_FIXED:
      case DRIVE_RAMDISK:
        break;
      }
    }

  bailout:
    osal_free(PathBuffer);
    return rc;
  }

#else

  struct statvfs statvfs_info;
  if (fstatvfs(handle, &statvfs_info))
    return errno;
#if defined(ST_LOCAL) || defined(ST_EXPORTED)
  const unsigned long st_flags = statvfs_info.f_flag;
#endif /* ST_LOCAL || ST_EXPORTED */

#if defined(__NetBSD__)
  const unsigned type = 0;
  const char *const name = statvfs_info.f_fstypename;
  const size_t name_len = VFS_NAMELEN;
#elif defined(_AIX) || defined(__OS400__)
  const char *const name = statvfs_info.f_basetype;
  const size_t name_len = sizeof(statvfs_info.f_basetype);
  struct stat st;
  if (fstat(handle, &st))
    return errno;
  const unsigned type = st.st_vfstype;
  if ((st.st_flag & FS_REMOTE) != 0 && !(flags & MDBX_EXCLUSIVE))
    return MDBX_EREMOTE;
#elif defined(FSTYPSZ) || defined(_FSTYPSZ)
  const unsigned type = 0;
  const char *const name = statvfs_info.f_basetype;
  const size_t name_len = sizeof(statvfs_info.f_basetype);
#elif defined(__sun) || defined(__SVR4) || defined(__svr4__) || defined(ST_FSTYPSZ) || defined(_ST_FSTYPSZ)
  const unsigned type = 0;
  struct stat st;
  if (fstat(handle, &st))
    return errno;
  const char *const name = st.st_fstype;
  const size_t name_len = strlen(name);
#else
  struct statfs statfs_info;
  if (fstatfs(handle, &statfs_info))
    return errno;
#if defined(__OpenBSD__)
  const unsigned type = 0;
#else
  const unsigned type = statfs_info.f_type;
#endif
#if defined(MNT_LOCAL) || defined(MNT_EXPORTED)
  const unsigned long mnt_flags = statfs_info.f_flags;
#endif /* MNT_LOCAL || MNT_EXPORTED */
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__BSD__) || defined(__bsdi__) ||    \
    defined(__DragonFly__) || defined(__APPLE__) || defined(__MACH__) || defined(MFSNAMELEN) ||                        \
    defined(MFSTYPENAMELEN) || defined(VFS_NAMELEN)
  const char *const name = statfs_info.f_fstypename;
  const size_t name_len = sizeof(statfs_info.f_fstypename);
#elif defined(__ANDROID_API__) && __ANDROID_API__ < 21
  const char *const name = "";
  const unsigned name_len = 0;
#else

  const char *name = "";
  unsigned name_len = 0;

  struct stat st;
  if (fstat(handle, &st))
    return errno;

  char pathbuf[PATH_MAX];
  FILE *mounted = nullptr;
#if defined(__linux__) || defined(__gnu_linux__)
  mounted = setmntent("/proc/mounts", "r");
#endif /* Linux */
  if (!mounted)
    mounted = setmntent("/etc/mtab", "r");
  if (mounted) {
    const struct mntent *ent;
#if defined(_BSD_SOURCE) || defined(_SVID_SOURCE) || defined(__BIONIC__) ||                                            \
    (defined(_DEFAULT_SOURCE) && __GLIBC_PREREQ(2, 19))
    struct mntent entbuf;
    const bool should_copy = false;
    while (nullptr != (ent = getmntent_r(mounted, &entbuf, pathbuf, sizeof(pathbuf))))
#else
    const bool should_copy = true;
    while (nullptr != (ent = getmntent(mounted)))
#endif
    {
      struct stat mnt;
      if (!stat(ent->mnt_dir, &mnt) && mnt.st_dev == st.st_dev) {
        name = ent->mnt_fsname;
        if (should_copy) {
          name = strncpy(pathbuf, ent->mnt_fsname, sizeof(pathbuf) - 1);
          pathbuf[sizeof(pathbuf) - 1] = 0;
        }
        name_len = strlen(name);
        break;
      }
    }
    endmntent(mounted);
  }
#endif /* !xBSD && !Android/Bionic */
#endif

  if (name_len) {
    if (((name_len > 2 && strncasecmp("nfs", name, 3) == 0) || strncasecmp("cifs", name, name_len) == 0 ||
         strncasecmp("ncpfs", name, name_len) == 0 || strncasecmp("smbfs", name, name_len) == 0 ||
         strcasecmp("9P" /* WSL2 */, name) == 0 ||
         ((name_len > 3 && strncasecmp("fuse", name, 4) == 0) && strncasecmp("fuseblk", name, name_len) != 0)) &&
        !(flags & MDBX_EXCLUSIVE))
      return MDBX_EREMOTE;
    if (strcasecmp("ftp", name) == 0 || strcasecmp("http", name) == 0 || strcasecmp("sshfs", name) == 0)
      return MDBX_EREMOTE;
  }

#ifdef ST_LOCAL
  if ((st_flags & ST_LOCAL) == 0 && !(flags & MDBX_EXCLUSIVE))
    return MDBX_EREMOTE;
#elif defined(MNT_LOCAL)
  if ((mnt_flags & MNT_LOCAL) == 0 && !(flags & MDBX_EXCLUSIVE))
    return MDBX_EREMOTE;
#endif /* ST/MNT_LOCAL */

#ifdef ST_EXPORTED
  if ((st_flags & ST_EXPORTED) != 0 && !(flags & (MDBX_RDONLY | MDBX_EXCLUSIVE)))
    return MDBX_RESULT_TRUE;
#elif defined(MNT_EXPORTED)
  if ((mnt_flags & MNT_EXPORTED) != 0 && !(flags & (MDBX_RDONLY | MDBX_EXCLUSIVE)))
    return MDBX_RESULT_TRUE;
#endif /* ST/MNT_EXPORTED */

  switch (type) {
  case 0xFF534D42 /* CIFS_MAGIC_NUMBER */:
  case 0x6969 /* NFS_SUPER_MAGIC */:
  case 0x564c /* NCP_SUPER_MAGIC */:
  case 0x517B /* SMB_SUPER_MAGIC */:
#if defined(__digital__) || defined(__osf__) || defined(__osf)
  case 0x0E /* Tru64 NFS */:
#endif
#ifdef ST_FST_NFS
  case ST_FST_NFS:
#endif
    if ((flags & MDBX_EXCLUSIVE) == 0)
      return MDBX_EREMOTE;
  case 0:
  default:
    break;
  }
#endif /* Unix */

  return MDBX_SUCCESS;
}

static int check_mmap_limit(const size_t limit) {
  if ((RUNNING_ON_ASAN || mdbx_running_on_Valgrind()) && limit > globals.mmap_limit) {
    ERROR("%s (%zu bytes) is too large for %s (%zu)", "database upper size limit", limit,
          RUNNING_ON_ASAN ? "AddressSanitizer" : "Valgrind", globals.mmap_limit);
    return MDBX_TOO_LARGE;
  }
  return MDBX_SUCCESS;
}

int osal_mmap(const int flags, osal_mmap_t *map, size_t size, const size_t limit, const unsigned options,
              const pathchar_t *pathname4logging) {
  ASSERT(size <= limit);
  map->limit = 0;
  map->current = 0;
  map->base = nullptr;
  map->filesize = 0;
#if IS_WINDOWS
  map->section = nullptr;
#endif /* Windows */

  int err = osal_check_fs_local(map->fd, flags);
  if (unlikely(err != MDBX_SUCCESS)) {
#if IS_WINDOWS
    if (globals.running_under_Wine)
      NOTICE("%s", "Please use native Linux application or WSL at least, instead of trouble-full Wine!");
#endif /* Windows */
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
#if !IS_WINDOWS
    map->current = size;
#endif /* !Windows */
  } else {
    err = osal_filesize(map->fd, &map->filesize);
    VERBOSE("filesize %" PRIu64 ", err %d", map->filesize, err);
    if (err != MDBX_SUCCESS)
      return LOG_IFERR(err);
#if IS_WINDOWS
    if (map->filesize < size) {
      WARNING("file size (%zu) less than requested for mapping (%zu)", (size_t)map->filesize, size);
      size = (size_t)map->filesize;
    }
#else
    map->current = (map->filesize > limit) ? limit : (size_t)map->filesize;
#endif /* !Windows */
  }

#if IS_WINDOWS
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

#else /* Windows */

#ifndef MAP_TRYFIXED
#define MAP_TRYFIXED 0
#endif

#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE 0
#endif

#ifndef MAP_CONCEAL
#define MAP_CONCEAL 0
#endif

#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

  map->limit = limit;
retry:
  map->base = mmap(nullptr, map->limit, (flags & MDBX_WRITEMAP) ? PROT_READ | PROT_WRITE : PROT_READ,
                   MAP_SHARED | MAP_FILE | MAP_NORESERVE | (F_ISSET(flags, MDBX_UTTERLY_NOSYNC) ? MAP_NOSYNC : 0) |
                       ((options & MMAP_OPTION_SEMAPHORE) ? MAP_HASSEMAPHORE | MAP_NOSYNC : MAP_CONCEAL),
                   map->fd, 0);

  if (unlikely(map->base == MAP_FAILED)) {
    err = errno;
    ASSERT(err != 0);
    if ((RUNNING_ON_ASAN || mdbx_running_on_Valgrind()) && err == EINVAL && map->limit - map->current > MEGABYTE) {
      map->limit -= (map->limit - map->current) / 2;
      map->limit = max_unsigned(size, floor_powerof2(map->limit, MEGABYTE));
      goto retry;
    }
    map->limit = 0;
    map->current = 0;
    map->base = nullptr;
    return LOG_IFERR(err);
  }
  if (unlikely(map->limit < limit) && (RUNNING_ON_ASAN || mdbx_running_on_Valgrind()))
    NOTICE("Mapping size has been reduced from %zu to %zu for compatibility with %s", limit, map->limit,
           RUNNING_ON_VALGRIND ? "Valgrind" : "AddressSanitizer");

#ifdef MADV_DONTFORK
  if (unlikely(madvise(map->base, map->limit, MADV_DONTFORK) != 0)) {
    err = errno;
    ASSERT(err != 0);
    return LOG_IFERR(err);
  }
#endif /* MADV_DONTFORK */
#ifdef MADV_NOHUGEPAGE
  (void)madvise(map->base, map->limit, MADV_NOHUGEPAGE);
#endif /* MADV_NOHUGEPAGE */

#endif /* ! Windows */

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
#if IS_WINDOWS
  if (map->section) {
    NtClose(map->section);
    map->section = 0;
  }
  NTSTATUS err = NtUnmapViewOfSection(GetCurrentProcess(), map->base);
  if (!NT_SUCCESS(err))
    ERROR("Unexpected NtUnmapViewOfSection(%p, %zi) error %d", map->base, map->limit, osal_ntstatus2errcode(err));
#else
  if (unlikely(munmap(map->base, map->limit)))
    ERROR("Unexpected munmap(%p, %zi) error %d", map->base, map->limit, errno);
#endif /* ! Windows */

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

#if IS_WINDOWS
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

#else /* Windows */

  if (flags & MDBX_RDONLY) {
    if (size > map->filesize)
      rc = MDBX_UNABLE_EXTEND_MAPSIZE;
    else if (size < map->filesize && map->filesize > limit)
      rc = MDBX_EPERM;
    map->current = (map->filesize > limit) ? limit : (size_t)map->filesize;
  } else {
    if (map->filesize != size) {
      if (size > map->filesize) {
        rc = osal_fsetsize(map->fd, size);
        VERBOSE("osal_fsetsize-%s %zu, err %d", "extend", size, rc);
      } else if (flags & txn_unmap_allowed) {
        rc = osal_fsetsize(map->fd, size);
        VERBOSE("osal_fsetsize-%s %zu, err %d", "shrink", size, rc);
      }
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      map->filesize = size;
    }

    if (map->current > size) {
      /* Clearing asan's bitmask for the region which released in shrinking,
       * since:
       *  - after the shrinking we will get an exception when accessing
       *    this region and (therefore) do not need the help of ASAN.
       *  - this allows us to clear the mask only within the file size
       *    when closing the mapping. */
      MDBX_ASAN_UNPOISON_MEMORY_REGION(ptr_disp(map->base, size),
                                       ((map->current < map->limit) ? map->current : map->limit) - size);
    }
    map->current = (size < map->limit) ? size : map->limit;
  }

  if (limit == map->limit)
    return rc;

  if (limit < map->limit) {
    /* unmap an excess at end of mapping. */
    // coverity[offset_free : FALSE]
    if (unlikely(munmap(ptr_disp(map->base, limit), map->limit - limit))) {
      ASSERT(errno != 0);
      return errno;
    }
    map->limit = limit;
    return rc;
  }

  int err = check_mmap_limit(limit);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  ASSERT(limit > map->limit);
  void *ptr = MAP_FAILED;

#if (defined(__linux__) || defined(__gnu_linux__)) && defined(_GNU_SOURCE)
  ptr = mremap(map->base, map->limit, limit,
#if defined(MREMAP_MAYMOVE)
               (flags & MDBX_MRESIZE_MAY_MOVE) ? MREMAP_MAYMOVE :
#endif /* MREMAP_MAYMOVE */
                                               0);
  if (ptr == MAP_FAILED) {
    err = errno;
    ASSERT(err != 0);
    switch (err) {
    default:
      return err;
    case 0 /* paranoia */:
    case EAGAIN:
    case ENOMEM:
      return MDBX_UNABLE_EXTEND_MAPSIZE;
    case EFAULT /* MADV_DODUMP / MADV_DONTDUMP are mixed for mmap-range */:
      break;
    }
  }
#endif /* Linux & _GNU_SOURCE */

  const unsigned mmap_flags =
      MAP_CONCEAL | MAP_SHARED | MAP_FILE | MAP_NORESERVE | (F_ISSET(flags, MDBX_UTTERLY_NOSYNC) ? MAP_NOSYNC : 0);
  const unsigned mmap_prot = (flags & MDBX_WRITEMAP) ? PROT_READ | PROT_WRITE : PROT_READ;

  if (ptr == MAP_FAILED) {
    /* Try to mmap additional space beyond the end of mapping. */
    ptr = mmap(ptr_disp(map->base, map->limit), limit - map->limit, mmap_prot, mmap_flags | MAP_FIXED_NOREPLACE,
               map->fd, map->limit);
    if (ptr == ptr_disp(map->base, map->limit))
      /* успешно прилепили отображение в конец */
      ptr = map->base;
    else if (ptr != MAP_FAILED) {
      /* the desired address is busy, unmap unsuitable one */
      if (unlikely(munmap(ptr, limit - map->limit))) {
        ASSERT(errno != 0);
        return errno;
      }
      ptr = MAP_FAILED;
    } else {
      err = errno;
      ASSERT(err != 0);
      switch (err) {
      default:
        return err;
      case 0 /* paranoia */:
      case EAGAIN:
      case ENOMEM:
        return MDBX_UNABLE_EXTEND_MAPSIZE;
      case EEXIST: /* address busy */
      case EINVAL: /* kernel don't support MAP_FIXED_NOREPLACE */
        break;
      }
    }
  }

  if (ptr == MAP_FAILED) {
    /* unmap and map again whole region */
    if ((flags & MDBX_MRESIZE_MAY_UNMAP) == 0) {
      /* TODO: Perhaps here it is worth to implement suspend/resume threads
       * and perform unmap/map as like for Windows. */
      return MDBX_UNABLE_EXTEND_MAPSIZE;
    }

    if (unlikely(munmap(map->base, map->limit))) {
      ASSERT(errno != 0);
      return errno;
    }

    // coverity[pass_freed_arg : FALSE]
    ptr = mmap(map->base, limit, mmap_prot,
               (flags & MDBX_MRESIZE_MAY_MOVE) ? mmap_flags
                                               : mmap_flags | (MAP_FIXED_NOREPLACE ? MAP_FIXED_NOREPLACE : MAP_FIXED),
               map->fd, 0);
    if (MAP_FIXED_NOREPLACE != 0 && MAP_FIXED_NOREPLACE != MAP_FIXED && unlikely(ptr == MAP_FAILED) &&
        !(flags & MDBX_MRESIZE_MAY_MOVE) && errno == /* kernel don't support MAP_FIXED_NOREPLACE */ EINVAL)
      // coverity[pass_freed_arg : FALSE]
      ptr = mmap(map->base, limit, mmap_prot, mmap_flags | MAP_FIXED, map->fd, 0);

    if (unlikely(ptr == MAP_FAILED)) {
      /* try to restore prev mapping */
      // coverity[pass_freed_arg : FALSE]
      ptr = mmap(map->base, map->limit, mmap_prot,
                 (flags & MDBX_MRESIZE_MAY_MOVE) ? mmap_flags
                                                 : mmap_flags | (MAP_FIXED_NOREPLACE ? MAP_FIXED_NOREPLACE : MAP_FIXED),
                 map->fd, 0);
      if (MAP_FIXED_NOREPLACE != 0 && MAP_FIXED_NOREPLACE != MAP_FIXED && unlikely(ptr == MAP_FAILED) &&
          !(flags & MDBX_MRESIZE_MAY_MOVE) && errno == /* kernel don't support MAP_FIXED_NOREPLACE */ EINVAL)
        // coverity[pass_freed_arg : FALSE]
        ptr = mmap(map->base, map->limit, mmap_prot, mmap_flags | MAP_FIXED, map->fd, 0);
      if (unlikely(ptr == MAP_FAILED)) {
        VALGRIND_MAKE_MEM_NOACCESS(map->base, map->current);
        /* Unpoisoning is required for ASAN to avoid false-positive diagnostic
         * when this memory will re-used by malloc or another mmapping.
         * See https://libmdbx.dqdkfa.ru/dead-github/pull/93#issuecomment-613687203 */
        MDBX_ASAN_UNPOISON_MEMORY_REGION(map->base, (map->current < map->limit) ? map->current : map->limit);
        map->limit = 0;
        map->current = 0;
        map->base = nullptr;
        ASSERT(errno != 0);
        return errno;
      }
      rc = MDBX_UNABLE_EXTEND_MAPSIZE;
      limit = map->limit;
    }
  }

  ASSERT(ptr && ptr != MAP_FAILED);
  if (map->base != ptr) {
    VALGRIND_MAKE_MEM_NOACCESS(map->base, map->current);
    /* Unpoisoning is required for ASAN to avoid false-positive diagnostic
     * when this memory will re-used by malloc or another mmapping.
     * See
     * https://libmdbx.dqdkfa.ru/dead-github/pull/93#issuecomment-613687203
     */
    MDBX_ASAN_UNPOISON_MEMORY_REGION(map->base, (map->current < map->limit) ? map->current : map->limit);

    VALGRIND_MAKE_MEM_DEFINED(ptr, map->current);
    MDBX_ASAN_UNPOISON_MEMORY_REGION(ptr, map->current);
    map->base = ptr;
  }
  map->limit = limit;
  map->current = size;

#ifdef MADV_DONTFORK
  if (unlikely(madvise(map->base, map->limit, MADV_DONTFORK) != 0)) {
    ASSERT(errno != 0);
    return errno;
  }
#endif /* MADV_DONTFORK */
#ifdef MADV_NOHUGEPAGE
  (void)madvise(map->base, map->limit, MADV_NOHUGEPAGE);
#endif /* MADV_NOHUGEPAGE */

#endif /* POSIX / Windows */

  /* Zap: Redundant code */
  MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6287);
  ASSERT(rc != MDBX_SUCCESS || (map->base != nullptr && map->base != MAP_FAILED && map->current == size &&
                                map->limit == limit && map->filesize >= size));
  return rc;
}
