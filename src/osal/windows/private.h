/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file private.h (Windows part) — system include isolation for the OSAL
/// implementations. Included ONLY by the osal/windows/*.c sources, never by
/// the rest of the project.
///
/// Все системно-зависимые #include, типы и макросы Windows-платформ, которые
/// нужны реализациям OSAL, но не нужны остальному коду библиотеки.

#pragma once

#include "internals.h"

/*----------------------------------------------------------------------------*/

#include <windows.h>
#include <winnt.h>
#include <winternl.h>
#include <winioctl.h>
#include <wincrypt.h>
#include <psapi.h>
#include <excpt.h>
#include <io.h>
#include <tlhelp32.h>
#if !MDBX_WITHOUT_MSVC_CRT && defined(_DEBUG)
#include <crtdbg.h>
#endif /* CRT debug */

/*----------------------------------------------------------------------------*/
/* WOF/compressed-volume and NTSTATUS declarations used by osal-mmap.c and
 * osal-fs.c (not present in the MinGW-w64 headers). */

#ifndef WOF_CURRENT_VERSION
typedef struct _WOF_EXTERNAL_INFO {
  DWORD Version;
  DWORD Provider;
} WOF_EXTERNAL_INFO, *PWOF_EXTERNAL_INFO;
#endif /* WOF_CURRENT_VERSION */


/* WOF/WIM/FILE_PROVIDER structures for osal-fs.c (some MinGW-w64 versions
 * don't declare them in winioctl.h). */
#ifndef WIM_PROVIDER_EXTERNAL_INFO
#define WIM_PROVIDER_HASH_SIZE 20
typedef struct _WIM_PROVIDER_EXTERNAL_INFO {
  DWORD Version;
  DWORD Flags;
  LARGE_INTEGER DataSourceId;
  BYTE ResourceHash[WIM_PROVIDER_HASH_SIZE];
} WIM_PROVIDER_EXTERNAL_INFO, *PWIM_PROVIDER_EXTERNAL_INFO;
#endif /* WIM_PROVIDER_EXTERNAL_INFO */

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
