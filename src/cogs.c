/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

/*------------------------------------------------------------------------------
 * Pack/Unpack 16-bit values for Grow step & Shrink threshold */

MDBX_NOTHROW_CONST_FUNCTION static inline pgno_t me2v(size_t m, size_t e) {
  ASSERT(m < 2048 && e < 8);
  return (pgno_t)(32768 + ((m + 1) << (e + 8)));
}

MDBX_NOTHROW_CONST_FUNCTION static inline uint16_t v2me(size_t v, size_t e) {
  ASSERT(v > (e ? me2v(2047, e - 1) : 32768));
  ASSERT(v <= me2v(2047, e));
  size_t m = (v - 32768 + ((size_t)1 << (e + 8)) - 1) >> (e + 8);
  m -= m > 0;
  ASSERT(m < 2048 && e < 8);
  // f e d c b a 9 8 7 6 5 4 3 2 1 0
  // 1 e e e m m m m m m m m m m m 1
  const uint16_t pv = (uint16_t)(0x8001 + (e << 12) + (m << 1));
  ASSERT(pv != 65535);
  return pv;
}

/* Convert 16-bit packed (exponential quantized) value to number of pages */
pgno_t pv2pages(uint16_t pv) {
  if ((pv & 0x8001) != 0x8001)
    return pv;
  if (pv == 65535)
    return 65536;
  // f e d c b a 9 8 7 6 5 4 3 2 1 0
  // 1 e e e m m m m m m m m m m m 1
  return me2v((pv >> 1) & 2047, (pv >> 12) & 7);
}

/* Convert number of pages to 16-bit packed (exponential quantized) value */
uint16_t pages2pv(size_t pages) {
  if (pages < 32769 || (pages < 65536 && (pages & 1) == 0))
    return (uint16_t)pages;
  if (pages <= me2v(2047, 0))
    return v2me(pages, 0);
  if (pages <= me2v(2047, 1))
    return v2me(pages, 1);
  if (pages <= me2v(2047, 2))
    return v2me(pages, 2);
  if (pages <= me2v(2047, 3))
    return v2me(pages, 3);
  if (pages <= me2v(2047, 4))
    return v2me(pages, 4);
  if (pages <= me2v(2047, 5))
    return v2me(pages, 5);
  if (pages <= me2v(2047, 6))
    return v2me(pages, 6);
  return (pages < me2v(2046, 7)) ? v2me(pages, 7) : 65533;
}

__cold bool pv2pages_verify(void) {
  bool ok = true, dump_translation = false;
  for (size_t i = 0; i < 65536; ++i) {
    size_t pages = pv2pages((uint16_t)i);
    size_t x = pages2pv(pages);
    size_t xp = pv2pages((uint16_t)x);
    if (pages != xp) {
      ERROR("%zu => %zu => %zu => %zu\n", i, pages, x, xp);
      ok = false;
    } else if (dump_translation && !(x == i || (x % 2 == 0 && x < 65536))) {
      DEBUG("%zu => %zu => %zu => %zu\n", i, pages, x, xp);
    }
  }
  return ok;
}

/*----------------------------------------------------------------------------*/

