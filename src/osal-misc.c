/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer
/// \file osal-misc.c — panic, asserts, string/memory helpers, monotonic clock,
/// jitter, boot-id/guid, sysram info and ctor/dtor.

/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer

#include "internals.h"

#if defined(__linux__) || defined(__gnu_linux__)
#include <sys/sysinfo.h>
#endif /* Linux */

#if defined(__APPLE__) || defined(__MACH__)
#include <mach/mach_time.h>
#include <mach/vm_statistics.h>
#endif /* Apple */

#if IS_WINDOWS

#include <psapi.h>
#include <winioctl.h>

#if !MDBX_WITHOUT_MSVC_CRT && defined(_DEBUG)
#include <crtdbg.h>
#endif

/* Map a result from an NTAPI call to WIN32 error code. */
static int osal_ntstatus2errcode(NTSTATUS status) {
  DWORD dummy;
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  ov.Internal = status;
  /* Zap: '_Param_(1)' could be '0' */
  MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6387);
  return GetOverlappedResult(nullptr, &ov, &dummy, FALSE) ? MDBX_SUCCESS : (int)GetLastError();
}

MDBX_INTERNAL int osal_waitstatus2errcode(DWORD result) {
  switch (result) {
  case WAIT_OBJECT_0:
    return MDBX_SUCCESS;
  case WAIT_FAILED:
    return (int)GetLastError();
  case WAIT_ABANDONED:
    return ERROR_ABANDONED_WAIT_0;
  case WAIT_IO_COMPLETION:
    return ERROR_USER_APC;
  case WAIT_TIMEOUT:
    return ERROR_TIMEOUT;
  default:
    return osal_ntstatus2errcode(result);
  }
}

/* We use native NT APIs to setup the memory map, so that we can
 * let the DB file grow incrementally instead of always preallocating
 * the full size. These APIs are defined in <wdm.h> and <ntifs.h>
 * but those headers are meant for driver-level development and
 * conflict with the regular user-level headers, so we explicitly
 * declare them here. Using these APIs also means we must link to
 * ntdll.dll, which is not linked by default in user code. */

extern NTSTATUS NTAPI NtCreateSection(OUT PHANDLE SectionHandle, IN ACCESS_MASK DesiredAccess,
                                      IN OPTIONAL POBJECT_ATTRIBUTES ObjectAttributes,
                                      IN OPTIONAL PLARGE_INTEGER MaximumSize, IN ULONG SectionPageProtection,
                                      IN ULONG AllocationAttributes, IN OPTIONAL HANDLE FileHandle);

typedef struct _SECTION_BASIC_INFORMATION {
  ULONG Unknown;
  ULONG SectionAttributes;
  LARGE_INTEGER SectionSize;
} SECTION_BASIC_INFORMATION, *PSECTION_BASIC_INFORMATION;

extern NTSTATUS NTAPI NtMapViewOfSection(IN HANDLE SectionHandle, IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress,
                                         IN ULONG_PTR ZeroBits, IN SIZE_T CommitSize,
                                         IN OUT OPTIONAL PLARGE_INTEGER SectionOffset, IN OUT PSIZE_T ViewSize,
                                         IN SECTION_INHERIT InheritDisposition, IN ULONG AllocationType,
                                         IN ULONG Win32Protect);

extern NTSTATUS NTAPI NtUnmapViewOfSection(IN HANDLE ProcessHandle, IN OPTIONAL PVOID BaseAddress);

/* Zap: Inconsistent annotation for 'NtClose'... */
MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(28251)
extern NTSTATUS NTAPI NtClose(HANDLE Handle);

extern NTSTATUS NTAPI NtAllocateVirtualMemory(IN HANDLE ProcessHandle, IN OUT PVOID *BaseAddress, IN ULONG_PTR ZeroBits,
                                              IN OUT PSIZE_T RegionSize, IN ULONG AllocationType, IN ULONG Protect);

extern NTSTATUS NTAPI NtFreeVirtualMemory(IN HANDLE ProcessHandle, IN PVOID *BaseAddress, IN OUT PSIZE_T RegionSize,
                                          IN ULONG FreeType);

#ifndef WOF_CURRENT_VERSION
typedef struct _WOF_EXTERNAL_INFO {
  DWORD Version;
  DWORD Provider;
} WOF_EXTERNAL_INFO, *PWOF_EXTERNAL_INFO;
#endif /* WOF_CURRENT_VERSION */

#ifndef WIM_PROVIDER_CURRENT_VERSION
#define WIM_PROVIDER_HASH_SIZE 20

typedef struct _WIM_PROVIDER_EXTERNAL_INFO {
  DWORD Version;
  DWORD Flags;
  LARGE_INTEGER DataSourceId;
  BYTE ResourceHash[WIM_PROVIDER_HASH_SIZE];
} WIM_PROVIDER_EXTERNAL_INFO, *PWIM_PROVIDER_EXTERNAL_INFO;
#endif /* WIM_PROVIDER_CURRENT_VERSION */

#ifndef FILE_PROVIDER_CURRENT_VERSION
typedef struct _FILE_PROVIDER_EXTERNAL_INFO_V1 {
  ULONG Version;
  ULONG Algorithm;
  ULONG Flags;
} FILE_PROVIDER_EXTERNAL_INFO_V1, *PFILE_PROVIDER_EXTERNAL_INFO_V1;
#endif /* FILE_PROVIDER_CURRENT_VERSION */

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

#else  /* Windows */
#endif /* !Windows */

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
#if IS_WINDOWS
#if defined(_DEBUG) && !MDBX_WITHOUT_MSVC_CRT
    _CrtDbgReport(_CRT_ASSERT, func, line, "libmdbx", "assertion failed: %s", msg);
#else
    char *message = nullptr;
    const int num = osal_asprintf(&message, "\r\nMDBX-ASSERTION: %s, %s:%u\r\n", msg, func, line);
    if (num < 1 || !message)
      message = "\r\n<asprintf() failed>\r\n";
    OutputDebugStringA(message);
