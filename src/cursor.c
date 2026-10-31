/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor.c — umbrella for the cursor module parts:
///   - cursor-setup.c   lifecycle: validation, tracking, touch/spill,
///                      shadowing, couple init, dupsort setup, cloning;
///   - cursor-nav.c     movement: siblings, first/last/next/prev wrappers,
///                      on-first/on-last predicates, long-range scrolling;
///   - cursor-mut.c     mutations: put/append/multiple and deletion;
///   - cursor-seek.c    positioning by key, operations dispatcher and
///                      estimate/distribution machinery;
///   - cursor-internal.h static __always_inline helper-templates shared
///                      by the parts above (sibling/cursor_bring/
///                      cursor_brim/cursor_step).
/// Here remains only the cursor_check() state-gate used by the public API.

#include "internals.h"
#include "cursor-internal.h"

#include "cursor-setup.c"
#include "cursor-nav.c"
#include "cursor-mut.c"
#include "cursor-seek.c"

int cursor_check(const MDBX_cursor *mc, int txn_bad_bits) {
  if (unlikely(mc == nullptr))
    return MDBX_EINVAL;

  if (unlikely(mc->signature != cur_signature_live)) {
    if (mc->signature != cur_signature_ready4dispose)
      return MDBX_EBADSIGN;
    return (txn_bad_bits > MDBX_TXN_FINISHED) ? MDBX_EINVAL : MDBX_SUCCESS;
  }

#if !defined(__SANITIZE_ADDRESS__)
  /* Проверяем что курсор в связанном списке для отслеживания.
   * Исключение допускается только для read-only операций для служебных/временных курсоров на стеке.
   * Но при включении ASAN и LTO компилятор может выносить размещение курсоров со стека очень далеко,
   * из-за чего эта проверка может ложно срабатывать.
   * Например, при вызове mdbx_estimate_distance() из mdbx_estimate_range(). */
  MDBX_MAYBE_UNUSED char stack_top[sizeof(void *)];
  ENSURE_OBJ(mc, cursor_is_tracked(mc) || (!(txn_bad_bits & txn_ro_flat) && stack_top < (char *)mc &&
                                           (char *)mc - stack_top < (ptrdiff_t)globals.sys_pagesize * 4));
#endif /* __SANITIZE_ADDRESS__ */

  if (txn_bad_bits) {
    int rc = check_txn(mc->txn, txn_bad_bits & ~MDBX_TXN_HAS_CHILD);
    if (unlikely(rc != MDBX_SUCCESS)) {
      cASSERT0(mc, rc != MDBX_RESULT_TRUE);
      return rc;
    }

    if (likely((mc->txn->flags & MDBX_TXN_HAS_CHILD) == 0))
      /* связанная с курсором транзакций не имеет дочерних, курсор не требует затенения (возможно уже затенён) */
      return likely(!cursor_dbi_changed(mc)) ? MDBX_SUCCESS : MDBX_BAD_DBI;

    /* связанная с курсором транзакция имеет дочернюю, курсор необходимо затенить, либо DBI-хендл не валиден */
    cASSERT0(mc, (mc->txn->flags & txn_ro_flat) == 0 && mc->txn != mc->txn->env->txn && mc->txn->env->txn);
    rc = dbi_check(mc->txn->env->txn, cursor_dbi(mc));
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;

    /* В результате dbi_check() курсор должен быть затенён */
    cASSERT0(mc, (mc->txn->flags & txn_ro_flat) == 0 && mc->txn == mc->txn->env->txn);
    if (unlikely(mc->txn != mc->txn->env->txn))
      return MDBX_BAD_TXN;
  }

  return MDBX_SUCCESS;
}
