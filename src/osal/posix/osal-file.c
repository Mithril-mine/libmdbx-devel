/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-file.c (POSIX part) — files: open/close, read/write, size/sync
/// and filesystem checks.

#include "internals.h"

static const char osal_dev_null4file[] = "/dev/null";

/*----------------------------------------------------------------------------*/

int osal_removefile(const pathchar_t *pathname) { return unlink(pathname) ? errno : MDBX_SUCCESS; }

static bool is_valid_fd(int fd) { return !(isatty(fd) < 0 && errno == EBADF); }

int osal_removedirectory(const pathchar_t *pathname) { return rmdir(pathname) ? errno : MDBX_SUCCESS; }

int osal_fileexists(const pathchar_t *pathname) {
  if (access(pathname, F_OK) == 0)
    return MDBX_RESULT_TRUE;
  int err = errno;
  return (err == ENOENT || err == ENOTDIR) ? MDBX_RESULT_FALSE : err;
}

pathchar_t *osal_fileext(const pathchar_t *pathname, size_t len) {
  const pathchar_t *ext = nullptr;
  for (size_t i = 0; i < len && pathname[i]; i++)
    if (pathname[i] == '.')
      ext = pathname + i;
    else if (osal_isdirsep(pathname[i]))
      ext = nullptr;
  return (pathchar_t *)ext;
}

bool osal_pathequal(const pathchar_t *l, const pathchar_t *r, size_t len) {
  return memcmp(l, r, len * sizeof(pathchar_t)) == 0;
}