#endif
    if (IsDebuggerPresent())
      DebugBreak();
    FatalExit(STATUS_ASSERTION_FAILURE);
#else
    __assert_fail(msg, "libmdbx", line, func);
    abort();
#endif
  }
}

/*----------------------------------------------------------------------------*/

#ifndef osal_vasprintf
int osal_vasprintf(char **strp, const char *fmt, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);
  const int needed = vsnprintf(nullptr, 0, fmt, ap_copy);
  va_end(ap_copy);

  if (unlikely(needed < 0 || needed >= INT_MAX)) {
    *strp = nullptr;
    return needed;
  }

  *strp = osal_malloc(needed + (size_t)1);
  if (unlikely(*strp == nullptr)) {
#if IS_WINDOWS
    SetLastError(MDBX_ENOMEM);
#else
    errno = MDBX_ENOMEM;
#endif
    return -1;
  }

  const int actual = vsnprintf(*strp, needed + (size_t)1, fmt, ap);
  ASSERT(actual == needed);
  if (unlikely(actual < 0)) {
    osal_free(*strp);
    *strp = nullptr;
  }
  return actual;
}
#endif /* osal_vasprintf */

#ifndef osal_asprintf
int osal_asprintf(char **strp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int rc = osal_vasprintf(strp, fmt, ap);
  va_end(ap);
  return rc;
}
#endif /* osal_asprintf */

/*----------------------------------------------------------------------------*/

__cold void osal_jitter(bool tiny) {
  for (;;) {
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    unsigned salt = 5296013u * (unsigned)__rdtsc();
    salt ^= salt >> 11;
    salt *= 25810541u;
#elif IS_WINDOWS && MDBX_WITHOUT_MSVC_CRT
    static ULONG state;
    const unsigned salt = (unsigned)RtlRandomEx(&state);
#else
    const unsigned salt = rand();
#endif

    const int coin = salt % (tiny ? 29u : 43u);
    if (coin < 43 / 3)
      break;
#if IS_WINDOWS
    if (coin < 43 * 2 / 3)
      osal_yield();
    else {
      static HANDLE timer;
      if (!timer)
        timer = CreateWaitableTimer(NULL, TRUE, NULL);

      LARGE_INTEGER ft;
      ft.QuadPart = coin * (int64_t)-10; // Convert to 100 nanosecond interval,
                                         // negative value indicates relative time.
      SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0);
      WaitForSingleObject(timer, INFINITE);
      // CloseHandle(timer);
      break;
    }
#else
    osal_yield();
    if (coin > 43 * 2 / 3)
      usleep(coin);
#endif
  }
}

/*----------------------------------------------------------------------------*/

#if IS_WINDOWS
static LARGE_INTEGER performance_frequency;
#elif defined(__APPLE__) || defined(__MACH__)
static uint64_t ratio_16dot16_to_monotonic;
#elif defined(__linux__) || defined(__gnu_linux__)
static clockid_t posix_clockid;
__cold static clockid_t choice_monoclock(void) {
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
#if IS_WINDOWS
  const uint64_t ratio = performance_frequency.QuadPart;
#elif defined(__APPLE__) || defined(__MACH__)
  const uint64_t ratio = ratio_16dot16_to_monotonic;
#else
  const uint64_t ratio = UINT64_C(1000000000);
#endif
  const uint64_t ret = (ratio * seconds_16dot16 + 32768) >> 16;
  return likely(ret || seconds_16dot16 == 0) ? ret : /* fix underflow */ 1;
}

static uint64_t monotime_limit;
uint32_t osal_monotime_to_16dot16(uint64_t monotime) {
  if (unlikely(monotime > monotime_limit))
    return UINT32_MAX;

  const uint32_t ret =
#if IS_WINDOWS
      (uint32_t)((monotime << 16) / performance_frequency.QuadPart);
#elif defined(__APPLE__) || defined(__MACH__)
      (uint32_t)((monotime << 16) / ratio_16dot16_to_monotonic);
#else
      (uint32_t)((monotime << 7) / 1953125);
#endif
  return ret;
}

uint64_t osal_monotime(void) {
#if IS_WINDOWS
  LARGE_INTEGER counter;
  if (QueryPerformanceCounter(&counter))
    return counter.QuadPart;
#elif defined(__APPLE__) || defined(__MACH__)
  return mach_absolute_time();
#else
  struct timespec ts;
  if (likely(clock_gettime(posix_clockid, &ts) == 0))
    return ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
#endif
  return 0;
}

