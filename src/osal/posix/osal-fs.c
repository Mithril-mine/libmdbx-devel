/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-fs (Posix part) — filesystem checks: read-only-ness,
/// incore (resident) probe and local/remoteness of the target filesystem.

#include "../../internals.h"
#include "../details.h"

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
