/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#pragma once

#include "../../src/essentials.h"

#ifdef _MSC_VER
#pragma warning(push, 1)
#pragma warning(disable : 4548) /* expression before comma has no effect;                                              \
                                   expected expression with side - effect */
#pragma warning(disable : 4530) /* C++ exception handler used, but unwind                                              \
                                   semantics are not enabled. Specify /EHsc */
#pragma warning(disable : 4577) /* 'noexcept' used with no exception handling                                          \
                                   mode specified; termination on exception                                            \
                                   is not guaranteed. Specify /EHsc */
#endif                          /* _MSC_VER (warnings) */

#ifndef IS_WINDOWS
#error IS_WINDOWS?
#endif

#if IS_WINDOWS
/* If you wish to build your application for a previous Windows platform,
 * include WinSDKVer.h and set the _WIN32_WINNT macro to the platform you
 * wish to support before including SDKDDKVer.h.
 *
 * TODO: #define _WIN32_WINNT WIN32_MUSTDIE */
#include <SDKDDKVer.h>
#endif /* WINDOWS */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if IS_WINDOWS
#include <io.h>
#else
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _BSD_SOURCE
#include <endian.h>
#endif

#include <algorithm>
#include <cassert>
#include <cinttypes> // for PRId64, PRIu64
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define MDBX_INTERNAL
#define xMDBX_TOOLS /* Avoid using internal ASSERT() */
#include "../../mdbx.h++"
#include "../../src/osal.h"

#include "../../src/options.h"

#ifdef _MSC_VER
#pragma warning(pop)
#pragma warning(disable : 4201) /* nonstandard extension used: nameless                                                \
                                   struct/union */
#pragma warning(disable : 4127) /* conditional expression is constant */
#if _MSC_VER < 1900
#pragma warning(disable : 4510) /* default constructor could                                                           \
                                   not be generated */
#pragma warning(disable : 4512) /* assignment operator could                                                           \
                                   not be generated  */
#pragma warning(disable : 4610) /* user-defined constructor required */
#ifndef snprintf
#define snprintf(buffer, buffer_size, format, ...) _snprintf_s(buffer, buffer_size, _TRUNCATE, format, __VA_ARGS__)
#endif
#ifndef vsnprintf
#define vsnprintf(buffer, buffer_size, format, args) _vsnprintf_s(buffer, buffer_size, _TRUNCATE, format, args)
#endif
#pragma warning(disable : 4996) /* 'vsnprintf': This function or variable                                              \
                                   may be unsafe */
#endif
#endif /* _MSC_VER */
