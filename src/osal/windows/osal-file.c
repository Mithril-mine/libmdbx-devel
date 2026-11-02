/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-file.c (Windows part) — files: open/close, read/write, size/sync
/// and filesystem checks.

#include "../../internals.h"
#include "osal/windows/private.h"
#include "../details.h"

/*----------------------------------------------------------------------------*/

int osal_removefile(const pathchar_t *pathname) { return DeleteFileW(pathname) ? MDBX_SUCCESS : (int)GetLastError(); }

int osal_removedirectory(const pathchar_t *pathname) {
  return RemoveDirectoryW(pathname) ? MDBX_SUCCESS : (int)GetLastError();
}

int osal_fileexists(const pathchar_t *pathname) {
  if (GetFileAttributesW(pathname) != INVALID_FILE_ATTRIBUTES)
    return MDBX_RESULT_TRUE;
  int err = GetLastError();
  return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? MDBX_RESULT_FALSE : err;
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
  for (size_t i = 0; i < len; ++i) {
    pathchar_t a = l[i];
    pathchar_t b = r[i];
    a = (a == '\\') ? '/' : a;
    b = (b == '\\') ? '/' : b;
    if (a != b)
      return false;
  }
  return true;
}

int osal_openfile(const enum osal_openfile_purpose purpose, const MDBX_env *env, const pathchar_t *pathname,
                  mdbx_filehandle_t *fd, mdbx_mode_t unix_mode_bits) {
  *fd = INVALID_HANDLE_VALUE;

  DWORD CreationDisposition = unix_mode_bits ? OPEN_ALWAYS : OPEN_EXISTING;
  DWORD FlagsAndAttributes = FILE_FLAG_POSIX_SEMANTICS | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
  DWORD DesiredAccess = FILE_READ_ATTRIBUTES;
  DWORD ShareMode = (env->flags & MDBX_EXCLUSIVE) ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE);

  switch (purpose) {
  default:
    return ERROR_INVALID_PARAMETER;
  case MDBX_OPEN_LCK:
    CreationDisposition = OPEN_ALWAYS;
    DesiredAccess |= GENERIC_READ | GENERIC_WRITE;
    FlagsAndAttributes |= FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OVERLAPPED;
    break;
  case MDBX_OPEN_DXB_READ:
    CreationDisposition = OPEN_EXISTING;
    DesiredAccess |= GENERIC_READ;
    ShareMode |= FILE_SHARE_READ;
    break;
  case MDBX_OPEN_DXB_LAZY:
    DesiredAccess |= GENERIC_READ | GENERIC_WRITE;
    /* With the exclusive fast i/o path (issue #179) the data file is opened
     * shareable even in MDBX_EXCLUSIVE, so that the overlapped write-through
     * and dsync handles below can be created from this same process; the
     * exclusivity is then enforced by the constant byte-range locks taken in
     * windows_seize_exclusive(). Only the real data-file opens are relaxed: the
     * lock file above stays unshareable in exclusive mode for a fast rejection
     * of other openers, and DELETE/COPY below keep the original semantics. */
    if (env_exclusive_fastpath(env))
      ShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    break;
  case MDBX_OPEN_DXB_OVERLAPPED_DIRECT:
    FlagsAndAttributes |= FILE_FLAG_NO_BUFFERING;
    __fallthrough /* fall through */;
  case MDBX_OPEN_DXB_OVERLAPPED:
    FlagsAndAttributes |= FILE_FLAG_OVERLAPPED;
    __fallthrough /* fall through */;
  case MDBX_OPEN_DXB_DSYNC:
    CreationDisposition = OPEN_EXISTING;
    DesiredAccess |= GENERIC_WRITE | GENERIC_READ;
    FlagsAndAttributes |= FILE_FLAG_WRITE_THROUGH;
    if (env_exclusive_fastpath(env))
      ShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE; /* see MDBX_OPEN_DXB_LAZY */
    break;
  case MDBX_OPEN_DELETE:
    CreationDisposition = OPEN_EXISTING;
    ShareMode |= FILE_SHARE_DELETE;
    DesiredAccess = FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE;
    break;
  case MDBX_OPEN_COPY_EXCL:
    CreationDisposition = CREATE_NEW;
    __fallthrough /* fall through */;
  case MDBX_OPEN_COPY_OVERWRITE:
    ShareMode = 0;
    DesiredAccess |= GENERIC_WRITE;
    if (env->ps >= globals.sys_pagesize)
      FlagsAndAttributes |= FILE_FLAG_NO_BUFFERING;
    break;
  }

  *fd = CreateFileW(pathname, DesiredAccess, ShareMode, nullptr, CreationDisposition, FlagsAndAttributes, nullptr);
  if (*fd == INVALID_HANDLE_VALUE) {
    int err = (int)GetLastError();
    if (err == ERROR_ACCESS_DENIED && purpose == MDBX_OPEN_LCK) {
      if (GetFileAttributesW(pathname) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        err = ERROR_FILE_NOT_FOUND;
    }
    /* In exclusive mode a sharing/lock violation means exactly what
     * mdbx_env_open() documents: the environment is in use by another process
     * (or another exclusive opener of this process). Report MDBX_BUSY instead
     * of the raw winerror. (issue #179) */
    if ((env->flags & MDBX_EXCLUSIVE) && (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION))
      err = MDBX_BUSY;
    return err;
  }

  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(*fd, &info)) {
    int err = (int)GetLastError();
    CloseHandle(*fd);
    *fd = INVALID_HANDLE_VALUE;
    return err;
  }
  const DWORD AttributesDiff =
      (info.dwFileAttributes ^ FlagsAndAttributes) & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                                                      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_COMPRESSED);
  if (AttributesDiff)
    (void)SetFileAttributesW(pathname, info.dwFileAttributes ^ AttributesDiff);

  return MDBX_SUCCESS;
}