int osal_openfile(const enum osal_openfile_purpose purpose, const MDBX_env *env, const pathchar_t *pathname,
                  mdbx_filehandle_t *fd, mdbx_mode_t unix_mode_bits) {
  *fd = INVALID_HANDLE_VALUE;

  int flags = unix_mode_bits ? O_CREAT : 0;
  switch (purpose) {
  default:
    return EINVAL;
  case MDBX_OPEN_LCK:
    flags |= O_RDWR;
    break;
  case MDBX_OPEN_DXB_READ:
    flags = O_RDONLY;
    break;
  case MDBX_OPEN_DXB_LAZY:
    flags |= O_RDWR;
    break;
  case MDBX_OPEN_DXB_DSYNC:
    flags |= O_WRONLY;
#if defined(O_DSYNC)
    flags |= O_DSYNC;
#elif defined(O_SYNC)
    flags |= O_SYNC;
#elif defined(O_FSYNC)
    flags |= O_FSYNC;
#endif
    break;
  case MDBX_OPEN_DELETE:
    flags = O_RDWR;
    break;
  case MDBX_OPEN_COPY_EXCL:
    flags |= O_EXCL;
    __fallthrough /* fall through */;
  case MDBX_OPEN_COPY_OVERWRITE:
    flags |= O_WRONLY;
  }

  const bool direct_nocache_for_copy = env->ps >= globals.sys_pagesize && purpose >= MDBX_OPEN_COPY_EXCL;
  if (direct_nocache_for_copy) {
#if defined(O_DIRECT)
    flags |= O_DIRECT;
#endif /* O_DIRECT */
#if defined(O_NOCACHE)
    flags |= O_NOCACHE;
#endif /* O_NOCACHE */
  }

#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif /* O_CLOEXEC */

  /* Safeguard for https://libmdbx.dqdkfa.ru/dead-github/issues/144 */
#if STDIN_FILENO == 0 && STDOUT_FILENO == 1 && STDERR_FILENO == 2
  /* When opening a database in the switched daemon mode program, the standard file descriptors can be closed, and the
   * newly opened here file descriptor can get one of the those values. In this cases, using such a descriptor to
   * interact with the database is extremely dangerous, since in a complex application using many libraries with
   * dependencies, it cannot be guaranteed that no one component continued to use the standard value of the descriptor
   * and thus will perform unexpected input/output to the database file and damage it.
   *
   * To prevent the described situation, several safety measures are being taken here:
   *  1. If any of the descriptors 0-2 is closed, an attempt will be made to open it for /dev/null.
   *     This ensures that the subsequent dup() returns a handle greater than 2.
   *  2. If the descriptor open to the database is less than or equal to 2,
   *     it will be duplicated using dup() so that the total value is greater than 2.
   *  3. All temporarily open descriptors are closed,
   *     including those open to the database if its value is less than or equal to 2.
   */
  int hazardous_fd0 = -1, hazardous_fd1 = -1, hazardous_fd2 = -1;
  if (!is_valid_fd(STDIN_FILENO)) {
    WARNING("STD%s_FILENO/%d is invalid, open %s for temporary stub", "IN", STDIN_FILENO, osal_dev_null4file);
    hazardous_fd0 = open(osal_dev_null4file, O_RDONLY | O_NOCTTY);
  }
  if (!is_valid_fd(STDOUT_FILENO)) {
    WARNING("STD%s_FILENO/%d is invalid, open %s for temporary stub", "OUT", STDOUT_FILENO, osal_dev_null4file);
    hazardous_fd1 = open(osal_dev_null4file, O_WRONLY | O_NOCTTY);
  }
  if (!is_valid_fd(STDERR_FILENO)) {
    WARNING("STD%s_FILENO/%d is invalid, open %s for temporary stub", "ERR", STDERR_FILENO, osal_dev_null4file);
    hazardous_fd2 = open(osal_dev_null4file, O_WRONLY | O_NOCTTY);
  }
#else
#error "Unexpected or unsupported UNIX or POSIX system"
#endif /* STDIN_FILENO == 0 && STDERR_FILENO == 2 */

  *fd = open(pathname, flags, unix_mode_bits);
#if defined(O_DIRECT)
  if (*fd < 0 && (flags & O_DIRECT) && (errno == EINVAL || errno == EAFNOSUPPORT)) {
    flags &= ~O_DIRECT;
    *fd = open(pathname, flags, unix_mode_bits);
  }
#endif /* O_DIRECT */

  int err = MDBX_SUCCESS;
  if (unlikely(*fd < 0)) {
    err = errno;
    if (err == EACCES && purpose == MDBX_OPEN_LCK) {
      struct stat unused;
      if (stat(pathname, &unused) == 0 || (err = errno) != ENOENT)
        err = EACCES /* restore error if file exists */;
    }
  }

  /* Safeguard for https://libmdbx.dqdkfa.ru/dead-github/issues/144 */
#if STDIN_FILENO == 0 && STDOUT_FILENO == 1 && STDERR_FILENO == 2
  else if (unlikely(*fd == STDIN_FILENO)) {
    WARNING("Got STD%s_FILENO/%d, avoid using it by dup(fd)", "IN", STDIN_FILENO);
    ASSERT(hazardous_fd0 == -1);
    *fd = dup(hazardous_fd0 = STDIN_FILENO);
    if (*fd < 0)
      err = errno;
  } else if (unlikely(*fd == STDOUT_FILENO)) {
    WARNING("Got STD%s_FILENO/%d, avoid using it by dup(fd)", "OUT", STDOUT_FILENO);
    ASSERT(hazardous_fd1 == -1);
    *fd = dup(hazardous_fd1 = STDOUT_FILENO);
    if (*fd < 0)
      err = errno;
  } else if (unlikely(*fd == STDERR_FILENO)) {
    WARNING("Got STD%s_FILENO/%d, avoid using it by dup(fd)", "ERR", STDERR_FILENO);
    ASSERT(hazardous_fd2 == -1);
    *fd = dup(hazardous_fd2 = STDERR_FILENO);
    if (*fd < 0)
      err = errno;
  }
  if (hazardous_fd0 != -1)
    close(hazardous_fd0);
  if (hazardous_fd1 != -1)
    close(hazardous_fd1);
  if (hazardous_fd2 != -1)
    close(hazardous_fd2);
  if (*fd >= STDIN_FILENO && *fd <= STDERR_FILENO) {
    ERROR("Rejecting the use of a FD in the range "
          "STDIN_FILENO/%d..STDERR_FILENO/%d to prevent database corruption",
          STDIN_FILENO, STDERR_FILENO);
    close(*fd);
    return EBADF;
  }
#else
#error "Unexpected or unsupported UNIX or POSIX system"
#endif /* STDIN_FILENO == 0 && STDERR_FILENO == 2 */

  if (unlikely(err != 0)) {
    if (*fd >= 0)
      close(*fd);
    return err;
  }

#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
  const int fd_flags = fcntl(*fd, F_GETFD);
  if (fd_flags != -1)
    (void)fcntl(*fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif /* FD_CLOEXEC && !O_CLOEXEC */

  if (direct_nocache_for_copy) {
#if defined(F_NOCACHE) && !defined(O_NOCACHE)
    (void)fcntl(*fd, F_NOCACHE, 1);
#endif /* F_NOCACHE */
  }

  return MDBX_SUCCESS;
}

int osal_closefile(mdbx_filehandle_t fd) {
  ASSERT(fd > STDERR_FILENO);
  return (close(fd) == 0) ? MDBX_SUCCESS : errno;
}

int osal_pread(mdbx_filehandle_t fd, void *buf, size_t bytes, uint64_t offset) {
  if (bytes > MAX_IO_BYTES)
    return MDBX_EINVAL;
  STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");
  intptr_t read = pread(fd, buf, bytes, offset);
  if (read < 0) {
    int rc = errno;
    return (rc == MDBX_SUCCESS) ? /* paranoia */ MDBX_EIO : rc;
  }
  return (bytes == (size_t)read) ? MDBX_SUCCESS : MDBX_ENODATA;
}

int osal_pwrite(mdbx_filehandle_t fd, const void *buf, size_t bytes, uint64_t offset) {
  while (true) {
    STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");
    const intptr_t written = pwrite(fd, buf, likely(bytes <= MAX_IO_BYTES) ? bytes : MAX_IO_BYTES, offset);
    if (likely(bytes == (size_t)written))
      return MDBX_SUCCESS;
    if (written < 0) {
      const int rc = errno;
      if (rc != EINTR)
        return rc;
      continue;
    }
    bytes -= written;
    offset += written;
    buf = ptr_disp(buf, written);
  }
}

int osal_write(mdbx_filehandle_t fd, const void *buf, size_t bytes) {
  while (true) {
    STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");
    const intptr_t written = write(fd, buf, likely(bytes <= MAX_IO_BYTES) ? bytes : MAX_IO_BYTES);
    if (likely(bytes == (size_t)written))
      return MDBX_SUCCESS;
    if (written < 0) {
      const int rc = errno;
      if (rc != EINTR)
        return rc;
      continue;
    }
    bytes -= written;
    buf = ptr_disp(buf, written);
  }
}

int osal_pwritev(mdbx_filehandle_t fd, struct iovec *iov, size_t sgvcnt, uint64_t offset) {
  size_t expected = 0;
  for (size_t i = 0; i < sgvcnt; ++i)
    expected += iov[i].iov_len;
#if !MDBX_HAVE_PWRITEV
  size_t written = 0;
  for (size_t i = 0; i < sgvcnt; ++i) {
    int rc = osal_pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    written += iov[i].iov_len;
    offset += iov[i].iov_len;
  }
  return (expected == written) ? MDBX_SUCCESS : MDBX_EIO /* ERROR_WRITE_FAULT */;
#else
  int rc;
  intptr_t written;
  do {
    STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");
    written = pwritev(fd, iov, sgvcnt, offset);
    if (likely(expected == (size_t)written))
      return MDBX_SUCCESS;
    rc = errno;
  } while (rc == EINTR);
  return (written < 0) ? rc : MDBX_EIO /* Use which error code? */;
#endif
}

/* Write an auxiliary (stub) buffer to the database body through the handle that
 * holds the per-txn body lock: overlapped_fd on Windows when present, otherwise
 * the memory-mapped (lazy) handle.  On Windows byte-range locks are per-handle,
 * so writing a locked region through a different handle fails with
 * ERROR_LOCK_VIOLATION; routing through the lock owner avoids that.
 *
 * Both helpers delegate to osal_pwrite()/osal_pwritev(), which on Windows
 * already implement a synchronous-over-overlapped write with hEvent == NULL
 * (GetOverlappedResult waits on the file handle, not on a manual-reset event).
 * Because the scatter-gather segments are written strictly one after another,
 * there is no risk of reusing a shared event across in-flight operations. */
int osal_dxb_write_stub(const MDBX_env *env, const void *buf, size_t bytes, uint64_t offset) {
  return osal_pwrite(env->lazy_fd, buf, bytes, offset);
}

int osal_dxb_write_stubv(const MDBX_env *env, struct iovec *iov, size_t sgvcnt, uint64_t offset) {
  return osal_pwritev(env->lazy_fd, iov, sgvcnt, offset);
}

int osal_fsync(mdbx_filehandle_t fd, enum osal_syncmode_bits mode_bits) {

#if defined(__APPLE__) && MDBX_APPLE_SPEED_INSTEADOF_DURABILITY == MDBX_OSX_WANNA_DURABILITY
  if (mode_bits & MDBX_SYNC_IODQ)
    return likely(fcntl(fd, F_FULLFSYNC) != -1) ? MDBX_SUCCESS : errno;
#endif /* MacOS */

  /* LY: This approach is always safe and without appreciable performance
   * degradation, even on a kernel with fdatasync's bug.
   *
   * For more info about of a corresponding fdatasync() bug
   * see http://www.spinics.net/lists/linux-ext4/msg33714.html */
  while (1) {
    switch (mode_bits & (MDBX_SYNC_DATA | MDBX_SYNC_SIZE)) {
    case MDBX_SYNC_NONE:
    case MDBX_SYNC_KICK:
      return MDBX_SUCCESS /* nothing to do */;
#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    case MDBX_SYNC_DATA:
      if (likely(fdatasync(fd) == 0))
        return MDBX_SUCCESS;
      break /* error */;
#if defined(__linux__) || defined(__gnu_linux__)
    case MDBX_SYNC_SIZE:
      ASSERT(globals.linux_kernel_version >= 0x03060000);
      return MDBX_SUCCESS;
#endif /* Linux */
#endif /* _POSIX_SYNCHRONIZED_IO > 0 */
    default:
      if (likely(fsync(fd) == 0))
        return MDBX_SUCCESS;
    }

    int rc = errno;
    if (rc != EINTR)
      return rc;
  }
}

int osal_filesize(mdbx_filehandle_t fd, uint64_t *length) {
  struct stat st;

  STATIC_ASSERT_MSG(sizeof(off_t) <= sizeof(uint64_t), "off_t is expected to be no wider than 64 bits");
  if (fstat(fd, &st))
    return errno;

  *length = st.st_size;
  return MDBX_SUCCESS;
}

int osal_is_pipe(mdbx_filehandle_t fd) {
  struct stat info;
  if (fstat(fd, &info))
    return errno;
  switch (info.st_mode & S_IFMT) {
  case S_IFBLK:
  case S_IFREG:
    return MDBX_RESULT_FALSE;
  case S_IFCHR:
  case S_IFIFO:
  case S_IFSOCK:
    return MDBX_RESULT_TRUE;
  case S_IFDIR:
  case S_IFLNK:
  default:
    return MDBX_INCOMPATIBLE;
  }
}

int osal_fsetsize(mdbx_filehandle_t fd, const uint64_t length) {
  STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");

#if MDBX_USE_FALLOCATE
  struct stat info;
  if (unlikely(fstat(fd, &info)))
    return errno;

  const uint64_t allocated = UINT64_C(512) * info.st_blocks;
  if (length > allocated) {
#if defined(FALLOC_FL_ALLOCATE_RANGE) && defined(FALLOC_FL_KEEP_SIZE)
    /* prefer a low-level non-portable function to avoid glibc emulation
     * if the file system does not support the operation. */
    int err = fallocate(fd, FALLOC_FL_ALLOCATE_RANGE | FALLOC_FL_KEEP_SIZE, 0, length);
#elif defined(__APPLE__)
    fstore_t store = {
        .fst_flags = F_ALLOCATECONTIG, .fst_posmode = F_PEOFPOSMODE, .fst_offset = 0, .fst_length = length};
    int err = MDBX_SUCCESS;
    if (fcntl(fd, F_PREALLOCATE, &store)) {
      /* TODO: implement step-by-step allocation in chunks of 16384, 8192, 4094, 2048, 1024 Kb */
      store.fst_flags = F_ALLOCATEALL;
      if (fcntl(fd, F_PREALLOCATE, &store))
        err = errno;
    }
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    const int err = posix_fallocate(fd, 0, length);
    if (!err && length > (uint64_t)info.st_size)
      info.st_size = length /* posix_fallocate() extends the file */;
#else
    const int err = fallocate(fd, 0, 0, length) ? errno : MDBX_SUCCESS;
    if (!err && length > (uint64_t)info.st_size)
      info.st_size = length /* fallocate() extends the file */;
#endif
    if (unlikely(err) && ignore_enosys_and_eremote(err) != MDBX_RESULT_TRUE) {
      /* Workaround for testing: ignore ENOSPC for TMPFS/RAMFS.
       * This is insignificant for production, but it helps in some tests using /dev/shm inside docker/containers. */
      if (err != ENOSPC || osal_check_fs_incore(fd) != MDBX_RESULT_TRUE)
        return err;
    }
  }

  if (length == (uint64_t)info.st_size)
    return MDBX_SUCCESS;
#endif

#if defined(__linux__) || defined(__gnu_linux__)
  if (globals.linux_kernel_version < 0x05110000 && globals.linux_kernel_version >= 0x050a0000) {
    struct statfs statfs_info;
    if (fstatfs(fd, &statfs_info))
      return errno;
    if (statfs_info.f_type == 0xEF53 /* EXT4_SUPER_MAGIC */ && unlikely(fdatasync(fd)))
      return errno;
  }
#endif /* Linux */

  return unlikely(ftruncate(fd, length)) ? errno : MDBX_SUCCESS;
}

int osal_fseek(mdbx_filehandle_t fd, uint64_t pos) {
  STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");
  return (lseek(fd, pos, SEEK_SET) < 0) ? errno : MDBX_SUCCESS;
}

int osal_fseek_shut(mdbx_filehandle_t fd, uint64_t *safe_parking_lot_offset) {
  /* Set the position in files outside of the data to avoid corruption
   * due to erroneous use of file descriptors in the application code. */
  int rc = osal_fseek(fd, *safe_parking_lot_offset);
  if (unlikely(rc == MDBX_EINVAL)) {
    const uint64_t offset = *safe_parking_lot_offset;
    uint64_t best = 0;
    unsigned shift_min = 1, shift_max = 32;
    while (shift_min <= shift_max) {
      unsigned shift = (shift_min + shift_max) >> 1;
      const uint64_t probe = offset >> shift;
      int err = osal_fseek(fd, probe);
      if (err == MDBX_SUCCESS) {
        /* remember only successful offset, since the search may end on a failing probe */
        best = probe;
        shift_max = shift - 1;
        rc = err;
      } else if (err == MDBX_EINVAL)
        shift_min = shift + 1;
      else {
        rc = err;
        break;
      }
    }
    if (rc == MDBX_SUCCESS) {
      *safe_parking_lot_offset = best;
      /* the last successful probe is the one with the smallest shift, i.e. `best`,
       * but re-seek explicitly to not depend on the order of probes */
      rc = osal_fseek(fd, best);
    }
  }
  return rc;
}

/*----------------------------------------------------------------------------*/

int osal_thread_create(osal_thread_t *thread, THREAD_RESULT(THREAD_CALL *start_routine)(void *), void *arg) {
  return pthread_create(thread, nullptr, start_routine, arg);
}

int osal_thread_join(osal_thread_t thread) {
  void *unused_retval = &unused_retval;
  return pthread_join(thread, &unused_retval);
}

/*----------------------------------------------------------------------------*/
