/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-sysinfo (Windows part) — system RAM info (page size, total/available pages).

#include "../../internals.h"
#include "../details.h"

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
