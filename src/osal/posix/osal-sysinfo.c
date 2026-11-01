/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-sysinfo (POSIX part) — system RAM info (page size, total/available pages).

#include "../../internals.h"
#include "../details.h"

#if defined(__linux__) || defined(__gnu_linux__)
#include <sys/sysinfo.h>
#endif /* Linux */

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

  if (total_pages) {
#if defined(_SC_PHYS_PAGES)
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
#if defined(_SC_AVPHYS_PAGES)
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
