/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-fs (Windows part) — filesystem checks: read-only-ness,
/// incore (resident) probe and local/remoteness of the target filesystem.

#include "../../internals.h"
#include "osal/windows/private.h"
#include "../details.h"

int osal_check_fs_rdonly(mdbx_filehandle_t handle, const pathchar_t *pathname, int err) {
  (void)pathname;
  (void)err;
  if (!imports.GetVolumeInformationByHandleW)
    return MDBX_ENOSYS;
  DWORD unused, flags;
  if (!imports.GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, &unused, &flags, nullptr, 0))
    return (int)GetLastError();
  if ((flags & FILE_READ_ONLY_VOLUME) == 0)
    return MDBX_EACCESS;
  return MDBX_SUCCESS;
}

int osal_check_fs_incore(mdbx_filehandle_t handle) {
  (void)handle;
  return MDBX_RESULT_FALSE;
}

int osal_check_fs_local(mdbx_filehandle_t handle, int flags) {
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

  return MDBX_SUCCESS;
}
