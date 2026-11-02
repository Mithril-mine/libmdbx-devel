/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-init (POSIX part) — process constructor/destructor: the environment prerequisites.

#include "../../internals.h"
#include "../posix/private.h"
#include "../details.h"

uint64_t ratio_16dot16_to_monotonic;
clockid_t posix_clockid;
uint64_t monotime_limit;

extern size_t osal_iov_max; /* defined in osal-ioring.c under the same condition */

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

  if (globals.sys_allocation_granularity > 4 * MEGABYTE && globals.sys_pagesize < MEGABYTE)
    globals.sys_allocation_granularity = 4 * MEGABYTE;
  ASSERT(globals.sys_pagesize > 0 && (globals.sys_pagesize & (globals.sys_pagesize - 1)) == 0);
  ASSERT(globals.sys_allocation_granularity >= globals.sys_pagesize &&
         globals.sys_allocation_granularity % globals.sys_pagesize == 0);
  globals.sys_pagesize_ln2 = (uint8_t)log2n_powerof2(globals.sys_pagesize);

#if defined(__linux__) || defined(__gnu_linux__)
  posix_clockid = choice_monoclock();
#endif

#if defined(__APPLE__) || defined(__MACH__)
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
