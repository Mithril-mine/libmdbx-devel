/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-mmap.c (POSIX part) — memory mapping: mmap/munmap/mresize/msync
/// and filesystem checks for a mapped device.

#include "internals.h"

int osal_msync(const osal_mmap_t *map, size_t length, enum osal_syncmode_bits mode_bits) {
  if (!MDBX_MMAP_NEEDS_JOLT && mode_bits == MDBX_SYNC_NONE)
    return MDBX_SUCCESS;

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
  return MDBX_SUCCESS;
}

int osal_check_fs_rdonly(mdbx_filehandle_t handle, const pathchar_t *pathname, int err) {
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
  return MDBX_SUCCESS;
}

int osal_check_fs_incore(mdbx_filehandle_t handle) {
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

  return MDBX_RESULT_FALSE;
}

int osal_check_fs_local(mdbx_filehandle_t handle, int flags) {

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

  int err = osal_check_fs_local(map->fd, flags);
  if (unlikely(err != MDBX_SUCCESS)) {
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
    map->current = size;
  } else {
    err = osal_filesize(map->fd, &map->filesize);
    VERBOSE("filesize %" PRIu64 ", err %d", map->filesize, err);
    if (err != MDBX_SUCCESS)
      return LOG_IFERR(err);
    map->current = (map->filesize > limit) ? limit : (size_t)map->filesize;
  }

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
  if (unlikely(munmap(map->base, map->limit)))
    ERROR("Unexpected munmap(%p, %zi) error %d", map->base, map->limit, errno);

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

  /* Zap: Redundant code */
  MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6287);
  ASSERT(rc != MDBX_SUCCESS || (map->base != nullptr && map->base != MAP_FAILED && map->current == size &&
                                map->limit == limit && map->filesize >= size));
  return rc;
}
