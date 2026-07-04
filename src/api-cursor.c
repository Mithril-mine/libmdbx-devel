/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static bool cursor_check_samedbi(const MDBX_cursor *left, const MDBX_cursor *right) {
  return left->txn->front_txnid == right->txn->front_txnid && cursor_dbi(left) == cursor_dbi(right);
}

MDBX_cursor *mdbx_cursor_create(void *context) {
  cursor_couple_t *couple = osal_calloc(1, sizeof(cursor_couple_t));
  if (unlikely(!couple))
    return nullptr;

  VALGRIND_MAKE_MEM_UNDEFINED(couple, sizeof(cursor_couple_t));
  couple->outer.signature = cur_signature_ready4dispose;
  couple->outer.next = &couple->outer;
  couple->userctx = context;
  cursor_reset(couple);
  VALGRIND_MAKE_MEM_DEFINED(&couple->outer.backup, sizeof(couple->outer.backup));
  VALGRIND_MAKE_MEM_DEFINED(&couple->outer.tree, sizeof(couple->outer.tree));
  VALGRIND_MAKE_MEM_DEFINED(&couple->outer.clc, sizeof(couple->outer.clc));
  VALGRIND_MAKE_MEM_DEFINED(&couple->outer.dbi_state, sizeof(couple->outer.dbi_state));
  VALGRIND_MAKE_MEM_DEFINED(&couple->outer.subcur, sizeof(couple->outer.subcur));
  VALGRIND_MAKE_MEM_DEFINED(&couple->outer.txn, sizeof(couple->outer.txn));
  return &couple->outer;
}

int mdbx_cursor_renew(MDBX_txn *txn, MDBX_cursor *mc) {
  return likely(mc) ? mdbx_cursor_bind(txn, mc, (kvx_t *)mc->clc - txn->env->kvs) : LOG_IFERR(MDBX_EINVAL);
}

int mdbx_cursor_reset(MDBX_cursor *mc) {
  int rc = cursor_check(mc, MDBX_TXN_FINISHED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  cursor_reset((cursor_couple_t *)mc);
  return MDBX_SUCCESS;
}

int mdbx_cursor_bind(MDBX_txn *txn, MDBX_cursor *mc, MDBX_dbi dbi) {
  if (unlikely(!mc))
    return LOG_IFERR(MDBX_EINVAL);

  if (unlikely(mc->signature != cur_signature_ready4dispose && mc->signature != cur_signature_live)) {
    int rc = (mc->signature == cur_signature_wait4eot) ? MDBX_EINVAL : MDBX_EBADSIGN;
    return LOG_IFERR(rc);
  }

  int rc = check_txn(txn, MDBX_TXN_FINISHED | MDBX_TXN_HAS_CHILD);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(dbi == FREE_DBI) && ((txn->flags & txn_ro_both) == 0 || txn->parent))
    return LOG_IFERR(MDBX_EACCESS);

  rc = dbi_check(txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(mc->backup)) /* Cursor from parent transaction */
    LOG_IFERR(MDBX_EINVAL);

  if (mc->signature == cur_signature_live) {
    if (mc->txn == txn && cursor_dbi(mc) == dbi)
      return MDBX_SUCCESS;
    rc = mdbx_cursor_unbind(mc);
    if (unlikely(rc != MDBX_SUCCESS))
      return (rc == MDBX_BAD_TXN) ? MDBX_EINVAL : rc;
  }
  cASSERT0(mc, mc->next == mc);

  rc = cursor_init(mc, txn, dbi);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  mc->next = txn->cursors[dbi];
  txn->cursors[dbi] = mc;
  txn->flags |= txn_may_have_cursors;
  return MDBX_SUCCESS;
}

int mdbx_cursor_unbind(MDBX_cursor *mc) {
  if (unlikely(!mc))
    return LOG_IFERR(MDBX_EINVAL);

  if (unlikely(mc->signature != cur_signature_live))
    return (mc->signature == cur_signature_ready4dispose) ? MDBX_SUCCESS : LOG_IFERR(MDBX_EBADSIGN);

  if (unlikely(mc->backup)) /* Cursor from parent transaction */
    /* TODO: реализовать при переходе на двусвязный список курсоров */
    return LOG_IFERR(MDBX_EINVAL);

  int rc = check_txn(mc->txn, MDBX_TXN_FINISHED | MDBX_TXN_HAS_CHILD);
  if (unlikely(rc != MDBX_SUCCESS)) {
    for (const MDBX_txn *txn = mc->txn; rc == MDBX_BAD_TXN && check_txn(txn, MDBX_TXN_FINISHED) == MDBX_SUCCESS;
         txn = txn->nested)
      if (dbi_state(txn, cursor_dbi(mc)) == 0)
        /* специальный случай: курсор прикреплён к родительской транзакции, но соответствующий dbi-дескриптор ещё
         * не использовался во вложенной транзакции, т.е. курсор ещё не импортирован в дочернюю транзакцию и не имеет
         * связанного сохранённого состояния (поэтому mc→backup равен nullptr). */
        rc = MDBX_EINVAL;
    return LOG_IFERR(rc);
  }

  if (unlikely(!mc->txn || mc->txn->signature != txn_signature)) {
    ERROR("Wrong cursor's transaction %p 0x%x", __Wpedantic_format_voidptr(mc->txn), mc->txn ? mc->txn->signature : 0);
    return LOG_IFERR(MDBX_PROBLEM);
  }

  if (mc->next != mc) {
    const size_t dbi = cursor_dbi(mc);
    cASSERT0(mc, dbi < mc->txn->n_dbi);
    cASSERT0(mc, &mc->txn->env->kvs[dbi].clc == mc->clc);
    if (dbi < mc->txn->n_dbi) {
      MDBX_cursor **prev = &mc->txn->cursors[dbi];
      while (/* *prev && */ *prev != mc) {
        ENSURE_OBJ(mc, (*prev)->signature == cur_signature_live || (*prev)->signature == cur_signature_wait4eot);
        prev = &(*prev)->next;
      }
      cASSERT0(mc, *prev == mc);
      *prev = mc->next;
    }
    mc->next = mc;
  }
  cursor_drown((cursor_couple_t *)mc);
  mc->signature = cur_signature_ready4dispose;
  return MDBX_SUCCESS;
}

