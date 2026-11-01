/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer
/// \file osal-misc.c (POSIX part) — panic, asserts, string/memory helpers,
/// monotonic clock, jitter, boot-id/guid, sysram info and ctor/dtor.

/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer

#include "../../internals.h"
#include "../details.h"

#if defined(__linux__) || defined(__gnu_linux__)
#include <sys/sysinfo.h>
#endif /* Linux */

#if defined(__APPLE__) || defined(__MACH__)
#include <mach/mach_time.h>
#include <mach/vm_statistics.h>
#endif /* Apple */

/*----------------------------------------------------------------------------*/

#if defined(__ANDROID_API__)
__extern_C void __assert2(const char *file, int line, const char *function, const char *msg) __noreturn;
#define __assert_fail(assertion, file, line, function) __assert2(file, line, function, assertion)

#elif defined(__UCLIBC__)
MDBX_NORETURN __extern_C void __assert(const char *, const char *, unsigned, const char *)
#ifdef __THROW
    __THROW
#else
    __nothrow
#endif /* __THROW */
    ;
#define __assert_fail(assertion, file, line, function) __assert(assertion, file, line, function)

#elif _POSIX_C_SOURCE > 200212 &&                                                                                      \
    /* workaround for avoid musl libc wrong prototype */ (defined(__GLIBC__) || defined(__GNU_LIBRARY__))
/* Prototype should match libc runtime. ISO POSIX (2003) & LSB 1.x-3.x */
MDBX_NORETURN __extern_C void __assert_fail(const char *assertion, const char *file, unsigned line,
                                            const char *function)
#ifdef __THROW
    __THROW
#else
    __nothrow
#endif /* __THROW */
    ;

#elif defined(__APPLE__) || defined(__MACH__)
__extern_C void __assert_rtn(const char *function, const char *file, int line, const char *assertion) /* __nothrow */
#ifdef __dead2
    __dead2
#else
    MDBX_NORETURN
#endif /* __dead2 */
#ifdef __disable_tail_calls
    __disable_tail_calls
#endif /* __disable_tail_calls */
    ;

#define __assert_fail(assertion, file, line, function) __assert_rtn(function, file, line, assertion)
#elif defined(__sun) || defined(__SVR4) || defined(__svr4__)
MDBX_NORETURN __extern_C void __assert_c99(const char *assertion, const char *file, int line, const char *function);
#define __assert_fail(assertion, file, line, function) __assert_c99(assertion, file, line, function)
#elif defined(__OpenBSD__)
__extern_C __dead void __assert2(const char *file, int line, const char *function,
                                 const char *assertion) /* __nothrow */;
#define __assert_fail(assertion, file, line, function) __assert2(file, line, function, assertion)
#elif defined(__NetBSD__)
__extern_C __dead void __assert13(const char *file, int line, const char *function,
                                  const char *assertion) /* __nothrow */;
#define __assert_fail(assertion, file, line, function) __assert13(file, line, function, assertion)
#elif defined(__FreeBSD__) || defined(__BSD__) || defined(__bsdi__) || defined(__DragonFly__)
__extern_C void __assert(const char *function, const char *file, int line, const char *assertion) /* __nothrow */
#ifdef __dead2
    __dead2
#else
    MDBX_NORETURN
#endif /* __dead2 */
#ifdef __disable_tail_calls
    __disable_tail_calls
#endif /* __disable_tail_calls */
    ;
#define __assert_fail(assertion, file, line, function) __assert(function, file, line, assertion)

#endif /* __assert_fail */

__cold void osal_panic(const char *msg, const char *func, unsigned line) {
  while (1) {
    __assert_fail(msg, "libmdbx", line, func);
    abort();
  }
}

/*----------------------------------------------------------------------------*/

#ifndef osal_vasprintf
#endif /* osal_vasprintf */

#ifndef osal_asprintf
#endif /* osal_asprintf */

/*----------------------------------------------------------------------------*/