int osal_closefile(mdbx_filehandle_t fd) { return CloseHandle(fd) ? MDBX_SUCCESS : (int)GetLastError(); }

int osal_pread(mdbx_filehandle_t fd, void *buf, size_t bytes, uint64_t offset) {
  return osal_pread_ev(fd, 0, buf, bytes, offset);
}

int osal_pread_ev(mdbx_filehandle_t fd, HANDLE ev, void *buf, size_t bytes, uint64_t offset) {
  if (bytes > MAX_IO_BYTES)
    return MDBX_EINVAL;
  OVERLAPPED ov;
  ov.Internal = 0;
  ov.InternalHigh = 0;
  ov.Offset = (DWORD)offset;
  ov.OffsetHigh = HIGH_DWORD(offset);
  ov.hEvent = ev;

  DWORD read;
  if (unlikely(!ReadFile(fd, buf, (DWORD)bytes, &read, &ov))) {
    int err = (int)GetLastError();
    if (err != ERROR_IO_PENDING)
      return (err == MDBX_SUCCESS) ? /* paranoia */ ERROR_READ_FAULT : err;
    if (!GetOverlappedResult(fd, &ov, &read, true)) {
      err = (int)GetLastError();
      CancelIo(fd);
      return (err == MDBX_SUCCESS) ? /* paranoia */ ERROR_READ_FAULT : err;
    }
  }
  return (bytes == (size_t)read) ? MDBX_SUCCESS : MDBX_ENODATA;
}

int osal_pwrite(mdbx_filehandle_t fd, const void *buf, size_t bytes, uint64_t offset) {
  return osal_pwrite_ev(fd, 0, buf, bytes, offset);
}

int osal_pwrite_ev(mdbx_filehandle_t fd, HANDLE ev, const void *buf, size_t bytes, uint64_t offset) {
  while (true) {
    OVERLAPPED ov;
    ov.Internal = 0;
    ov.InternalHigh = 0;
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = HIGH_DWORD(offset);
    ov.hEvent = ev;

    DWORD written;
    if (unlikely(!WriteFile(fd, buf, likely(bytes <= MAX_IO_BYTES) ? (DWORD)bytes : MAX_IO_BYTES, &written, &ov))) {
      int err = (int)GetLastError();
      if (err != ERROR_IO_PENDING)
        return (err == MDBX_SUCCESS) ? /* paranoia */ ERROR_WRITE_FAULT : err;
      if (!GetOverlappedResult(fd, &ov, &written, true)) {
        err = (int)GetLastError();
        CancelIo(fd);
        return (err == MDBX_SUCCESS) ? /* paranoia */ ERROR_WRITE_FAULT : err;
      }
    }
    if (likely(bytes == written))
      return MDBX_SUCCESS;
    bytes -= written;
    offset += written;
    buf = ptr_disp(buf, written);
  }
}

