/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer
/// \file osal-misc.c (Windows part) — panic, asserts, string/memory helpers,
/// monotonic clock, jitter, boot-id/guid, sysram info and ctor/dtor.

#include "../../internals.h"
#include "../details.h"

#include <psapi.h>
#include <winioctl.h>

#if !MDBX_WITHOUT_MSVC_CRT && defined(_DEBUG)
#include <crtdbg.h>
#endif

/* Map a result from an NTAPI call to WIN32 error code (used by osal-mmap.c too). */
__cold MDBX_INTERNAL int osal_ntstatus2errcode(NTSTATUS status) {
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

uint64_t osal_16dot16_to_monotime(uint32_t seconds_16dot16) {
  const uint64_t ratio = performance_frequency.QuadPart;
  const uint64_t ret = (ratio * seconds_16dot16 + 32768) >> 16;
  return likely(ret || seconds_16dot16 == 0) ? ret : /* fix underflow */ 1;
}

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

/*----------------------------------------------------------------------------*/

#include <wincrypt.h>

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
