/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-init (Windows part) — process constructor/destructor: the environment prerequisites.

#include "../../internals.h"
#include "osal/windows/private.h"
#include "../details.h"

LARGE_INTEGER performance_frequency;
uint64_t monotime_limit;

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