int osal_write(mdbx_filehandle_t fd, const void *buf, size_t bytes) {
  while (true) {
    DWORD written;
    if (unlikely(!WriteFile(fd, buf, likely(bytes <= MAX_IO_BYTES) ? (DWORD)bytes : MAX_IO_BYTES, &written, nullptr)))
      return (int)GetLastError();
    if (likely(bytes == written))
      return MDBX_SUCCESS;
    bytes -= written;
    buf = ptr_disp(buf, written);
  }
}

int osal_pwritev(mdbx_filehandle_t fd, struct iovec *iov, size_t sgvcnt, uint64_t offset) {
  size_t expected = 0;
  for (size_t i = 0; i < sgvcnt; ++i)
    expected += iov[i].iov_len;
  size_t written = 0;
  for (size_t i = 0; i < sgvcnt; ++i) {
    int rc = osal_pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    written += iov[i].iov_len;
    offset += iov[i].iov_len;
  }
  return (expected == written) ? MDBX_SUCCESS : MDBX_EIO /* ERROR_WRITE_FAULT */;
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
  if (env->ioring.overlapped_fd)
    return osal_pwrite(env->ioring.overlapped_fd, buf, bytes, offset);
  return osal_pwrite(env->lazy_fd, buf, bytes, offset);
}

int osal_dxb_write_stubv(const MDBX_env *env, struct iovec *iov, size_t sgvcnt, uint64_t offset) {
  if (env->ioring.overlapped_fd)
    return osal_pwritev(env->ioring.overlapped_fd, iov, sgvcnt, offset);
  return osal_pwritev(env->lazy_fd, iov, sgvcnt, offset);
}

int osal_fsync(mdbx_filehandle_t fd, enum osal_syncmode_bits mode_bits) {
  if ((mode_bits & (MDBX_SYNC_DATA | MDBX_SYNC_IODQ)) && !FlushFileBuffers(fd))
    return (int)GetLastError();
  return MDBX_SUCCESS;
}

int osal_filesize(mdbx_filehandle_t fd, uint64_t *length) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(fd, &info))
    return (int)GetLastError();
  *length = info.nFileSizeLow | (uint64_t)info.nFileSizeHigh << 32;
  return MDBX_SUCCESS;
}

int osal_is_pipe(mdbx_filehandle_t fd) {
  switch (GetFileType(fd)) {
  case FILE_TYPE_DISK:
    return MDBX_RESULT_FALSE;
  case FILE_TYPE_CHAR:
  case FILE_TYPE_PIPE:
    return MDBX_RESULT_TRUE;
  default:
    return (int)GetLastError();
  }
}

int osal_fsetsize(mdbx_filehandle_t fd, const uint64_t length) {
  if (imports.SetFileInformationByHandle) {
    FILE_END_OF_FILE_INFO EndOfFileInfo;
    EndOfFileInfo.EndOfFile.QuadPart = length;
    return imports.SetFileInformationByHandle(fd, FileEndOfFileInfo, &EndOfFileInfo, sizeof(FILE_END_OF_FILE_INFO))
               ? MDBX_SUCCESS
               : (int)GetLastError();
  } else {
    LARGE_INTEGER li;
    li.QuadPart = length;
    return (SetFilePointerEx(fd, li, nullptr, FILE_BEGIN) && SetEndOfFile(fd)) ? MDBX_SUCCESS : (int)GetLastError();
  }
}

int osal_fseek(mdbx_filehandle_t fd, uint64_t pos) {
  LARGE_INTEGER li;
  li.QuadPart = pos;
  return SetFilePointerEx(fd, li, nullptr, FILE_BEGIN) ? MDBX_SUCCESS : (int)GetLastError();
}

/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