uint64_t osal_cputime(size_t *optional_page_faults) {
#if IS_WINDOWS
  if (optional_page_faults) {
    PROCESS_MEMORY_COUNTERS pmc;
    *optional_page_faults = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.PageFaultCount : 0;
  }
  FILETIME unused, usermode;
  if (GetThreadTimes(GetCurrentThread(),
                     /* CreationTime */ &unused,
                     /* ExitTime */ &unused,
                     /* KernelTime */ &unused,
                     /* UserTime */ &usermode)) {
    /* one second = 10_000_000 * 100ns = 78125 * (1 << 7) * 100ns;
     * result = (h * f / 10_000_000) << 32) + l * f / 10_000_000 =
     *        = ((h * f) >> 7) / 78125) << 32) + ((l * f) >> 7) / 78125;
     * 1) {h, l} *= f;
     * 2) {h, l} >>= 7;
     * 3) result = ((h / 78125) << 32) + l / 78125; */
    uint64_t l = usermode.dwLowDateTime * performance_frequency.QuadPart;
    uint64_t h = usermode.dwHighDateTime * performance_frequency.QuadPart;
    l = h << (64 - 7) | l >> 7;
    h = h >> 7;
    return ((h / 78125) << 32) + l / 78125;
  }
#elif defined(RUSAGE_THREAD) || defined(RUSAGE_LWP)
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

static void bootid_shake(bin128_t *p) {
  /* Bob Jenkins's PRNG: https://burtleburtle.net/bob/rand/smallprng.html */
  const uint32_t e = p->a - (p->b << 23 | p->b >> 9);
  p->a = p->b ^ (p->c << 16 | p->c >> 16);
  p->b = p->c + (p->d << 11 | p->d >> 21);
  p->c = p->d + e;
  p->d = e + p->a;
}

__cold static void bootid_collect(bin128_t *p, const void *s, size_t n) {
  p->y += UINT64_C(64526882297375213);
  bootid_shake(p);
  for (size_t i = 0; i < n; ++i) {
    bootid_shake(p);
    p->y ^= UINT64_C(48797879452804441) * ((const uint8_t *)s)[i];
    bootid_shake(p);
    p->y += 14621231;
  }
  bootid_shake(p);

  /* minor non-linear tomfoolery */
  const unsigned z = p->x % 61 + 1;
  p->y = p->y << z | p->y >> (64 - z);
  bootid_shake(p);
  bootid_shake(p);
  const unsigned q = p->x % 59 + 1;
  p->y = p->y << q | p->y >> (64 - q);
  bootid_shake(p);
  bootid_shake(p);
  bootid_shake(p);
}

static size_t hamming_weight(size_t v) {
  const size_t m1 = (size_t)UINT64_C(0x5555555555555555);
  const size_t m2 = (size_t)UINT64_C(0x3333333333333333);
  const size_t m4 = (size_t)UINT64_C(0x0f0f0f0f0f0f0f0f);
  const size_t h01 = (size_t)UINT64_C(0x0101010101010101);
  v -= (v >> 1) & m1;
  v = (v & m2) + ((v >> 2) & m2);
  v = (v + (v >> 4)) & m4;
  return (v * h01) >> (sizeof(v) * 8 - 8);
}

static inline size_t hw64(uint64_t v) {
  size_t r = hamming_weight((size_t)v);
  if (sizeof(v) > sizeof(r))
    r += hamming_weight((size_t)(v >> sizeof(r) * 4 >> sizeof(r) * 4));
  return r;
}

static bool check_uuid(bin128_t uuid) {
  size_t hw = hw64(uuid.x) + hw64(uuid.y) + hw64(uuid.x ^ uuid.y);
  return (hw >> 6) == 1;
}

#if IS_WINDOWS

__cold static uint64_t windows_systemtime_ms() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000ul;
}

__cold static uint64_t windows_bootime(void) {
  unsigned confirmed = 0;
  uint64_t boottime = 0;
  uint64_t up0 = imports.GetTickCount64();
  uint64_t st0 = windows_systemtime_ms();
  for (uint64_t fuse = st0; up0 && st0 < fuse + 1000 * 1000u / 42;) {
    YieldProcessor();
    const uint64_t up1 = imports.GetTickCount64();
    const uint64_t st1 = windows_systemtime_ms();
    if (st1 > fuse && st1 == st0 && up1 == up0) {
      uint64_t diff = st1 - up1;
      if (boottime == diff) {
        if (++confirmed > 4)
          return boottime;
      } else {
        confirmed = 0;
        boottime = diff;
      }
      fuse = st1;
      Sleep(1);
    }
    st0 = st1;
    up0 = up1;
  }
  return 0;
}

__cold static LSTATUS mdbx_RegGetValue(HKEY hKey, LPCSTR lpSubKey, LPCSTR lpValue, PVOID pvData, LPDWORD pcbData) {
  LSTATUS rc;
  if (!imports.RegGetValueA) {
    /* an old Windows 2000/XP */
    HKEY hSubKey;
    rc = RegOpenKeyA(hKey, lpSubKey, &hSubKey);
    if (rc == ERROR_SUCCESS) {
      rc = RegQueryValueExA(hSubKey, lpValue, nullptr, nullptr, pvData, pcbData);
      RegCloseKey(hSubKey);
    }
    return rc;
  }

  rc = imports.RegGetValueA(hKey, lpSubKey, lpValue, RRF_RT_ANY, nullptr, pvData, pcbData);
  if (rc != ERROR_FILE_NOT_FOUND)
    return rc;

  rc = imports.RegGetValueA(hKey, lpSubKey, lpValue, RRF_RT_ANY | 0x00010000 /* RRF_SUBKEY_WOW6464KEY */, nullptr,
                            pvData, pcbData);
  if (rc != ERROR_FILE_NOT_FOUND)
    return rc;
  return imports.RegGetValueA(hKey, lpSubKey, lpValue, RRF_RT_ANY | 0x00020000 /* RRF_SUBKEY_WOW6432KEY */, nullptr,
                              pvData, pcbData);
}
#endif

MDBX_MAYBE_UNUSED __cold static bool bootid_parse_uuid(bin128_t *s, const void *p, const size_t n) {
  if (n > 31) {
    unsigned bits = 0;
    for (unsigned i = 0; i < n; ++i) /* try parse an UUID in text form */ {
      uint8_t c = ((const uint8_t *)p)[i];
      if (c >= '0' && c <= '9')
        c -= '0';
      else if (c >= 'a' && c <= 'f')
        c -= 'a' - 10;
      else if (c >= 'A' && c <= 'F')
        c -= 'A' - 10;
      else
        continue;
      ASSERT(c <= 15);
      c ^= s->y >> 60;
      s->y = s->y << 4 | s->x >> 60;
      s->x = s->x << 4 | c;
      bits += 4;
    }
    if (bits > 42 * 3)
      /* UUID parsed successfully */
      return true;
  }

  if (n > 15) /* is enough handle it as a binary? */ {
    if (n == sizeof(bin128_t)) {
      bin128_t aligned;
      memcpy(&aligned, p, sizeof(bin128_t));
      s->x += aligned.x;
      s->y += aligned.y;
    } else
      bootid_collect(s, p, n);
    return check_uuid(*s);
  }

  if (n)
    bootid_collect(s, p, n);
  return false;
}