int mdbx_cursor_open(MDBX_txn *txn, MDBX_dbi dbi, MDBX_cursor **ret) {
  if (unlikely(!ret))
    return LOG_IFERR(MDBX_EINVAL);
  *ret = nullptr;

  MDBX_cursor *const mc = mdbx_cursor_create(nullptr);
  if (unlikely(!mc))
    return LOG_IFERR(MDBX_ENOMEM);

  int rc = mdbx_cursor_bind(txn, mc, dbi);
  if (unlikely(rc != MDBX_SUCCESS)) {
    mdbx_cursor_close(mc);
    return LOG_IFERR(rc);
  }

  *ret = mc;
  return MDBX_SUCCESS;
}

void mdbx_cursor_close(MDBX_cursor *cursor) {
  if (likely(cursor)) {
    int err = mdbx_cursor_close2(cursor);
    if (unlikely(err != MDBX_SUCCESS))
      panic_fmt(cursor, "error %d (%s) while closing cursor", err, mdbx_liberr2str(err));
  }
}

int mdbx_cursor_close2(MDBX_cursor *mc) {
  if (unlikely(!mc))
    return LOG_IFERR(MDBX_EINVAL);

  if (mc->signature == cur_signature_ready4dispose) {
    if (unlikely(mc->txn || mc->backup))
      return LOG_IFERR(MDBX_PANIC);
    cursor_drown((cursor_couple_t *)mc);
    mc->signature = 0;
    osal_free(mc);
    return MDBX_SUCCESS;
  }

  if (unlikely(mc->signature != cur_signature_live))
    return LOG_IFERR(MDBX_EBADSIGN);

  MDBX_txn *const txn = mc->txn;
  int rc = check_txn(txn, MDBX_TXN_FINISHED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (mc->backup) {
    /* Cursor closed before nested txn ends */
    cursor_reset((cursor_couple_t *)mc);
    mc->signature = cur_signature_wait4eot;
    return MDBX_SUCCESS;
  }

  if (mc->next != mc) {
    const size_t dbi = cursor_dbi(mc);
    cASSERT0(mc, dbi < mc->txn->n_dbi);
    cASSERT0(mc, &mc->txn->env->kvs[dbi].clc == mc->clc);
    if (likely(dbi < txn->n_dbi)) {
      MDBX_cursor **prev = &txn->cursors[dbi];
      while (/* *prev && */ *prev != mc) {
        ENSURE_OBJ(txn, (*prev)->signature == cur_signature_live || (*prev)->signature == cur_signature_wait4eot);
        prev = &(*prev)->next;
      }
      cASSERT0(txn, *prev == mc);
      *prev = mc->next;
    }
    mc->next = mc;
  }
  cursor_drown((cursor_couple_t *)mc);
  mc->signature = 0;
  osal_free(mc);
  return MDBX_SUCCESS;
}

int mdbx_cursor_copy(const MDBX_cursor *src, MDBX_cursor *dest) {
  if (unlikely(src == dest))
    return LOG_IFERR(MDBX_EINVAL);
  int rc = cursor_check(src, MDBX_TXN_FINISHED | MDBX_TXN_HAS_CHILD);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = mdbx_cursor_bind(src->txn, dest, cursor_dbi(src));
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  cursor_cpstk(src, dest);
  if (src->subcur) {
    dest->subcur->nested_tree = src->subcur->nested_tree;
    dest->subcur->cursor.tree = &dest->subcur->nested_tree;
    cursor_cpstk(&src->subcur->cursor, &dest->subcur->cursor);
  }
  return MDBX_SUCCESS;
}

int mdbx_txn_release_all_cursors_ex(const MDBX_txn *txn, bool unbind, size_t *count) {
  int rc = check_txn(txn, MDBX_TXN_FINISHED | MDBX_TXN_HAS_CHILD);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  size_t n = 0;
  do {
    TXN_FOREACH_DBI_FROM(txn, i, MAIN_DBI) {
      MDBX_cursor *mc = txn->cursors[i], *next = nullptr;
      if (mc) {
        txn->cursors[i] = nullptr;
        do {
          next = mc->next;
          if (mc->signature == cur_signature_live) {
            mc->signature = cur_signature_wait4eot;
            cursor_drown((cursor_couple_t *)mc);
          } else
            ENSURE(mc->signature == cur_signature_wait4eot);
          if (mc->backup) {
            MDBX_cursor *bk = mc->backup;
            mc->next = bk->next;
            mc->backup = bk->backup;
            bk->backup = nullptr;
            bk->signature = 0;
            osal_free(bk);
          } else {
            mc->signature = cur_signature_ready4dispose;
            mc->next = mc;
            ++n;
            if (!unbind) {
              mc->signature = 0;
              osal_free(mc);
            }
          }
        } while ((mc = next) != nullptr);
      }
    }
    txn = txn->parent;
  } while (txn);

  if (count)
    *count = n;
  return MDBX_SUCCESS;
}

int mdbx_cursor_compare(const MDBX_cursor *l, const MDBX_cursor *r, bool ignore_multival) {
  const int incomparable = INT16_MAX + 1;

  if (unlikely(!l))
    return r ? -incomparable * 9 : 0;
  else if (unlikely(!r))
    return incomparable * 9;

  if (unlikely(cursor_check_pure(l) != MDBX_SUCCESS))
    return (cursor_check_pure(r) == MDBX_SUCCESS) ? -incomparable * 8 : 0;
  else if (unlikely(cursor_check_pure(r) != MDBX_SUCCESS))
    return incomparable * 8;

  if (unlikely(l->clc != r->clc)) {
    if (l->txn->env != r->txn->env)
      return (l->txn->env > r->txn->env) ? incomparable * 7 : -incomparable * 7;
    if (l->txn->front_txnid != r->txn->front_txnid)
      return (l->txn->front_txnid > r->txn->front_txnid) ? incomparable * 6 : -incomparable * 6;
    return (l->clc > r->clc) ? incomparable * 5 : -incomparable * 5;
  }
  ASSERT(cursor_dbi(l) == cursor_dbi(r));

  int diff = is_pointed(l) - is_pointed(r);
  if (unlikely(diff))
    return (diff > 0) ? incomparable * 4 : -incomparable * 4;
  if (unlikely(!is_pointed(l)))
    return 0;

  intptr_t detent = (l->top <= r->top) ? l->top : r->top;
  for (intptr_t i = 0; i <= detent; ++i) {
    diff = l->ki[i] - r->ki[i];
    if (diff)
      return diff;
  }
  if (unlikely(l->top != r->top))
    return (l->top > r->top) ? incomparable * 3 : -incomparable * 3;

  ASSERT((l->subcur != nullptr) == (r->subcur != nullptr));
  if (unlikely((l->subcur != nullptr) != (r->subcur != nullptr)))
    return l->subcur ? incomparable * 2 : -incomparable * 2;
  if (ignore_multival || !l->subcur)
    return 0;

  if (is_pointed(&l->subcur->cursor)) {
    const page_t *mp = l->pg[l->top];
    const node_t *node = page_node(mp, l->ki[l->top]);
    ASSERT(node_flags(node) & N_DUP);
  }
  if (is_pointed(&r->subcur->cursor)) {
    const page_t *mp = r->pg[r->top];
    const node_t *node = page_node(mp, r->ki[r->top]);
    ASSERT(node_flags(node) & N_DUP);
  }

  l = &l->subcur->cursor;
  r = &r->subcur->cursor;
  diff = is_pointed(l) - is_pointed(r);
  if (unlikely(diff))
    return (diff > 0) ? incomparable * 2 : -incomparable * 2;
  if (unlikely(!is_pointed(l)))
    return 0;

  detent = (l->top <= r->top) ? l->top : r->top;
  for (intptr_t i = 0; i <= detent; ++i) {
    diff = l->ki[i] - r->ki[i];
    if (diff)
      return diff;
  }
  if (unlikely(l->top != r->top))
    return (l->top > r->top) ? incomparable : -incomparable;

  return (l->flags & z_eof_hard) - (r->flags & z_eof_hard);
}

int mdbx_cursor_count_ex(const MDBX_cursor *mc, size_t *count, MDBX_stat *ns, size_t bytes) {
  int rc = cursor_check_ro(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (ns) {
    const size_t size_before_modtxnid = offsetof(MDBX_stat, ms_mod_txnid);
    if (unlikely(bytes != sizeof(MDBX_stat)) && bytes != size_before_modtxnid)
      return LOG_IFERR(MDBX_EINVAL);
    memset(ns, 0, sizeof(*ns));
  }

  size_t nvals = 0;
  if (is_filled(mc)) {
    nvals = 1;
    if (!inner_hollow(mc)) {
      const page_t *mp = mc->pg[mc->top];
      const node_t *node = page_node(mp, mc->ki[mc->top]);
      cASSERT0(mc, node_flags(node) & N_DUP);
      const tree_t *nt = &mc->subcur->nested_tree;
      nvals = unlikely(nt->items > PTRDIFF_MAX) ? PTRDIFF_MAX : (size_t)nt->items;
      if (ns) {
        ns->ms_psize = (unsigned)node_ds(node);
        if (node_flags(node) & N_TREE) {
          ns->ms_psize = mc->txn->env->ps;
          ns->ms_depth = nt->height;
          ns->ms_branch_pages = nt->branch_pages;
        }
        cASSERT0(mc, nt->large_pages == 0);
        ns->ms_leaf_pages = nt->leaf_pages;
        ns->ms_entries = nt->items;
        if (likely(bytes >= offsetof(MDBX_stat, ms_mod_txnid) + sizeof(ns->ms_mod_txnid)))
          ns->ms_mod_txnid = nt->mod_txnid;
      }
    }
  }

  if (likely(count))
    *count = nvals;

  return MDBX_SUCCESS;
}

int mdbx_cursor_count(const MDBX_cursor *mc, size_t *count) {
  if (unlikely(count == nullptr))
    return LOG_IFERR(MDBX_EINVAL);

  return mdbx_cursor_count_ex(mc, count, nullptr, 0);
}

int mdbx_cursor_on_first(const MDBX_cursor *mc) {
  int rc = cursor_check_pure(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return cursor_on_first(mc);
}

int mdbx_cursor_on_first_dup(const MDBX_cursor *mc) {
  int rc = cursor_check_pure(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return (is_filled(mc) && mc->subcur) ? cursor_on_first(&mc->subcur->cursor) : MDBX_RESULT_TRUE;
}

int mdbx_cursor_on_last(const MDBX_cursor *mc) {
  int rc = cursor_check_pure(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return cursor_on_last(mc);
}

int mdbx_cursor_on_last_dup(const MDBX_cursor *mc) {
  int rc = cursor_check_pure(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return (is_filled(mc) && mc->subcur) ? cursor_on_last(&mc->subcur->cursor) : MDBX_RESULT_TRUE;
}

int mdbx_cursor_eof(const MDBX_cursor *mc) {
  int rc = cursor_check_pure(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return is_eof(mc) ? MDBX_RESULT_TRUE : MDBX_RESULT_FALSE;
}

int mdbx_cursor_get(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data, MDBX_cursor_op op) {
  int rc = cursor_check_ro(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return LOG_IFERR(cursor_ops(mc, key, data, op));
}

__hot static int scan_continue(MDBX_cursor *mc, MDBX_predicate_func predicate, void *context, void *arg, MDBX_val *key,
                               MDBX_val *value, MDBX_cursor_op turn_op) {
  int rc;
  switch (turn_op) {
  case MDBX_NEXT:
  case MDBX_NEXT_NODUP:
    for (;;) {
      rc = predicate(context, key, value, arg);
      if (rc != MDBX_RESULT_FALSE)
        return rc;
      rc = outer_next(mc, key, value, turn_op);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_NOTFOUND) ? MDBX_RESULT_FALSE : rc;
    }

  case MDBX_PREV:
  case MDBX_PREV_NODUP:
    for (;;) {
      rc = predicate(context, key, value, arg);
      if (rc != MDBX_RESULT_FALSE)
        return rc;
      rc = outer_prev(mc, key, value, turn_op);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_NOTFOUND) ? MDBX_RESULT_FALSE : rc;
    }

  case MDBX_NEXT_DUP:
    if (mc->subcur)
      for (;;) {
        rc = predicate(context, key, value, arg);
        if (rc != MDBX_RESULT_FALSE)
          return rc;
        rc = inner_next(&mc->subcur->cursor, value);
        if (unlikely(rc != MDBX_SUCCESS))
          return (rc == MDBX_NOTFOUND) ? MDBX_RESULT_FALSE : rc;
      }
    return MDBX_NOTFOUND;

  case MDBX_PREV_DUP:
    if (mc->subcur)
      for (;;) {
        rc = predicate(context, key, value, arg);
        if (rc != MDBX_RESULT_FALSE)
          return rc;
        rc = inner_prev(&mc->subcur->cursor, value);
        if (unlikely(rc != MDBX_SUCCESS))
          return (rc == MDBX_NOTFOUND) ? MDBX_RESULT_FALSE : rc;
      }
    return MDBX_NOTFOUND;

  default:
    for (;;) {
      rc = predicate(context, key, value, arg);
      if (rc != MDBX_RESULT_FALSE)
        return rc;
      rc = cursor_ops(mc, key, value, turn_op);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_NOTFOUND) ? MDBX_RESULT_FALSE : rc;
    }
  }
}

int mdbx_cursor_scan(MDBX_cursor *mc, MDBX_predicate_func predicate, void *context, MDBX_cursor_op start_op,
                     MDBX_cursor_op turn_op, void *arg) {
  if (unlikely(!predicate))
    return LOG_IFERR(MDBX_EINVAL);

  const unsigned valid_start_mask = 1 << MDBX_FIRST | 1 << MDBX_FIRST_DUP | 1 << MDBX_LAST | 1 << MDBX_LAST_DUP |
                                    1 << MDBX_GET_CURRENT | 1 << MDBX_GET_MULTIPLE;
  if (unlikely(start_op > 30 || ((1 << start_op) & valid_start_mask) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  const unsigned valid_turn_mask = 1 << MDBX_NEXT | 1 << MDBX_NEXT_DUP | 1 << MDBX_NEXT_NODUP | 1 << MDBX_PREV |
                                   1 << MDBX_PREV_DUP | 1 << MDBX_PREV_NODUP | 1 << MDBX_NEXT_MULTIPLE |
                                   1 << MDBX_PREV_MULTIPLE;
  if (unlikely(turn_op > 30 || ((1 << turn_op) & valid_turn_mask) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  MDBX_val key = {nullptr, 0}, value = {nullptr, 0};
  int rc = mdbx_cursor_get(mc, &key, &value, start_op);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);
  return LOG_IFERR(scan_continue(mc, predicate, context, arg, &key, &value, turn_op));
}

int mdbx_cursor_scan_from(MDBX_cursor *mc, MDBX_predicate_func predicate, void *context, MDBX_cursor_op from_op,
                          MDBX_val *key, MDBX_val *value, MDBX_cursor_op turn_op, void *arg) {
  if (unlikely(!predicate || !key))
    return LOG_IFERR(MDBX_EINVAL);

  const unsigned valid_start_mask = 1 << MDBX_GET_BOTH | 1 << MDBX_GET_BOTH_RANGE | 1 << MDBX_SET_KEY |
                                    1 << MDBX_GET_MULTIPLE | 1 << MDBX_SET_LOWERBOUND | 1 << MDBX_SET_UPPERBOUND;
  if (unlikely(from_op < MDBX_TO_KEY_LESSER_THAN && ((1 << from_op) & valid_start_mask) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  const unsigned valid_turn_mask = 1 << MDBX_NEXT | 1 << MDBX_NEXT_DUP | 1 << MDBX_NEXT_NODUP | 1 << MDBX_PREV |
                                   1 << MDBX_PREV_DUP | 1 << MDBX_PREV_NODUP | 1 << MDBX_NEXT_MULTIPLE |
                                   1 << MDBX_PREV_MULTIPLE;
  if (unlikely(turn_op > 30 || ((1 << turn_op) & valid_turn_mask) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  int rc = mdbx_cursor_get(mc, key, value, from_op);
  if (unlikely(MDBX_IS_ERROR(rc)))
    return LOG_IFERR(rc);

  cASSERT0(mc, key != nullptr);
  MDBX_val stub;
  if (!value) {
    value = &stub;
    rc = cursor_ops(mc, key, value, MDBX_GET_CURRENT);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }
  return LOG_IFERR(scan_continue(mc, predicate, context, arg, key, value, turn_op));
}

int mdbx_cursor_get_batch(MDBX_cursor *mc, size_t *count, MDBX_val *pairs, size_t limit, MDBX_cursor_op op) {
  if (unlikely(!count))
    return LOG_IFERR(MDBX_EINVAL);

  *count = 0;
  if (unlikely(limit < 4 || limit > INTPTR_MAX - 2))
    return LOG_IFERR(MDBX_EINVAL);

  int rc = cursor_check_ro(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(mc->subcur))
    return LOG_IFERR(MDBX_INCOMPATIBLE) /* must be a non-dupsort table */;

  switch (op) {
  case MDBX_NEXT:
    if (unlikely(is_eof(mc)))
      return LOG_IFERR(is_pointed(mc) ? MDBX_NOTFOUND : MDBX_ENODATA);
    break;

  case MDBX_FIRST:
    if (!is_filled(mc)) {
      rc = outer_first(mc, nullptr, nullptr);
      if (unlikely(rc != MDBX_SUCCESS))
        return LOG_IFERR(rc);
    }
    break;

  default:
    DEBUG("unhandled/unimplemented cursor operation %u", op);
    return LOG_IFERR(MDBX_EINVAL);
  }

  const page_t *mp = mc->pg[mc->top];
  size_t nkeys = page_numkeys(mp);
  size_t ki = mc->ki[mc->top];
  size_t n = 0;
  while (n + 2 <= limit) {
    cASSERT0(mc, ki < nkeys);
    if (unlikely(ki >= nkeys))
      goto sibling;

    const node_t *leaf = page_node(mp, ki);
    pairs[n] = get_key(leaf);
    rc = node_read(mc, leaf, &pairs[n + 1], mp);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;

    n += 2;
    if (++ki == nkeys) {
    sibling:
      rc = cursor_sibling_right(mc);
      if (rc != MDBX_SUCCESS) {
        if (rc == MDBX_NOTFOUND)
          rc = MDBX_RESULT_TRUE;
        goto bailout;
      }

      mp = mc->pg[mc->top];
      DEBUG("next page is %" PRIaPGNO ", key index %u", mp->pgno, mc->ki[mc->top]);
      if (!MDBX_DISABLE_VALIDATION && unlikely(!check_leaf_type(mc, mp))) {
        ERROR("unexpected leaf-page #%" PRIaPGNO " type 0x%x seen by cursor", mp->pgno, mp->flags);
        rc = MDBX_CORRUPTED;
        goto bailout;
      }
      nkeys = page_numkeys(mp);
      ki = 0;
    }
  }
  mc->ki[mc->top] = (indx_t)ki;

bailout:
  *count = n;
  return LOG_IFERR(rc);
}

/*----------------------------------------------------------------------------*/

int mdbx_cursor_set_userctx(MDBX_cursor *mc, void *ctx) {
  int rc = cursor_check(mc, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  cursor_couple_t *couple = container_of(mc, cursor_couple_t, outer);
  couple->userctx = ctx;
  return MDBX_SUCCESS;
}

void *mdbx_cursor_get_userctx(const MDBX_cursor *mc) {
  if (unlikely(!mc))
    return nullptr;

  if (unlikely(mc->signature != cur_signature_ready4dispose && mc->signature != cur_signature_live))
    return nullptr;

  cursor_couple_t *couple = container_of(mc, cursor_couple_t, outer);
  return couple->userctx;
}

MDBX_txn *mdbx_cursor_txn(const MDBX_cursor *mc) {
  if (unlikely(!mc || mc->signature != cur_signature_live))
    return nullptr;
  MDBX_txn *txn = mc->txn;
  if (unlikely(!txn || txn->signature != txn_signature || (txn->flags & MDBX_TXN_FINISHED)))
    return nullptr;
  return (txn->flags & MDBX_TXN_HAS_CHILD) ? txn->env->txn : txn;
}

MDBX_dbi mdbx_cursor_dbi(const MDBX_cursor *mc) {
  if (unlikely(!mc || mc->signature != cur_signature_live))
    return UINT_MAX;
  return cursor_dbi(mc);
}

/*----------------------------------------------------------------------------*/

int mdbx_cursor_put(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, MDBX_put_flags_t flags) {
  if (unlikely(key == nullptr || data == nullptr))
    return LOG_IFERR(MDBX_EINVAL);

  int rc = cursor_check_rw(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(flags & MDBX_MULTIPLE)) {
    rc = cursor_check_multiple(mc, key, data, flags);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }

  if (flags & MDBX_RESERVE) {
    if (unlikely(mc->tree->flags & (MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_INTEGERDUP | MDBX_DUPFIXED)))
      return LOG_IFERR(MDBX_INCOMPATIBLE);
    data->iov_base = nullptr;
  }

  return LOG_IFERR(cursor_put_checklen(mc, key, data, flags));
}

int mdbx_cursor_del(MDBX_cursor *mc, MDBX_put_flags_t flags) {
  int rc = cursor_check_rw(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  return LOG_IFERR(cursor_del(mc, flags));
}

__cold int mdbx_cursor_ignord(MDBX_cursor *mc) {
  int rc = cursor_check(mc, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  mc->checking |= z_ignord;
  if (mc->subcur)
    mc->subcur->cursor.checking |= z_ignord;

  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

#if MDBX_ENABLE_BUNCHES_REMOVAL
static MDBX_cursor *get_axe(MDBX_cursor *mc, cursor_couple_t *couple) {
  MDBX_cursor *axe = cursor_clone_complete(mc, couple);
  axe->next = axe->txn->cursors[cursor_dbi(axe)];
  axe->txn->cursors[cursor_dbi(axe)] = axe;
  return axe;
}
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */

int mdbx_cursor_bunch_delete(MDBX_cursor *mc, MDBX_bunch_action_t action, uint64_t *number_of_affected) {
  int rc = cursor_check_rw(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(action < MDBX_DELETE_CURRENT_VALUE || action > MDBX_DELETE_WHOLE))
    return LOG_IFERR(MDBX_EINVAL);

#if MDBX_ENABLE_BUNCHES_REMOVAL
  cursor_couple_t couple;
  MDBX_cursor *axe = nullptr;
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */

  const uint64_t save_items = mc->tree->items;
  switch (action) {
  default:
    rc = MDBX_EINVAL;
    break;
  case MDBX_DELETE_CURRENT_VALUE:
    rc = cursor_del(mc, 0);
    break;
  case MDBX_DELETE_CURRENT_MULTIVAL_ALL:
    rc = cursor_del(mc, MDBX_ALLDUPS);
    break;
  case MDBX_DELETE_WHOLE:
    rc = tbl_purge(mc);
    break;

#if MDBX_ENABLE_BUNCHES_REMOVAL
  case MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_EXCLUDING:
  case MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_INCLUDING:
    axe = get_axe(mc, &couple);
    if (inner_pointed(axe)) {
      rc = inner_first(&axe->subcur->cursor, nullptr);
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
    }
    rc = tree_cutoff_range(axe, mc, action == MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_INCLUDING);
#else
  case MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_INCLUDING:
    rc = cursor_del(mc, 0);
    __fallthrough /* fall through */;
  case MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_EXCLUDING:
    while (rc == MDBX_SUCCESS) {
      rc = outer_prev(mc, nullptr, nullptr, MDBX_PREV_DUP);
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
      rc = cursor_del(mc, 0);
    }
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
    break;

#if MDBX_ENABLE_BUNCHES_REMOVAL
  case MDBX_DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING:
    rc = outer_next(mc, nullptr, nullptr, MDBX_NEXT_DUP);
    if (rc != MDBX_SUCCESS) {
      rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
      break;
    }
    __fallthrough /* fall through */;
  case MDBX_DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING:
    axe = get_axe(mc, &couple);
    if (inner_pointed(mc)) {
      rc = inner_last(&mc->subcur->cursor, nullptr);
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
    }
    rc = tree_cutoff_range(axe, mc, true);
#else
  case MDBX_DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING:
    rc = cursor_del(mc, 0);
    __fallthrough /* fall through */;
  case MDBX_DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING:
    while (rc == MDBX_SUCCESS) {
      rc = outer_next(mc, nullptr, nullptr, MDBX_NEXT_DUP);
      mc->flags &= ~z_eof_hard;
      ((cursor_couple_t *)mc)->inner.cursor.flags &= ~z_eof_hard;
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
      rc = cursor_del(mc, 0);
    }
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
    break;

#if MDBX_ENABLE_BUNCHES_REMOVAL
  case MDBX_DELETE_BEFORE_INCLUDING:
  case MDBX_DELETE_BEFORE_EXCLUDING:
    axe = get_axe(mc, &couple);
    rc = outer_first(axe, nullptr, nullptr);
    if (rc != MDBX_SUCCESS) {
      rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
      break;
    }
    rc = tree_cutoff_range(axe, mc, action == MDBX_DELETE_BEFORE_INCLUDING);
#else
  case MDBX_DELETE_BEFORE_INCLUDING:
    rc = cursor_del(mc, 0);
    __fallthrough /* fall through */;
  case MDBX_DELETE_BEFORE_EXCLUDING:
    while (rc == MDBX_SUCCESS) {
      rc = outer_prev(mc, nullptr, nullptr, MDBX_PREV);
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
      rc = cursor_del(mc, 0);
    }
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
    break;

#if MDBX_ENABLE_BUNCHES_REMOVAL
  case MDBX_DELETE_AFTER_EXCLUDING:
    rc = outer_next(mc, nullptr, nullptr, MDBX_NEXT);
    if (rc != MDBX_SUCCESS) {
      rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
      break;
    }
    __fallthrough /* fall through */;
  case MDBX_DELETE_AFTER_INCLUDING:
    axe = get_axe(mc, &couple);
    rc = outer_last(mc, nullptr, nullptr);
    if (rc != MDBX_SUCCESS) {
      rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
      break;
    }
    rc = tree_cutoff_range(axe, mc, true);
#else
  case MDBX_DELETE_AFTER_INCLUDING:
    rc = cursor_del(mc, 0);
    __fallthrough /* fall through */;
  case MDBX_DELETE_AFTER_EXCLUDING:
    while (rc == MDBX_SUCCESS) {
      rc = outer_next(mc, nullptr, nullptr, MDBX_NEXT);
      mc->flags &= ~z_eof_hard;
      ((cursor_couple_t *)mc)->inner.cursor.flags &= ~z_eof_hard;
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
      rc = cursor_del(mc, 0);
    }
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
    break;
  }

#if MDBX_ENABLE_BUNCHES_REMOVAL
  if (axe)
    axe->txn->cursors[cursor_dbi(axe)] = axe->next;
#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
  if (number_of_affected)
    *number_of_affected = save_items - mc->tree->items;

  return LOG_IFERR(rc);
}

int mdbx_cursor_delete_range(MDBX_cursor *begin, MDBX_cursor *end, bool end_including, uint64_t *number_of_affected) {
  if (number_of_affected)
    *number_of_affected = 0;
  if (unlikely(!begin && !end))
    return LOG_IFERR(MDBX_EINVAL);

  int rc;
  if (begin) {
    rc = cursor_check_rw(begin);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    if (unlikely(!is_pointed(begin)))
      return LOG_IFERR(MDBX_ENODATA);
  }

  if (end) {
    rc = cursor_check_rw(end);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    if (unlikely(!is_pointed(end)))
      return LOG_IFERR(MDBX_ENODATA);
  }

  if (begin && end && unlikely(!cursor_check_samedbi(begin, end)))
    return LOG_IFERR(MDBX_EINVAL);

  cursor_couple_t couple;
  if (!begin) {
    begin = cursor_clone_complete(end, &couple);
    rc = outer_first(begin, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    couple.outer.next = begin->txn->cursors[cursor_dbi(begin)];
    begin->txn->cursors[cursor_dbi(begin)] = &couple.outer;
  }

  if (!end) {
    end = cursor_clone_complete(begin, &couple);
    rc = outer_last(end, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    couple.outer.next = end->txn->cursors[cursor_dbi(end)];
    end->txn->cursors[cursor_dbi(end)] = &couple.outer;
  }

  const uint64_t save_items = begin->tree->items;
  rc = tree_cutoff_range(begin, end, !!end_including);
  if (number_of_affected)
    *number_of_affected = save_items - begin->tree->items;

  if (begin->txn->cursors[cursor_dbi(begin)] == &couple.outer)
    /* coverity[UNINIT] */
    couple.outer.txn->cursors[cursor_dbi(begin)] = couple.outer.next;

  return LOG_IFERR(rc);
}

int mdbx_cursor_distribute(const MDBX_cursor *begin, const MDBX_cursor *end, MDBX_cursor **array, intptr_t array_size,
                           unsigned deepness) {
  if (unlikely(!begin && !end))
    return LOG_IFERR(MDBX_EINVAL);
  if (unlikely(!array || array_size < 1))
    return LOG_IFERR(MDBX_EINVAL);

  int rc;
  if (begin) {
    rc = cursor_check_ro(begin);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    if (unlikely(!is_pointed(begin)))
      return LOG_IFERR(MDBX_ENODATA);
  }

  if (end) {
    rc = cursor_check_ro(end);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    if (unlikely(!is_pointed(end)))
      return LOG_IFERR(MDBX_ENODATA);
  }

  if (begin && end && unlikely(!cursor_check_samedbi(begin, end)))
    return LOG_IFERR(MDBX_EINVAL);

  const MDBX_cursor *ref = begin ? begin : end;
  MDBX_cursor *iter = nullptr;
  for (intptr_t i = 0; i < array_size; ++i) {
    if (array[i] != begin && array[i] != end) {
      rc = cursor_check_pure(iter = array[i]);
      if (unlikely(rc != MDBX_SUCCESS))
        return LOG_IFERR(rc);
      if (unlikely(!cursor_check_samedbi(iter, ref)))
        return LOG_IFERR(MDBX_EINVAL);
    }
  }
  if (unlikely(!iter))
    return LOG_IFERR(MDBX_EINVAL);

  cursor_couple_t couple;
  if (!begin) {
    begin = cursor_clone_complete(end, &couple);
    rc = outer_first((MDBX_cursor *)begin, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }

  if (!end) {
    end = cursor_clone_complete(begin, &couple);
    rc = outer_last((MDBX_cursor *)end, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }

  rc = cursor_distribute(begin, end, array, array_size, (deepness < CURSOR_STACK_SIZE) ? deepness : CURSOR_STACK_SIZE);
  return LOG_IFERR(rc);
}

int mdbx_cursor_scroll(MDBX_cursor *mc, intptr_t amount, unsigned deepness) {
  int rc = cursor_check_ro(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);
  if (unlikely(!is_pointed(mc)))
    return LOG_IFERR(MDBX_ENODATA);

  if (amount > 0)
    rc = cursor_scroll_forward(mc, amount, deepness);
  else if (amount < 0)
    rc = cursor_scroll_backward(mc, -amount, deepness);

  return LOG_IFERR(rc);
}

int mdbx_cursor_distance(const MDBX_cursor *begin, const MDBX_cursor *end, intptr_t *distance, unsigned deepness) {
  if (unlikely(!begin && !end))
    return LOG_IFERR(MDBX_EINVAL);
  if (unlikely(!distance))
    return LOG_IFERR(MDBX_EINVAL);

  int rc;
  if (begin) {
    rc = cursor_check_ro(begin);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    if (unlikely(!is_pointed(begin)))
      return LOG_IFERR(MDBX_ENODATA);
  }

  if (end) {
    rc = cursor_check_ro(end);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    if (unlikely(!is_pointed(end)))
      return LOG_IFERR(MDBX_ENODATA);
  }

  if (begin && end && unlikely(!cursor_check_samedbi(begin, end)))
    return LOG_IFERR(MDBX_EINVAL);

  cursor_couple_t couple_opposite;
  if (!begin) {
    begin = cursor_clone_complete(end, &couple_opposite);
    rc = outer_first((MDBX_cursor *)begin, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }

  if (!end) {
    end = cursor_clone_complete(begin, &couple_opposite);
    rc = outer_last((MDBX_cursor *)end, nullptr, nullptr);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }

  intptr_t cmp = cursor_cmp(begin, end);
  if (cmp == 0) {
    cmp = (end->flags & z_eof_hard) - (begin->flags & z_eof_hard);
    if (unlikely(cmp != 0)) {
      *distance = (cmp > 0) ? 1 : -1;
      return MDBX_SUCCESS;
    }
    if (inner_pointed(end)) {
      ASSERT(inner_pointed(begin));
      cmp = cursor_cmp(&begin->subcur->cursor, &end->subcur->cursor);
      if (cmp == 0) {
        *distance = 0;
        return MDBX_SUCCESS;
      }
    }
  }

  const bool negative = cmp > 0;
  if (negative) {
    const MDBX_cursor *swap = begin;
    begin = end;
    end = swap;
  }

  cursor_couple_t couple_iter;
  MDBX_cursor *iter = (MDBX_cursor *)begin;
  if (iter != &couple_opposite.outer)
    iter = cursor_clone_complete(iter, &couple_iter);

  cdr_t tdr = cursor_distance(iter, end, deepness);
  rc = tdr.err;
  if (likely(rc == MDBX_SUCCESS))
    *distance = (!negative) ? (intptr_t)tdr.distance : -(intptr_t)tdr.distance;

  return LOG_IFERR(rc);
}
