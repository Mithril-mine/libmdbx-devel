/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#if defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS)
#define IS_WINDOWS 1
#else
#define IS_WINDOWS 0
#endif

/* > dist-cutoff-begin */
#ifdef _MSC_VER
#pragma warning(disable : 4206) /* nonstandard extension used: translation unit is empty */
#endif                          /* _MSC_VER (warnings) */
/* < dist-cutoff-end */