#if defined(__linux__) || defined(__gnu_linux__)

__cold static bool is_inside_lxc(void) {
  bool inside_lxc = false;
  FILE *mounted = setmntent("/proc/mounts", "r");
  if (mounted) {
    const struct mntent *ent;
    while (nullptr != (ent = getmntent(mounted))) {
      if (strcmp(ent->mnt_fsname, "lxcfs") == 0 && strncmp(ent->mnt_dir, "/proc/", 6) == 0) {
        inside_lxc = true;
        break;
      }
    }
    endmntent(mounted);
  }
  return inside_lxc;
}

__cold static bool proc_read_uuid(const char *path, bin128_t *target) {
  const int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd != -1) {
    struct statfs fs;
    char buf[42];
    const ssize_t len = (fstatfs(fd, &fs) == 0 &&
                         (fs.f_type == /* procfs */ 0x9FA0 || (fs.f_type == /* tmpfs */ 0x1021994 && is_inside_lxc())))
                            ? read(fd, buf, sizeof(buf))
                            : -1;
    const int err = close(fd);
    ASSERT(err == 0);
    (void)err;
    if (len > 0)
      return bootid_parse_uuid(target, buf, len);
  }
  return false;
}
#endif /* Linux */

__cold static bin128_t osal_bootid(void) {
  bin128_t uuid = {{0, 0}};
  bool got_machineid = false, got_boottime = false, got_bootseq = false;

#if defined(__linux__) || defined(__gnu_linux__)
  if (proc_read_uuid("/proc/sys/kernel/random/boot_id", &uuid))
    return uuid;
#endif /* Linux */

#if defined(__APPLE__) || defined(__MACH__)
  {
    char buf[42];
    size_t len = sizeof(buf);
    if (!sysctlbyname("kern.bootsessionuuid", buf, &len, nullptr, 0) && bootid_parse_uuid(&uuid, buf, len))
      return uuid;

#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED > 1050
    uuid_t hostuuid;
    struct timespec wait = {0, 1000000000u / 42};
    if (!gethostuuid(hostuuid, &wait))
      got_machineid = bootid_parse_uuid(&uuid, hostuuid, sizeof(hostuuid));
#endif /* > 10.5 */

    struct timeval boottime;
    len = sizeof(boottime);
    if (!sysctlbyname("kern.boottime", &boottime, &len, nullptr, 0) && len == sizeof(boottime) && boottime.tv_sec)
      got_boottime = true;
  }
#endif /* Apple/Darwin */

#if IS_WINDOWS
  {
    union buf {
      DWORD BootId;
      DWORD BaseTime;
      SYSTEM_TIMEOFDAY_INFORMATION SysTimeOfDayInfo;
      struct {
        LARGE_INTEGER BootTime;
        LARGE_INTEGER CurrentTime;
        LARGE_INTEGER TimeZoneBias;
        ULONG TimeZoneId;
        ULONG Reserved;
        ULONGLONG BootTimeBias;
        ULONGLONG SleepTimeBias;
      } SysTimeOfDayInfoHacked;
      wchar_t MachineGuid[42];
      char DigitalProductId[248];
    } buf;

    static const char HKLM_MicrosoftCryptography[] = "SOFTWARE\\Microsoft\\Cryptography";
    DWORD len = sizeof(buf);
    /* Windows is madness and must die */
    if (mdbx_RegGetValue(HKEY_LOCAL_MACHINE, HKLM_MicrosoftCryptography, "MachineGuid", &buf.MachineGuid, &len) ==
            ERROR_SUCCESS &&
        len < sizeof(buf))
      got_machineid = bootid_parse_uuid(&uuid, &buf.MachineGuid, len);

    if (!got_machineid) {
      /* again, Windows is madness */
      static const char HKLM_WindowsNT[] = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
      static const char HKLM_WindowsNT_DPK[] = "SOFTWARE\\Microsoft\\Windows "
                                               "NT\\CurrentVersion\\DefaultProductKey";
      static const char HKLM_WindowsNT_DPK2[] = "SOFTWARE\\Microsoft\\Windows "
                                                "NT\\CurrentVersion\\DefaultProductKey2";

      len = sizeof(buf);
      if (mdbx_RegGetValue(HKEY_LOCAL_MACHINE, HKLM_WindowsNT, "DigitalProductId", &buf.DigitalProductId, &len) ==
              ERROR_SUCCESS &&
          len > 42 && len < sizeof(buf)) {
        bootid_collect(&uuid, &buf.DigitalProductId, len);
        got_machineid = true;
      }
      len = sizeof(buf);
      if (mdbx_RegGetValue(HKEY_LOCAL_MACHINE, HKLM_WindowsNT_DPK, "DigitalProductId", &buf.DigitalProductId, &len) ==
              ERROR_SUCCESS &&
          len > 42 && len < sizeof(buf)) {
        bootid_collect(&uuid, &buf.DigitalProductId, len);
        got_machineid = true;
      }
      len = sizeof(buf);
      if (mdbx_RegGetValue(HKEY_LOCAL_MACHINE, HKLM_WindowsNT_DPK2, "DigitalProductId", &buf.DigitalProductId, &len) ==
              ERROR_SUCCESS &&
          len > 42 && len < sizeof(buf)) {
        bootid_collect(&uuid, &buf.DigitalProductId, len);
        got_machineid = true;
      }
    }

    static const char HKLM_PrefetcherParams[] = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory "
                                                "Management\\PrefetchParameters";
    len = sizeof(buf);
    if (mdbx_RegGetValue(HKEY_LOCAL_MACHINE, HKLM_PrefetcherParams, "BootId", &buf.BootId, &len) == ERROR_SUCCESS &&
        len > 1 && len < sizeof(buf)) {
      bootid_collect(&uuid, &buf.BootId, len);
      got_bootseq = true;
    }

    len = sizeof(buf);
    if (mdbx_RegGetValue(HKEY_LOCAL_MACHINE, HKLM_PrefetcherParams, "BaseTime", &buf.BaseTime, &len) == ERROR_SUCCESS &&
        len >= sizeof(buf.BaseTime) && buf.BaseTime) {
      bootid_collect(&uuid, &buf.BaseTime, len);
      got_boottime = true;
    }

    /* BootTime from SYSTEM_TIMEOFDAY_INFORMATION */
    NTSTATUS status = NtQuerySystemInformation(0x03 /* SystemTimeOfDayInformation */, &buf.SysTimeOfDayInfo,
                                               sizeof(buf.SysTimeOfDayInfo), &len);
    if (NT_SUCCESS(status) &&
        len >= offsetof(union buf, SysTimeOfDayInfoHacked.BootTimeBias) +
                   sizeof(buf.SysTimeOfDayInfoHacked.BootTimeBias) &&
        buf.SysTimeOfDayInfoHacked.BootTime.QuadPart) {
      const uint64_t UnbiasedBootTime =
          buf.SysTimeOfDayInfoHacked.BootTime.QuadPart - buf.SysTimeOfDayInfoHacked.BootTimeBias;
      if (UnbiasedBootTime) {
        bootid_collect(&uuid, &UnbiasedBootTime, sizeof(UnbiasedBootTime));
        got_boottime = true;
      }
    }

    if (!got_boottime) {
      uint64_t boottime = windows_bootime();
      if (boottime) {
        bootid_collect(&uuid, &boottime, sizeof(boottime));
        got_boottime = true;
      }
    }
  }
#endif /* Windows */

#if defined(CTL_HW) && defined(HW_UUID)
  if (!got_machineid) {
    static const int mib[] = {CTL_HW, HW_UUID};
    char buf[42];
    size_t len = sizeof(buf);
    if (sysctl(
#ifdef SYSCTL_LEGACY_NONCONST_MIB
            (int *)
#endif
                mib,
            ARRAY_LENGTH(mib), &buf, &len, nullptr, 0) == 0)
      got_machineid = bootid_parse_uuid(&uuid, buf, len);
  }
#endif /* CTL_HW && HW_UUID */

#if defined(CTL_KERN) && defined(KERN_HOSTUUID)
  if (!got_machineid) {
    static const int mib[] = {CTL_KERN, KERN_HOSTUUID};
    char buf[42];
    size_t len = sizeof(buf);
    if (sysctl(
#ifdef SYSCTL_LEGACY_NONCONST_MIB
            (int *)
#endif
                mib,
            ARRAY_LENGTH(mib), &buf, &len, nullptr, 0) == 0)
      got_machineid = bootid_parse_uuid(&uuid, buf, len);
  }
#endif /* CTL_KERN && KERN_HOSTUUID */

#if defined(__NetBSD__)
  if (!got_machineid) {
    char buf[42];
    size_t len = sizeof(buf);
    if (sysctlbyname("machdep.dmi.system-uuid", buf, &len, nullptr, 0) == 0)
      got_machineid = bootid_parse_uuid(&uuid, buf, len);
  }
#endif /* __NetBSD__ */

#if !IS_WINDOWS
  if (!got_machineid) {
    int fd = open("/etc/machine-id", O_RDONLY);
    if (fd == -1)
      fd = open("/var/lib/dbus/machine-id", O_RDONLY);
    if (fd != -1) {
      char buf[42];
      const ssize_t len = read(fd, buf, sizeof(buf));
      const int err = close(fd);
      ASSERT(err == 0);
      (void)err;
      if (len > 0)
        got_machineid = bootid_parse_uuid(&uuid, buf, len);
    }
  }
#endif /* !Windows */

#if _XOPEN_SOURCE_EXTENDED
  if (!got_machineid) {
    const long hostid = gethostid();
    if (hostid != 0 && hostid != -1) {
      bootid_collect(&uuid, &hostid, sizeof(hostid));
      got_machineid = true;
    }
  }
#endif /* _XOPEN_SOURCE_EXTENDED */

  if (!got_machineid) {
  lack:
    uuid.x = uuid.y = 0;
    return uuid;
  }

  /*--------------------------------------------------------------------------*/

#if defined(CTL_KERN) && defined(KERN_BOOTTIME)
  if (!got_boottime) {
    static const int mib[] = {CTL_KERN, KERN_BOOTTIME};
    struct timeval boottime;
    size_t len = sizeof(boottime);
    if (sysctl(
#ifdef SYSCTL_LEGACY_NONCONST_MIB
            (int *)
#endif
                mib,
            ARRAY_LENGTH(mib), &boottime, &len, nullptr, 0) == 0 &&
        len == sizeof(boottime) && boottime.tv_sec) {
      bootid_collect(&uuid, &boottime, len);
      got_boottime = true;
    }
  }
#endif /* CTL_KERN && KERN_BOOTTIME */

#if defined(__sun) || defined(__SVR4) || defined(__svr4__)
  if (!got_boottime) {
    kstat_ctl_t *kc = kstat_open();
    if (kc) {
      kstat_t *kp = kstat_lookup(kc, "unix", 0, "system_misc");
      if (kp && kstat_read(kc, kp, 0) != -1) {
        kstat_named_t *kn = (kstat_named_t *)kstat_data_lookup(kp, "boot_time");
        if (kn) {
          switch (kn->data_type) {
          case KSTAT_DATA_INT32:
          case KSTAT_DATA_UINT32:
            bootid_collect(&uuid, &kn->value, sizeof(int32_t));
            got_boottime = true;
            break;
          case KSTAT_DATA_INT64:
          case KSTAT_DATA_UINT64:
            bootid_collect(&uuid, &kn->value, sizeof(int64_t));
            got_boottime = true;
            break;
          }
        }
      }
      kstat_close(kc);
    }
  }
#endif /* SunOS / Solaris */

#if _XOPEN_SOURCE_EXTENDED && defined(BOOT_TIME)
  if (!got_boottime) {
    setutxent();
    const struct utmpx id = {.ut_type = BOOT_TIME};
    const struct utmpx *entry = getutxid(&id);
    if (entry) {
      bootid_collect(&uuid, entry, sizeof(*entry));
      got_boottime = true;
      while (unlikely((entry = getutxid(&id)) != nullptr)) {
        /* have multiple reboot records, assuming we can distinguish next
         * bootsession even if RTC is wrong or absent */
        bootid_collect(&uuid, entry, sizeof(*entry));
        got_bootseq = true;
      }
    }
    endutxent();
  }
#endif /* _XOPEN_SOURCE_EXTENDED && BOOT_TIME */

  if (!got_bootseq) {
    if (!got_boottime || !MDBX_TRUST_RTC)
      goto lack;

#if IS_WINDOWS
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    if (0x1CCCCCC > now.dwHighDateTime)
#else
    struct timespec mono, real;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) || clock_gettime(CLOCK_REALTIME, &real) ||
        /* wrong time, RTC is mad or absent */
        1555555555l > real.tv_sec ||
        /* seems no adjustment by RTC/NTP, i.e. a fake time */
        real.tv_sec < mono.tv_sec || 1234567890l > real.tv_sec - mono.tv_sec || (real.tv_sec - mono.tv_sec) % 900u == 0)
