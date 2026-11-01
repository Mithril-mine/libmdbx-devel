/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file details.c — platform-neutral OSAL functions shared by the osal/posix/
/// and osal/windows/ implementations (declared in details.h). This file is
/// compiled once for both platforms; the platform-specific code lives in
/// osal/posix/*.c and osal/windows/*.c.

#include "details.h"
#include "../internals.h"

/* file: parking the file position outside of the data */

int osal_fseek_shut(mdbx_filehandle_t fd, uint64_t *safe_parking_lot_offset) {
  /* Set the position in files outside of the data to avoid corruption
   * due to erroneous use of file descriptors in the application code. */
  int rc = osal_fseek(fd, *safe_parking_lot_offset);
  if (unlikely(rc == MDBX_EINVAL)) {
    const uint64_t offset = *safe_parking_lot_offset;
    uint64_t best = 0;
    unsigned shift_min = 1, shift_max = 32;
    while (shift_min <= shift_max) {
      unsigned shift = (shift_min + shift_max) >> 1;
      const uint64_t probe = offset >> shift;
      int err = osal_fseek(fd, probe);
      if (err == MDBX_SUCCESS) {
        /* remember only successful offset, since the search may end on a failing probe */
        best = probe;
        shift_max = shift - 1;
        rc = err;
      } else if (err == MDBX_EINVAL)
        shift_min = shift + 1;
      else {
        rc = err;
        break;
      }
    }
    if (rc == MDBX_SUCCESS) {
      *safe_parking_lot_offset = best;
      /* the last successful probe is the one with the smallest shift, i.e. `best`,
       * but re-seek explicitly to not depend on the order of probes */
      rc = osal_fseek(fd, best);
    }
  }
  return rc;
}

/*----------------------------------------------------------------------------*/

/* mmap: mapping limit check */

MDBX_INTERNAL int check_mmap_limit(const size_t limit) {
  if ((RUNNING_ON_ASAN || mdbx_running_on_Valgrind()) && limit > globals.mmap_limit) {
    ERROR("%s (%zu bytes) is too large for %s (%zu)", "database upper size limit", limit,
          RUNNING_ON_ASAN ? "AddressSanitizer" : "Valgrind", globals.mmap_limit);
    return MDBX_TOO_LARGE;
  }
  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

/* misc: string duplication */

/*----------------------------------------------------------------------------*/

/* misc: formatted message into an allocated buffer */

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
    errno = MDBX_ENOMEM;
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

/*----------------------------------------------------------------------------*/

/* misc: asprintf wrapper */

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

/* misc: well-formedness check of an uuid-like value */

MDBX_INTERNAL bool check_uuid(bin128_t uuid) {
  size_t hw = hw64(uuid.x) + hw64(uuid.y) + hw64(uuid.x ^ uuid.y);
  return (hw >> 6) == 1;
}

/*----------------------------------------------------------------------------*/

/* misc: bootid aggregation helper */

__cold void bootid_collect(bin128_t *p, const void *s, size_t n) {
  p->y += UINT64_C(64526882297375213);
  jenkins_prng(p);
  for (size_t i = 0; i < n; ++i) {
    jenkins_prng(p);
    p->y ^= UINT64_C(48797879452804441) * ((const uint8_t *)s)[i];
    jenkins_prng(p);
    p->y += 14621231;
  }
  jenkins_prng(p);

  /* minor non-linear tomfoolery */
  const unsigned z = p->x % 61 + 1;
  p->y = p->y << z | p->y >> (64 - z);
  jenkins_prng(p);
  jenkins_prng(p);
  const unsigned q = p->x % 59 + 1;
  p->y = p->y << q | p->y >> (64 - q);
  jenkins_prng(p);
  jenkins_prng(p);
  jenkins_prng(p);
}
