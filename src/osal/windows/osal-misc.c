/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer
/// \file osal-misc.c (Windows part) — panic, asserts, string/memory helpers,
/// monotonic clock, jitter, boot-id/guid, sysram info and ctor/dtor.

#include "internals.h"

#include <psapi.h>
#include <winioctl.h>

#if !MDBX_WITHOUT_MSVC_CRT && defined(_DEBUG)
#include <crtdbg.h>
#endif

/* Map a result from an NTAPI call to WIN32 error code (used by osal-mmap.c too). */
MDBX_INTERNAL int osal_ntstatus2errcode(NTSTATUS status) {
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

/*----------------------------------------------------------------------------*/

__cold void osal_panic(const char *msg, const char *func, unsigned line) {
  while (1) {
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
    SetLastError(MDBX_ENOMEM);
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
#elif MDBX_WITHOUT_MSVC_CRT
    static ULONG state;
    const unsigned salt = (unsigned)RtlRandomEx(&state);
#else
    const unsigned salt = rand();
#endif

    const int coin = salt % (tiny ? 29u : 43u);
    if (coin < 43 / 3)
      break;
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
  }
}

/*----------------------------------------------------------------------------*/

static LARGE_INTEGER performance_frequency;

uint64_t osal_16dot16_to_monotime(uint32_t seconds_16dot16) {
  const uint64_t ratio = performance_frequency.QuadPart;
  const uint64_t ret = (ratio * seconds_16dot16 + 32768) >> 16;
  return likely(ret || seconds_16dot16 == 0) ? ret : /* fix underflow */ 1;
}

static uint64_t monotime_limit;
uint32_t osal_monotime_to_16dot16(uint64_t monotime) {
  if (unlikely(monotime > monotime_limit))
    return UINT32_MAX;

  const uint32_t ret = (uint32_t)((monotime << 16) / performance_frequency.QuadPart);
  return ret;
}

uint64_t osal_monotime(void) {
  LARGE_INTEGER counter;
  if (QueryPerformanceCounter(&counter))
    return counter.QuadPart;
  return 0;
}

uint64_t osal_cputime(size_t *optional_page_faults) {
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

__cold static bin128_t osal_bootid(void) {
  bin128_t uuid = {{0, 0}};
  bool got_machineid = false, got_boottime = false, got_bootseq = false;

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

  if (!got_machineid) {
  lack:
    uuid.x = uuid.y = 0;
    return uuid;
  }

  /*--------------------------------------------------------------------------*/

  if (!got_bootseq) {
    if (!got_boottime || !MDBX_TRUST_RTC)
      goto lack;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    if (0x1CCCCCC > now.dwHighDateTime)
      goto lack;
  }

  return uuid;
}

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

  MEMORYSTATUSEX info;
  memset(&info, 0, sizeof(info));
  info.dwLength = sizeof(info);
  if (!GlobalMemoryStatusEx(&info))
    return LOG_IFERR((int)GetLastError());

  if (total_pages) {
    const intptr_t total_ram_pages = (intptr_t)(info.ullTotalPhys >> log2page);
    *total_pages = total_ram_pages;
    if (total_ram_pages < 1)
      return LOG_IFERR(MDBX_ENOSYS);
  }

  if (avail_pages) {
    const intptr_t avail_ram_pages = (intptr_t)(info.ullAvailPhys >> log2page);
    *avail_pages = avail_ram_pages;
    if (avail_ram_pages < 1)
      return LOG_IFERR(MDBX_ENOSYS);
  }

  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

#include <wincrypt.h>

bin128_t osal_guid(const MDBX_env *env) {
  struct {
    uint64_t begin, end, cputime;
    uintptr_t thread, pid;
    const void *x, *y;
    bin128_t (*z)(const MDBX_env *env);
  } salt;

  salt.begin = osal_monotime();
  bin128_t uuid = {{0, 0}};

  if (imports.CoCreateGuid && imports.CoCreateGuid(&uuid) == 0 && check_uuid(uuid))
    return uuid;

  HCRYPTPROV hCryptProv = 0;
  if (CryptAcquireContextW(&hCryptProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
    const BOOL ok = CryptGenRandom(hCryptProv, sizeof(uuid), (unsigned char *)&uuid);
    CryptReleaseContext(hCryptProv, 0);
    if (ok && check_uuid(uuid))
      return uuid;
  }

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
}

/*--------------------------------------------------------------------------*/

void osal_ctor(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  globals.sys_pagesize = si.dwPageSize;
  globals.sys_allocation_granularity =
      (si.dwAllocationGranularity > globals.sys_pagesize) ? si.dwAllocationGranularity : globals.sys_pagesize;
  if (globals.sys_allocation_granularity > 4 * MEGABYTE && globals.sys_pagesize < MEGABYTE)
    globals.sys_allocation_granularity = 4 * MEGABYTE;
  ASSERT(globals.sys_pagesize > 0 && (globals.sys_pagesize & (globals.sys_pagesize - 1)) == 0);
  ASSERT(globals.sys_allocation_granularity >= globals.sys_pagesize &&
         globals.sys_allocation_granularity % globals.sys_pagesize == 0);
  globals.sys_pagesize_ln2 = (uint8_t)log2n_powerof2(globals.sys_pagesize);

  QueryPerformanceFrequency(&performance_frequency);
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