#endif
      goto lack;
  }

  return uuid;
}

#if defined(__linux__) || defined(__gnu_linux__)
static intptr_t proc_meminfo_availpages(void) {
  intptr_t availpages = -1;
  FILE *const proc_meminfo = fopen("/proc/meminfo", "r");
  if (!proc_meminfo)
    return availpages;

  char buf[64], *line;
  while ((line = fgets(buf, sizeof(buf), proc_meminfo)) != nullptr) {
    if (strncmp("MemAvailable:", line, 13))
      continue;
    size_t value = 0;
    buf[0] = 0;
    if (sscanf(line + 13, "%zu %s", &value, buf) < 1)
      availpages = -1;
    else {
      if (!buf[0] || strcasecmp(buf, "b") == 0 || strcasecmp(buf, "bytes") == 0)
        availpages = value >> globals.sys_pagesize_ln2;
      else if (strcasecmp(buf, "kb") == 0 || strcasecmp(buf, "kbytes") == 0)
        availpages = (value << 10) >> globals.sys_pagesize_ln2;
      else if (strcasecmp(buf, "mb") == 0 || strcasecmp(buf, "mbytes") == 0)
        availpages = (value << 20) >> globals.sys_pagesize_ln2;
    }
    break;
  }
  fclose(proc_meminfo);
  return availpages;
}
#endif /* Linux */