__cold void osal_jitter(bool tiny) {
  for (;;) {
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    unsigned salt = 5296013u * (unsigned)__rdtsc();
    salt ^= salt >> 11;
    salt *= 25810541u;
#else
    const unsigned salt = rand();
#endif

    const int coin = salt % (tiny ? 29u : 43u);
    if (coin < 43 / 3)
      break;
    osal_yield();
    if (coin > 43 * 2 / 3)
      usleep(coin);
  }
}

/*----------------------------------------------------------------------------*/

#if defined(__APPLE__) || defined(__MACH__)
#elif defined(__linux__) || defined(__gnu_linux__)
__cold clockid_t choice_monoclock(void) {
  struct timespec probe;
#if defined(CLOCK_BOOTTIME)
  if (clock_gettime(CLOCK_BOOTTIME, &probe) == 0)
    return CLOCK_BOOTTIME;
#elif defined(CLOCK_MONOTONIC_RAW)
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &probe) == 0)
    return CLOCK_MONOTONIC_RAW;
#elif defined(CLOCK_MONOTONIC_COARSE)
  if (clock_gettime(CLOCK_MONOTONIC_COARSE, &probe) == 0)
    return CLOCK_MONOTONIC_COARSE;
#endif
  return CLOCK_MONOTONIC;
}
#elif defined(CLOCK_MONOTONIC)
#define posix_clockid CLOCK_MONOTONIC
#else
#define posix_clockid CLOCK_REALTIME
#endif

uint64_t osal_16dot16_to_monotime(uint32_t seconds_16dot16) {
#if defined(__APPLE__) || defined(__MACH__)
  const uint64_t ratio = ratio_16dot16_to_monotonic;
#else
  const uint64_t ratio = UINT64_C(1000000000);
#endif
  const uint64_t ret = (ratio * seconds_16dot16 + 32768) >> 16;
  return likely(ret || seconds_16dot16 == 0) ? ret : /* fix underflow */ 1;
}

uint32_t osal_monotime_to_16dot16(uint64_t monotime) {
  if (unlikely(monotime > monotime_limit))
    return UINT32_MAX;

  const uint32_t ret =
#if defined(__APPLE__) || defined(__MACH__)
      (uint32_t)((monotime << 16) / ratio_16dot16_to_monotonic);
#else
      (uint32_t)((monotime << 7) / 1953125);
#endif
  return ret;
}

uint64_t osal_monotime(void) {
#if defined(__APPLE__) || defined(__MACH__)
  return mach_absolute_time();
#else
  struct timespec ts;
  if (likely(clock_gettime(posix_clockid, &ts) == 0))
    return ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
#endif
  return 0;
}

uint64_t osal_cputime(size_t *optional_page_faults) {
#if defined(RUSAGE_THREAD) || defined(RUSAGE_LWP)
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD RUSAGE_LWP /* Solaris */
#endif
  struct rusage usage;
  if (getrusage(RUSAGE_THREAD, &usage) == 0) {
    if (optional_page_faults)
      *optional_page_faults = usage.ru_majflt;
    return usage.ru_utime.tv_sec * UINT64_C(1000000000) + usage.ru_utime.tv_usec * 1000u;
  }
  if (optional_page_faults)
    *optional_page_faults = 0;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
  if (optional_page_faults)
    *optional_page_faults = 0;
  struct timespec ts;
  if (likely(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0))
    return ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
#else
  /* FIXME */
  if (optional_page_faults)
    *optional_page_faults = 0;
#endif
  return 0;
}

/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

#ifdef __FreeBSD__
#include <sys/uuid.h>
#endif /* FreeBSD */

#ifdef __IPHONE_OS_VERSION_MIN_REQUIRED
#include <CommonCrypto/CommonRandom.h>
#elif __GLIBC_PREREQ(2, 25) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__BSD__) || defined(__bsdi__) || \
    defined(__DragonFly__) || defined(__APPLE__) || __has_include(<sys/random.h>)
#include <sys/random.h>
#endif /* sys/random.h */

const char *osal_getenv_singlethreaded(const char *name, bool secure) {
  (void)secure;
#if defined(_GNU_SOURCE) && __GLIBC_PREREQ(2, 17)
  if (secure)
    return secure_getenv(name);
#endif /* glibc >= 2.17 */
  return getenv(name);
}

/*--------------------------------------------------------------------------*/
