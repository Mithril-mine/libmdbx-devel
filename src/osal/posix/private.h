/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file private.h (POSIX part) — system include isolation for the OSAL
/// implementations. Included ONLY by the osal/posix/*.c sources (and
/// osal/details.c when needed), never by the rest of the project.
///
/// Все системно-зависимые #include, типы и макросы POSIX-платформ, которые
/// нужны реализациям OSAL, но не нужны остальному коду библиотеки.

#pragma once

#include "internals.h"

/*----------------------------------------------------------------------------*/

#if defined(__APPLE__) || defined(__MACH__)
#include <AvailabilityMacros.h>
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#include <mach/host_info.h>
#include <mach/mach_port.h>
#include <mach/vm_statistics.h>
#include <uuid/uuid.h>
#include <sys/sysctl.h>
#include <sys/vmmeter.h>
#endif /* Apple */

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/uuid.h>
#endif /* FreeBSD */

#if defined(__linux__) || defined(__gnu_linux__)
#include <mntent.h>
#include <sys/sysinfo.h>
#include <sys/sendfile.h>
#include <sys/statfs.h>
#include <sys/auxv.h>
#if defined(__ANDROID_API__)
#include <android/log.h>
#endif /* Android */
#if __GLIBC_PREREQ(2, 25) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__BSD__) || defined(__bsdi__) ||    \
    defined(__DragonFly__) || defined(__APPLE__) || __has_include(<sys/random.h>)
#include <sys/random.h>
#endif /* sys/random.h */
#endif /* Linux */

#if defined(__sun) || defined(__SVR4)
#include <kstat.h>
#include <sys/mnttab.h>
#endif /* Solaris */

#include <sched.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