__cold int mdbx_get_sysraminfo(intptr_t *page_size, intptr_t *total_pages, intptr_t *avail_pages) {
  if (!page_size && !total_pages && !avail_pages)
    return LOG_IFERR(MDBX_EINVAL);
  if (total_pages)
    *total_pages = -1;
  if (avail_pages)
    *avail_pages = -1;

  const intptr_t pagesize = globals.sys_pagesize;
  if (page_size)
    *page_size = pagesize;
  if (unlikely(pagesize < MDBX_MIN_PAGESIZE || !is_powerof2(pagesize)))
    return LOG_IFERR(MDBX_INCOMPATIBLE);

  const int log2page = globals.sys_pagesize_ln2;
  ASSERT(pagesize == (INT64_C(1) << log2page));
  (void)log2page;

#if defined(__linux__) || defined(__gnu_linux__)
  struct sysinfo si;
  if (sysinfo(&si) == 0) {
    if (total_pages)
      *total_pages = si.totalram >> log2page;
    if (avail_pages) {
      *avail_pages = proc_meminfo_availpages();
      if (*avail_pages < 0)
        *avail_pages = si.freeram >> log2page;
    }
    return MDBX_SUCCESS;
  }
#endif /* Linux */

#if IS_WINDOWS
  MEMORYSTATUSEX info;
  memset(&info, 0, sizeof(info));
  info.dwLength = sizeof(info);
  if (!GlobalMemoryStatusEx(&info))
    return LOG_IFERR((int)GetLastError());
#endif

  if (total_pages) {
#if IS_WINDOWS
    const intptr_t total_ram_pages = (intptr_t)(info.ullTotalPhys >> log2page);
#elif defined(_SC_PHYS_PAGES)
    const intptr_t total_ram_pages = sysconf(_SC_PHYS_PAGES);
    if (total_ram_pages == -1)
      return LOG_IFERR(errno);
#elif defined(_SC_AIX_REALMEM)
    const intptr_t total_ram_Kb = sysconf(_SC_AIX_REALMEM);
    if (total_ram_Kb == -1)
      return LOG_IFERR(errno);
    const intptr_t total_ram_pages = (total_ram_Kb << 10) >> log2page;
#elif defined(HW_USERMEM) || defined(HW_PHYSMEM64) || defined(HW_MEMSIZE) || defined(HW_PHYSMEM)
    size_t ram, len = sizeof(ram);
    static const int mib[] = {CTL_HW,
#if defined(HW_USERMEM)
                              HW_USERMEM
#elif defined(HW_PHYSMEM64)
                              HW_PHYSMEM64
#elif defined(HW_MEMSIZE)
                              HW_MEMSIZE
#else
                              HW_PHYSMEM
#endif
    };
    if (sysctl(
#ifdef SYSCTL_LEGACY_NONCONST_MIB
            (int *)
#endif
                mib,
            ARRAY_LENGTH(mib), &ram, &len, nullptr, 0) != 0)
      return LOG_IFERR(errno);
    if (len != sizeof(ram))
      return LOG_IFERR(MDBX_ENOSYS);
    const intptr_t total_ram_pages = (intptr_t)(ram >> log2page);
#else
#error "FIXME: Get User-accessible or physical RAM"
#endif
    *total_pages = total_ram_pages;
    if (total_ram_pages < 1)
      return LOG_IFERR(MDBX_ENOSYS);
  }

  if (avail_pages) {
#if IS_WINDOWS
    const intptr_t avail_ram_pages = (intptr_t)(info.ullAvailPhys >> log2page);
#elif defined(_SC_AVPHYS_PAGES)
    const intptr_t avail_ram_pages = sysconf(_SC_AVPHYS_PAGES);
    if (avail_ram_pages == -1)
      return LOG_IFERR(errno);
#elif defined(__MACH__)
    mach_msg_type_number_t count;
    mach_port_t mport = mach_host_self();
#if defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 100600
    struct vm_statistics64 vmstat;
    count = HOST_VM_INFO64_COUNT;
    kern_return_t kerr = host_statistics64(mport, HOST_VM_INFO64, (host_info64_t)&vmstat, &count);
    const intptr_t avail_ram_pages = vmstat.free_count + vmstat.purgeable_count;
#else
    vm_statistics_data_t vmstat;
    count = HOST_VM_INFO_COUNT;
    kern_return_t kerr = host_statistics(mport, HOST_VM_INFO, (host_info_t)&vmstat, &count);
    const intptr_t avail_ram_pages = vmstat.free_count;
#endif
    mach_port_deallocate(mach_task_self(), mport);
    if (unlikely(kerr != KERN_SUCCESS))
      return LOG_IFERR(MDBX_ENOSYS);
#elif defined(VM_TOTAL) || defined(VM_METER)
    struct vmtotal info;
    size_t len = sizeof(info);
    static const int mib[] = {CTL_VM,
#if defined(VM_TOTAL)
                              VM_TOTAL
#elif defined(VM_METER)
                              VM_METER
#endif
    };
    if (sysctl(
#ifdef SYSCTL_LEGACY_NONCONST_MIB
            (int *)
#endif
                mib,
            ARRAY_LENGTH(mib), &info, &len, nullptr, 0) != 0)
      return LOG_IFERR(errno);
    if (len != sizeof(info))
      return LOG_IFERR(MDBX_ENOSYS);
    const intptr_t avail_ram_pages = info.t_free + info.t_inact / 2;
#else
#error "FIXME: Get Available RAM"
#endif
    *avail_pages = avail_ram_pages;
    if (avail_ram_pages < 1)
      return LOG_IFERR(MDBX_ENOSYS);
  }

  return MDBX_SUCCESS;
}

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

