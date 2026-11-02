/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-bootid (Windows part) — the boot-id determination (mashine instance identity).

#include "../../internals.h"
#include "osal/windows/private.h"
#include "../details.h"

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

__cold bin128_t osal_bootid(void) {
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
