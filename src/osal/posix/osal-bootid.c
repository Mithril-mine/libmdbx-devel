/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-bootid (POSIX part) — the boot-id determination (mashine instance identity).

#include "../../internals.h"
#include "../posix/private.h"
#include "../details.h"



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

#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && defined(__IPHONE_8_0)
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

__cold bin128_t osal_bootid(void) {
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

    struct timespec mono, real;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) || clock_gettime(CLOCK_REALTIME, &real) ||
        /* wrong time, RTC is mad or absent */
        1555555555l > real.tv_sec ||
        /* seems no adjustment by RTC/NTP, i.e. a fake time */
        real.tv_sec < mono.tv_sec || 1234567890l > real.tv_sec - mono.tv_sec || (real.tv_sec - mono.tv_sec) % 900u == 0)
      goto lack;
  }

  return uuid;
}