#if IS_WINDOWS
#include <wincrypt.h>
#endif /* Windows */

bin128_t osal_guid(const MDBX_env *env) {
  struct {
    uint64_t begin, end, cputime;
    uintptr_t thread, pid;
    const void *x, *y;
    bin128_t (*z)(const MDBX_env *env);
  } salt;

  salt.begin = osal_monotime();
  bin128_t uuid = {{0, 0}};

#if defined(__linux__) || defined(__gnu_linux__)
  if (proc_read_uuid("/proc/sys/kernel/random/uuid", &uuid) && check_uuid(uuid))
    return uuid;
#endif /* Linux */

#ifdef __FreeBSD__
  STATIC_ASSERT(sizeof(uuid) == sizeof(struct uuid));
  if (uuidgen((struct uuid *)&uuid, 1) == 0 && check_uuid(uuid))
    return uuid;
#endif /* FreeBSD */

#if IS_WINDOWS
  if (imports.CoCreateGuid && imports.CoCreateGuid(&uuid) == 0 && check_uuid(uuid))
    return uuid;

  HCRYPTPROV hCryptProv = 0;
  if (CryptAcquireContextW(&hCryptProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
    const BOOL ok = CryptGenRandom(hCryptProv, sizeof(uuid), (unsigned char *)&uuid);
    CryptReleaseContext(hCryptProv, 0);
    if (ok && check_uuid(uuid))
      return uuid;
  }
#elif defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && defined(__IPHONE_8_0)
#if __IPHONE_OS_VERSION_MIN_REQUIRED >= __IPHONE_8_0
  if (CCRandomGenerateBytes(&uuid, sizeof(uuid)) == kCCSuccess && check_uuid(uuid))
    return uuid;
#endif /* iOS >= 8.x */
#else
  const int fd = open("/dev/urandom", O_RDONLY);
  if (fd != -1) {
    const ssize_t len = read(fd, &uuid, sizeof(uuid));
    const int err = close(fd);
    ASSERT(err == 0);
    (void)err;
    if (len == sizeof(uuid) && check_uuid(uuid))
      return uuid;
  }
#if (__GLIBC_PREREQ(2, 25) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__BSD__) || defined(__bsdi__) ||  \
     defined(__DragonFly__)) &&                                                                                        \
    !defined(__APPLE__) && !defined(__ANDROID_API__)
  if (getrandom(&uuid, sizeof(uuid), 0) == sizeof(uuid) && check_uuid(uuid))
    return uuid;
#elif defined(__OpenBSD__) || (defined(__sun) && defined(__SVR4)) ||                                                   \
    (defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 101200)
  if (getentropy(&uuid, sizeof(uuid)) == 0 && check_uuid(uuid))
    return uuid;
#endif /* getrandom() / getentropy() */
#endif /* !Windows */

  uuid = globals.bootid;
  bootid_collect(&uuid, env, sizeof(*env));
  salt.thread = osal_thread_self();
  salt.pid = osal_getpid();
  salt.x = &salt;
  salt.y = env;
  salt.z = &osal_guid;
  do {
    salt.cputime = osal_cputime(nullptr);
    salt.end = osal_monotime();
    bootid_collect(&uuid, &salt, sizeof(salt));
  } while (!check_uuid(uuid));
  return uuid;
}