MDBX_NOTHROW_PURE_FUNCTION size_t bytes_ceil2sp_bytes(const MDBX_env *env, size_t bytes) {
  return ceil_powerof2(bytes, (env->ps > globals.sys_pagesize) ? env->ps : globals.sys_pagesize);
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION size_t bytes_ceil2sp_pgno(const MDBX_env *env, size_t bytes) {
  return bytes2pgno(env, bytes_ceil2sp_bytes(env, bytes));
}

MDBX_NOTHROW_PURE_FUNCTION size_t pgno_ceil2sp_bytes(const MDBX_env *env, size_t pgno) {
  return ceil_powerof2(pgno2bytes(env, pgno), globals.sys_pagesize);
}

MDBX_NOTHROW_PURE_FUNCTION pgno_t pgno_ceil2sp_pgno(const MDBX_env *env, size_t pgno) {
  return bytes2pgno(env, pgno_ceil2sp_bytes(env, pgno));
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION size_t bytes_ceil2os_bytes(const MDBX_env *env, size_t bytes) {
  const size_t sys_unit =
      MDBX_ROUNDING_TO_ALLOCATION_GRANULARITY ? globals.sys_allocation_granularity : globals.sys_pagesize;
  return ceil_powerof2(bytes, (env->ps > sys_unit) ? env->ps : sys_unit);
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION size_t bytes_ceil2os_pgno(const MDBX_env *env, size_t bytes) {
  return bytes2pgno(env, bytes_ceil2os_bytes(env, bytes));
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION size_t pgno_ceil2os_bytes(const MDBX_env *env, size_t pgno) {
  const size_t sys_unit =
      MDBX_ROUNDING_TO_ALLOCATION_GRANULARITY ? globals.sys_allocation_granularity : globals.sys_pagesize;
  return ceil_powerof2(pgno2bytes(env, pgno), sys_unit);
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION pgno_t pgno_ceil2os_pgno(const MDBX_env *env, size_t pgno) {
  return bytes2pgno(env, pgno_ceil2os_bytes(env, pgno));
}

/*----------------------------------------------------------------------------*/

__cold void update_mlcnt(const MDBX_env *env, const pgno_t new_aligned_mlocked_pgno, const bool lock_not_release) {
  for (;;) {
    const pgno_t mlock_pgno_before = atomic_load32(&env->mlocked_pgno, mo_AcquireRelease);
    eASSERT0(env, pgno_ceil2sp_pgno(env, mlock_pgno_before) == mlock_pgno_before);
    eASSERT0(env, pgno_ceil2sp_pgno(env, new_aligned_mlocked_pgno) == new_aligned_mlocked_pgno);
    if (lock_not_release ? (mlock_pgno_before >= new_aligned_mlocked_pgno)
                         : (mlock_pgno_before <= new_aligned_mlocked_pgno))
      break;
    if (likely(atomic_cas32(&((MDBX_env *)env)->mlocked_pgno, mlock_pgno_before, new_aligned_mlocked_pgno)))
      for (;;) {
        mdbx_atomic_uint32_t *const mlcnt = env->lck->mlcnt;
        const int32_t snap_locked = atomic_load32(mlcnt + 0, mo_Relaxed);
        const int32_t snap_unlocked = atomic_load32(mlcnt + 1, mo_Relaxed);
        if (mlock_pgno_before == 0 && (snap_locked - snap_unlocked) < INT_MAX) {
          eASSERT0(env, lock_not_release);
          if (unlikely(!atomic_cas32(mlcnt + 0, snap_locked, snap_locked + 1)))
            continue;
        }
        if (new_aligned_mlocked_pgno == 0 && (snap_locked - snap_unlocked) > 0) {
          eASSERT0(env, !lock_not_release);
          if (unlikely(!atomic_cas32(mlcnt + 1, snap_unlocked, snap_unlocked + 1)))
            continue;
        }
        NOTICE("%s-pages %u..%u, mlocked-process(es) %u -> %u", lock_not_release ? "lock" : "unlock",
               lock_not_release ? mlock_pgno_before : new_aligned_mlocked_pgno,
               lock_not_release ? new_aligned_mlocked_pgno : mlock_pgno_before, snap_locked - snap_unlocked,
               atomic_load32(mlcnt + 0, mo_Relaxed) - atomic_load32(mlcnt + 1, mo_Relaxed));
        return;
      }
  }
}

__cold void munlock_after(const MDBX_env *env, const pgno_t aligned_pgno, const size_t end_bytes) {
  if (atomic_load32(&env->mlocked_pgno, mo_AcquireRelease) > aligned_pgno) {
    int err = MDBX_ENOSYS;
    const size_t munlock_begin = pgno2bytes(env, aligned_pgno);
    const size_t munlock_size = end_bytes - munlock_begin;
    eASSERT0(env, end_bytes % globals.sys_pagesize == 0 && munlock_begin % globals.sys_pagesize == 0 &&
                      munlock_size % globals.sys_pagesize == 0);
#if IS_WINDOWS
    err = VirtualUnlock(ptr_disp(env->dxb_mmap.base, munlock_begin), munlock_size) ? MDBX_SUCCESS : (int)GetLastError();
    if (err == ERROR_NOT_LOCKED)
      err = MDBX_SUCCESS;
#elif defined(_POSIX_MEMLOCK_RANGE)
    err = munlock(ptr_disp(env->dxb_mmap.base, munlock_begin), munlock_size) ? errno : MDBX_SUCCESS;
#endif
    if (likely(err == MDBX_SUCCESS))
      update_mlcnt(env, aligned_pgno, false);
    else {
#if IS_WINDOWS
      WARNING("VirtualUnlock(%zu, %zu) error %d", munlock_begin, munlock_size, err);
#else
      WARNING("munlock(%zu, %zu) error %d", munlock_begin, munlock_size, err);
#endif
    }
  }
}

__cold void munlock_all(const MDBX_env *env) { munlock_after(env, 0, bytes_ceil2sp_bytes(env, env->dxb_mmap.current)); }

/*----------------------------------------------------------------------------*/

uint32_t combine_durability_flags(const uint32_t a, const uint32_t b) {
  uint32_t r = a | b;

  /* avoid false MDBX_UTTERLY_NOSYNC */
  if (F_ISSET(r, MDBX_UTTERLY_NOSYNC) && !F_ISSET(a, MDBX_UTTERLY_NOSYNC) && !F_ISSET(b, MDBX_UTTERLY_NOSYNC))
    r = (r - MDBX_UTTERLY_NOSYNC) | MDBX_SAFE_NOSYNC;

  /* convert DEPRECATED_MAPASYNC to MDBX_SAFE_NOSYNC */
  if ((r & (MDBX_WRITEMAP | DEPRECATED_MAPASYNC)) == (MDBX_WRITEMAP | DEPRECATED_MAPASYNC) &&
      !F_ISSET(r, MDBX_UTTERLY_NOSYNC))
    r = (r - DEPRECATED_MAPASYNC) | MDBX_SAFE_NOSYNC;

  /* force MDBX_NOMETASYNC if NOSYNC enabled */
  if (r & (MDBX_SAFE_NOSYNC | MDBX_UTTERLY_NOSYNC))
    r |= MDBX_NOMETASYNC;

  ASSERT(!(F_ISSET(r, MDBX_UTTERLY_NOSYNC) && !F_ISSET(a, MDBX_UTTERLY_NOSYNC) && !F_ISSET(b, MDBX_UTTERLY_NOSYNC)));
  return r;
}