const char *osal_getenv_singlethreaded(const char *name, bool secure) {
  (void)secure;
#if IS_WINDOWS
  /* We sure this function never calls in a multithreaded cases, since used at initialization stage only.
   * NB: values longer than the previous fixed 42-byte buffer (e.g. long build paths via MDBX_PROBE_CTL)
   * were silently rejected; query the required size first and grow the static buffer accordingly. */
  static char *buf = nullptr;
  SetLastError(ERROR_OUT_OF_PAPER);
  const DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
  if (size == 0) {
    switch (GetLastError()) {
    case ERROR_OUT_OF_PAPER:
      return "";
    default:
      /* no idea to do in case of other error */
    case ERROR_ENVVAR_NOT_FOUND:
      return nullptr;
    }
  }
  if (size == 1)
    return "" /* the variable exists with an empty value */;
#if defined(MDBX_PROBES)
  /*> dist-cutoff-begin */
  /* NB: under MDBX_PROBES, osal_realloc is redirected to mprobe_realloc()
   * which re-enters mprobe_is_active() -> mprobe_init_activation() while
   * mprobe_reg_lock is held by the outer mprobe_init_activation() call ->
   * self-deadlock. Use the probe-bus's own raw allocator to avoid the
   * redirect (see osal.h "probe-bus allocation interception"). */
  char *const grown = (char *)osal_realloc_raw(buf, size);
  /*< dist-cutoff-end */
#else
  char *const grown = (char *)osal_realloc(buf, size);
#endif
  if (!grown)
    return nullptr;
  buf = grown;
  if (GetEnvironmentVariableA(name, buf, size) == 0)
    return nullptr;
  return buf;
#else
#if defined(_GNU_SOURCE) && __GLIBC_PREREQ(2, 17)
  if (secure)
    return secure_getenv(name);
#endif /* glibc >= 2.17 */
  return getenv(name);
#endif
}

#if MDBX_HAVE_PWRITEV && (defined(_SC_IOV_MAX) || defined(ENABLE_MEMCHECK))
extern size_t osal_iov_max;
#endif /* MDBX_HAVE_PWRITEV */

/*--------------------------------------------------------------------------*/

void osal_ctor(void) {
#if MDBX_HAVE_PWRITEV && (defined(_SC_IOV_MAX) || defined(ENABLE_MEMCHECK))
#if defined(_SC_IOV_MAX)
  osal_iov_max = sysconf(_SC_IOV_MAX);
#else
  osal_iov_max = IOV_MAX;
#endif /* _SC_IOV_MAX */
  if (RUNNING_ON_VALGRIND && osal_iov_max > 64)
    /* чтобы не описывать все 1024 исключения в valgrind.supp */
    osal_iov_max = 64;
#endif /* MDBX_HAVE_PWRITEV */

#if IS_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  globals.sys_pagesize = si.dwPageSize;
  globals.sys_allocation_granularity =
      (si.dwAllocationGranularity > globals.sys_pagesize) ? si.dwAllocationGranularity : globals.sys_pagesize;
#else
  globals.sys_pagesize = sysconf(_SC_PAGE_SIZE);
  globals.sys_allocation_granularity = (MDBX_WORDBITS > 32) ? 65536 : 16384;
  globals.sys_allocation_granularity = (globals.sys_allocation_granularity >= globals.sys_pagesize * 2)
                                           ? globals.sys_allocation_granularity
                                           : globals.sys_pagesize * 4;
#ifdef AT_UCACHEBSIZE
  const size_t unified_cache_block_size = getauxval(AT_UCACHEBSIZE);
  globals.sys_unified_cache_block = globals.sys_pagesize;
  if (unified_cache_block_size > 0 && unified_cache_block_size < INT_MAX) {
    globals.sys_unified_cache_block = (unsigned)unified_cache_block_size;
    if (globals.sys_unified_cache_block > globals.sys_pagesize)
      globals.sys_allocation_granularity = globals.sys_unified_cache_block;
  }
#endif /* AT_UCACHEBSIZE */
#endif
  if (globals.sys_allocation_granularity > 4 * MEGABYTE && globals.sys_pagesize < MEGABYTE)
    globals.sys_allocation_granularity = 4 * MEGABYTE;
  ASSERT(globals.sys_pagesize > 0 && (globals.sys_pagesize & (globals.sys_pagesize - 1)) == 0);
  ASSERT(globals.sys_allocation_granularity >= globals.sys_pagesize &&
         globals.sys_allocation_granularity % globals.sys_pagesize == 0);
  globals.sys_pagesize_ln2 = (uint8_t)log2n_powerof2(globals.sys_pagesize);

#if defined(__linux__) || defined(__gnu_linux__)
  posix_clockid = choice_monoclock();
#endif

#if IS_WINDOWS
  QueryPerformanceFrequency(&performance_frequency);
#elif defined(__APPLE__) || defined(__MACH__)
  mach_timebase_info_data_t ti;
  mach_timebase_info(&ti);
  ratio_16dot16_to_monotonic = UINT64_C(1000000000) * ti.denom / ti.numer;
#endif
  monotime_limit = osal_16dot16_to_monotime(UINT32_MAX - 1);

  uint32_t proba = UINT32_MAX;
  while (true) {
    unsigned time_conversion_checkup = osal_monotime_to_16dot16(osal_16dot16_to_monotime(proba));
    unsigned one_more = (proba < UINT32_MAX) ? proba + 1 : proba;
    unsigned one_less = (proba > 0) ? proba - 1 : proba;
    ENSURE(time_conversion_checkup >= one_less && time_conversion_checkup <= one_more);
    if (proba == 0)
      break;
    proba >>= 1;
  }

  globals.bootid = osal_bootid();
}

void osal_dtor(void) {}
