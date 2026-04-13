/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanations license change,
/// credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

/* Search for the lowest key under the current branch page. This just bypasses a numkeys check in the current page
 * before calling tree_search_continue(), because the callers are all in situations where the current page is known
 * to be underfilled. */
__hot __noinline int tree_search_lowest(MDBX_cursor *mc) {
  cASSERT0(mc, mc->top >= 0);
  page_t *mp = mc->pg[mc->top];
  cASSERT0(mc, is_branch(mp));

  node_t *node = page_node(mp, 0);
  int err = page_get(mc, node_pgno(node), &mp, mp->txnid);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  mc->ki[mc->top] = 0;
  err = cursor_push(mc, mp, 0);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  return tree_search_continue(mc, nullptr, Z_FIRST);
}

__hot int tree_search(MDBX_cursor *mc, const MDBX_val *key, int flags) {
  int err;
  if (unlikely(mc->txn->flags & MDBX_TXN_BLOCKED)) {
    DEBUG("%s", "transaction has failed, must abort");
    err = MDBX_BAD_TXN;
  bailout:
    be_poor(mc);
    return err;
  }

  const size_t dbi = cursor_dbi(mc);
  if (unlikely(*cursor_dbi_state(mc) & DBI_STALE)) {
    err = tbl_refresh_absent2baddbi(mc->txn, dbi);
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;
  }

  const pgno_t root = mc->tree->root;
  if (unlikely(root == P_INVALID)) {
    DEBUG("%s", "tree is empty");
    cASSERT0(mc, is_poor(mc));
    return MDBX_NOTFOUND;
  }

  cASSERT0(mc, root >= NUM_METAS && root < mc->txn->geo.first_unallocated);
  if (mc->top < 0 || mc->pg[0]->pgno != root) {
    err = page_get(mc, root, &mc->pg[0], tbl_root_txnid(mc->txn, cursor_dbi(mc)));
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;
  }

  mc->top = 0;
  mc->ki[0] = (flags & Z_LAST) ? page_numkeys(mc->pg[0]) - 1 : 0;
  DEBUG("db %d root page %" PRIaPGNO " has flags 0x%X", cursor_dbi_dbg(mc), root, mc->pg[0]->flags);

  if (flags & Z_MODIFY) {
    err = page_touch(mc);
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;
  }

  if (flags & Z_ROOTONLY)
    return MDBX_SUCCESS;

  return tree_search_continue(mc, key, flags);
}

__hot __noinline int tree_search_continue(MDBX_cursor *mc, const MDBX_val *key, int flags) {
  cASSERT0(mc, !is_poor(mc));
  DKBUF_DEBUG;
  int err;
  page_t *mp = mc->pg[mc->top];
  intptr_t ki = (flags & Z_FIRST) ? 0 : page_numkeys(mp) - 1;
  while (is_branch(mp)) {
    DEBUG("branch page %" PRIaPGNO " has %zu keys", mp->pgno, page_numkeys(mp));
    cASSERT0(mc, page_numkeys(mp) > 1);
    DEBUG("found index 0 to page %" PRIaPGNO, node_pgno(page_node(mp, 0)));

    if ((flags & (Z_FIRST | Z_LAST)) == 0) {
      ki = tree_search_branch(mc, key);
      DEBUG("following index %zu for key [%s]", ki, DKEY_DEBUG(key));
    }

    err = page_get(mc, node_pgno(page_node(mp, ki)), &mp, mp->txnid);
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;

    mc->ki[mc->top] = (indx_t)ki;
    ki = (flags & Z_FIRST) ? 0 : page_numkeys(mp) - 1;
    err = cursor_push(mc, mp, ki);
    if (unlikely(err != MDBX_SUCCESS))
      goto bailout;

    if (flags & Z_MODIFY) {
      err = page_touch(mc);
      if (unlikely(err != MDBX_SUCCESS))
        goto bailout;
      mp = mc->pg[mc->top];
    }
  }

  if (!MDBX_DISABLE_VALIDATION && unlikely(!check_leaf_type(mc, mp))) {
    ERROR("unexpected leaf-page #%" PRIaPGNO " type 0x%x seen by cursor", mp->pgno, mp->flags);
    err = MDBX_CORRUPTED;
  bailout:
    be_poor(mc);
    return err;
  }

  DEBUG("found leaf page %" PRIaPGNO " for key [%s]", mp->pgno, DKEY_DEBUG(key));
  /* Логически верно, но (в текущем понимании) нет необходимости.
     Однако, стоит ещё по-проверять/по-тестировать.
     Возможно есть сценарий, в котором очистка флагов всё-таки требуется.

     be_filled(mc); */
  return MDBX_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------------- */

#if defined(__GNUC__) && !(defined(__e2k__) || defined(__elbrus__))
#define CLEAR_VALUE_PROPAGATION(VAR) __asm__ __volatile__("" : "+r"(cmp))
#else
#define CLEAR_VALUE_PROPAGATION(VAR)                                                                                   \
  do {                                                                                                                 \
  } while (0)
#endif

#define BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN(IT, CMP, LOWER, SIZE)                                                     \
  intptr_t adjust = SIZE - 1, half = adjust >> 1;                                                                      \
  adjust &= 1;                                                                                                         \
  IT = LOWER + half

#define BINARY_BRANCHLESS_SEARCH_CYCLE_END(IT, CMP, LOWER, SIZE)                                                       \
  CMP >>= CHAR_BIT * sizeof(CMP) - 1;                                                                                  \
  CLEAR_VALUE_PROPAGATION(CMP);                                                                                        \
  LOWER += (half + 1) & CMP;                                                                                           \
  SIZE = half + (CMP & adjust)

static int null_comparator(const MDBX_val *a, const MDBX_val *b) {
  (void)a;
  (void)b;
  panic("must not be called");
  return 0;
}

#define SEARCH_FOLIAGE(NAME, BRANCH_COMPARATOR, LEAF_COMPARATOR, BOOL_DIFFERENT_COMPARATORS, BOOL_DUPFIXED)            \
  MDBX_NOTHROW_PURE_FUNCTION __hot static sfr_t search_foliage_##NAME(MDBX_cursor *mc, const MDBX_val *key) {          \
    page_t *mp = mc->pg[mc->top];                                                                                      \
    const intptr_t nkeys = page_numkeys(mp);                                                                           \
    DEBUG("searching %zu keys in %s %spage %" PRIaPGNO, nkeys, is_leaf(mp) ? "leaf" : "branch",                        \
          is_subpage(mp) ? "sub-" : "", mp->pgno);                                                                     \
                                                                                                                       \
    sfr_t ret;                                                                                                         \
    ret.exact = false;                                                                                                 \
    STATIC_ASSERT(P_BRANCH == 1);                                                                                      \
    intptr_t lo = mp->flags & P_BRANCH;                                                                                \
    intptr_t hi = nkeys - 1;                                                                                           \
    if (unlikely(hi < lo)) {                                                                                           \
      mc->ki[mc->top] = 0;                                                                                             \
      ret.node = nullptr;                                                                                              \
      return ret;                                                                                                      \
    }                                                                                                                  \
                                                                                                                       \
    intptr_t scope = nkeys - lo, cmp, it;                                                                              \
    node_t *node = (node_t *)(intptr_t)-1;                                                                             \
    MDBX_val node_key;                                                                                                 \
    if ((BOOL_DIFFERENT_COMPARATORS | BOOL_DUPFIXED) && lo == 0) {                                                     \
      ASSERT(is_leaf(mp));                                                                                             \
      if (BOOL_DUPFIXED) {                                                                                             \
        cASSERT0(mc, is_dupfix_leaf(mp));                                                                              \
        cASSERT0(mc, mp->dupfix_ksize == mc->tree->dupfix_size);                                                       \
        node_key.iov_len = mp->dupfix_ksize;                                                                           \
        TRACE(">> %s lo %zu, size %zu, nkeys %zu", "leaf-dupfix", lo, scope, nkeys);                                   \
        do {                                                                                                           \
          MDBX_CURSOR_STC_INC(mc);                                                                                     \
          BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN(it, cmp, lo, scope);                                                    \
          node_key.iov_base = page_dupfix_ptr(mp, it, node_key.iov_len);                                               \
          cASSERT0(mc, ptr_disp(mp, mc->txn->env->ps) >= ptr_disp(node_key.iov_base, node_key.iov_len));               \
          cmp = LEAF_COMPARATOR(&node_key, key);                                                                       \
          TRACE("== i %zu, cmp %zi", it, cmp);                                                                         \
          if (unlikely(cmp == 0))                                                                                      \
            goto found;                                                                                                \
          BINARY_BRANCHLESS_SEARCH_CYCLE_END(it, cmp, lo, scope);                                                      \
          TRACE("== lo %zi, size %zi", lo, scope);                                                                     \
        } while (scope > 0);                                                                                           \
                                                                                                                       \
        it += cmp & 1;                                                                                                 \
        /* store the key index */                                                                                      \
        mc->ki[mc->top] = (indx_t)it;                                                                                  \
        ret.node =                                                                                                     \
            (it < nkeys) ? /* fake for DUPFIX */ node : /* There is no entry larger or equal to the key. */ nullptr;   \
        TRACE("<< lo %zu, size %zu, nkeys %zu, i %zu, %c", lo, scope, nkeys, it, 'N');                                 \
        return ret;                                                                                                    \
      } else {                                                                                                         \
        cASSERT0(mc, !is_dupfix_leaf(mp));                                                                             \
        TRACE(">> %s lo %zu, size %zu, nkeys %zu", "leaf", lo, scope, nkeys);                                          \
        do {                                                                                                           \
          MDBX_CURSOR_STC_INC(mc);                                                                                     \
          BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN(it, cmp, lo, scope);                                                    \
          node = page_node(mp, it);                                                                                    \
          node_key = get_key(node);                                                                                    \
          cASSERT0(mc, ptr_disp(mp, mc->txn->env->ps) >= ptr_disp(node_key.iov_base, node_key.iov_len));               \
          cmp = LEAF_COMPARATOR(&node_key, key);                                                                       \
          TRACE("== i %zu, cmp %zi", it, cmp);                                                                         \
          if (unlikely(cmp == 0))                                                                                      \
            goto found;                                                                                                \
          BINARY_BRANCHLESS_SEARCH_CYCLE_END(it, cmp, lo, scope);                                                      \
          TRACE("== lo %zi, size %zi", lo, scope);                                                                     \
        } while (scope > 0);                                                                                           \
                                                                                                                       \
        it += cmp & 1;                                                                                                 \
        /* store the key index */                                                                                      \
        mc->ki[mc->top] = (indx_t)it;                                                                                  \
        ret.node = (it < nkeys) ? page_node(mp, it) : /* There is no entry larger or equal to the key. */ nullptr;     \
        TRACE("<< lo %zu, size %zu, nkeys %zu, i %zu, %c", lo, scope, nkeys, it, 'N');                                 \
        return ret;                                                                                                    \
      }                                                                                                                \
    }                                                                                                                  \
                                                                                                                       \
    TRACE(">> %s lo %zu, size %zu, nkeys %zu", "branch", lo, scope, nkeys);                                            \
    ASSERT(!is_dupfix_leaf(mp));                                                                                       \
    ASSERT(is_branch(mp) || !(BOOL_DIFFERENT_COMPARATORS | BOOL_DUPFIXED));                                            \
    do {                                                                                                               \
      MDBX_CURSOR_STC_INC(mc);                                                                                         \
      BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN(it, cmp, lo, scope);                                                        \
      node = page_node(mp, it);                                                                                        \
      node_key = get_key(node);                                                                                        \
      cASSERT0(mc, ptr_disp(mp, mc->txn->env->ps) >= ptr_disp(node_key.iov_base, node_key.iov_len));                   \
      cmp = BRANCH_COMPARATOR(&node_key, key);                                                                         \
      TRACE("== i %zu, cmp %zi", it, cmp);                                                                             \
      if (unlikely(cmp == 0))                                                                                          \
        goto found;                                                                                                    \
      BINARY_BRANCHLESS_SEARCH_CYCLE_END(it, cmp, lo, scope);                                                          \
      TRACE("== lo %zi, size %zi", lo, scope);                                                                         \
    } while (scope > 0);                                                                                               \
                                                                                                                       \
    it += cmp & 1;                                                                                                     \
    /* store the key index */                                                                                          \
    mc->ki[mc->top] = (indx_t)it;                                                                                      \
    ret.node = (it < nkeys) ? page_node(mp, it) : /* There is no entry larger or equal to the key. */ nullptr;         \
    TRACE("<< lo %zu, size %zu, nkeys %zu, i %zu, %c", lo, scope, nkeys, it, 'N');                                     \
    return ret;                                                                                                        \
                                                                                                                       \
  found:                                                                                                               \
    TRACE("<< lo %zu, size %zu, nkeys %zu, i %zu, %c", lo, scope, nkeys, it, 'Y');                                     \
    mc->ki[mc->top] = (indx_t)it;                                                                                      \
    ret.node = node;                                                                                                   \
    ret.exact = true;                                                                                                  \
    return ret;                                                                                                        \
  }

SEARCH_FOLIAGE(lexical_usual, cmp_lexical, null_comparator, false, false)
SEARCH_FOLIAGE(reverse_usual, cmp_reverse, null_comparator, false, false)
SEARCH_FOLIAGE(lenfast_usual, cmp_lenfast, null_comparator, false, false)
SEARCH_FOLIAGE(custom_usual, mc->clc->k.cmp, null_comparator, false, false)

#if defined(cmp_uint_align4)
SEARCH_FOLIAGE(ordinal_usual, cmp_uint_align4, null_comparator, false, false)
#else
SEARCH_FOLIAGE(ordinal_usual, cmp_uint_align4, cmp_uint_unaligned, true, false)
#endif

#if defined(cmp_uint32_align4_unchecked)
SEARCH_FOLIAGE(uint32_usual, cmp_uint32_align4_unchecked, null_comparator, false, false)
#else
SEARCH_FOLIAGE(uint32_usual, cmp_uint32_align4_unchecked, cmp_uint32_unaligned_unchecked, true, false)
#endif

#if defined(cmp_uint64_align4_unchecked)
SEARCH_FOLIAGE(uint64_usual, cmp_uint64_align4_unchecked, null_comparator, false, false)
#else
SEARCH_FOLIAGE(uint64_usual, cmp_uint64_align4_unchecked, cmp_uint64_unaligned_unchecked, true, false)
#endif

SEARCH_FOLIAGE(lexical_dupfix, cmp_lexical, cmp_lexical, false, true)
SEARCH_FOLIAGE(reverse_dupfix, cmp_reverse, cmp_reverse, false, true)
SEARCH_FOLIAGE(lenfast_dupfix, cmp_lenfast, cmp_lenfast, false, true)
SEARCH_FOLIAGE(custom_dupfix, mc->clc->k.cmp, mc->clc->k.cmp, false, true)

SEARCH_FOLIAGE(ordinal_dupfix, cmp_uint_align4, cmp_uint_unaligned, true, true)
SEARCH_FOLIAGE(uint32_dupfix, cmp_uint32_align4_unchecked, cmp_uint32_unaligned_unchecked, true, true)
SEARCH_FOLIAGE(uint64_dupfix, cmp_uint64_align4_unchecked, cmp_uint64_unaligned_unchecked, true, true)

#if MDBX_DEBUG_SEARCH_DISPATCHING

MDBX_MAYBE_UNUSED __cold static sfr_t old_node_search(MDBX_cursor *mc, const MDBX_val *key) {
  page_t *mp = mc->pg[mc->top];
  const intptr_t nkeys = page_numkeys(mp);
  DEBUG("searching %zu keys in %s %spage %" PRIaPGNO, nkeys, is_leaf(mp) ? "leaf" : "branch",
        is_subpage(mp) ? "sub-" : "", mp->pgno);

  sfr_t ret;
  ret.exact = false;
  STATIC_ASSERT(P_BRANCH == 1);
  intptr_t lo = mp->flags & P_BRANCH;
  intptr_t hi = nkeys - 1;
  if (unlikely(hi < lo)) {
    mc->ki[mc->top] = 0;
    ret.node = nullptr;
    return ret;
  }

  intptr_t i;
  MDBX_cmp_func comparator = mc->clc->k.cmp;
  MDBX_val nodekey;
  if (unlikely(is_dupfix_leaf(mp))) {
    cASSERT0(mc, mp->dupfix_ksize == mc->tree->dupfix_size);
    nodekey.iov_len = mp->dupfix_ksize;
    TRACE(">> lo %zi, hi %zi, size %zi, nkeys %zu", lo, hi, hi - lo + 1, nkeys);
    do {
      MDBX_CURSOR_STC_INC(mc);
      i = (lo + hi) >> 1;
      nodekey.iov_base = page_dupfix_ptr(mp, i, nodekey.iov_len);
      cASSERT0(mc, ptr_disp(mp, mc->txn->env->ps) >= ptr_disp(nodekey.iov_base, nodekey.iov_len));
      int cmp = comparator(key, &nodekey);
      TRACE("== i %zu, cmp %i", i, cmp);
      if (cmp > 0)
        lo = ++i;
      else if (cmp < 0)
        hi = i - 1;
      else {
        ret.exact = true;
        break;
      }
      TRACE("== lo %zi, hi %zi, size %zi", lo, hi, hi - lo + 1);
    } while (likely(lo <= hi));

    TRACE("<< lo %zi, hi %zi, size %zi, nkeys %zu, i %zu, %c", lo, hi, hi - lo + 1, nkeys, i, ret.exact ? 'Y' : 'N');

    /* store the key index */
    mc->ki[mc->top] = (indx_t)i;
    ret.node = (i < nkeys) ? /* fake for DUPFIX */ (node_t *)(intptr_t)-1
                           : /* There is no entry larger or equal to the key. */ nullptr;
    return ret;
  }

  if (MDBX_UNALIGNED_OK < 4 && is_branch(mp) && comparator == cmp_uint_align2)
    /* Branch pages have no data, so if using integer keys,
     * alignment is guaranteed. Use faster cmp_uint_align4(). */
    comparator = cmp_uint_align4;

  node_t *node;
  TRACE(">> lo %zi, hi %zi, size %zi, nkeys %zu", lo, hi, hi - lo + 1, nkeys);
  do {
    MDBX_CURSOR_STC_INC(mc);
    i = (lo + hi) >> 1;
    node = page_node(mp, i);
    nodekey.iov_len = node_ks(node);
    nodekey.iov_base = node_key(node);
    cASSERT0(mc, ptr_disp(mp, mc->txn->env->ps) >= ptr_disp(nodekey.iov_base, nodekey.iov_len));
    int cmp = comparator(key, &nodekey);
    TRACE("== i %zu, cmp %i", i, cmp);
    if (cmp > 0)
      lo = ++i;
    else if (cmp < 0)
      hi = i - 1;
    else {
      ret.exact = true;
      break;
    }
    TRACE("== lo %zi, hi %zi, size %zi", lo, hi, hi - lo + 1);
  } while (likely(lo <= hi));

  TRACE("<< lo %zi, hi %zi, size %zi, nkeys %zu, i %zu, %c", lo, hi, hi - lo + 1, nkeys, i, ret.exact ? 'Y' : 'N');

  /* store the key index */
  mc->ki[mc->top] = (indx_t)i;
  ret.node = (i < nkeys) ? page_node(mp, i) : /* There is no entry larger or equal to the key. */ nullptr;
  return ret;
}

MDBX_MAYBE_UNUSED __cold static size_t old_branch_search(MDBX_cursor *mc, const MDBX_val *key) {
  sfr_t nsr = old_node_search(mc, key);
  size_t indx = page_numkeys(mc->pg[mc->top]) - 1;
  if (likely(nsr.node))
    indx = mc->ki[mc->top] + (intptr_t)nsr.exact - 1;
  return indx;
}

#endif /* MDBX_DEBUG_SEARCH_DISPATCHING */

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION __hot static MDBX_search_foliage
cursor_to_search_foliage(const MDBX_cursor *mc) {
  MDBX_cmp_func comparator = mc->clc->k.cmp;
  ASSERT(comparator != nullptr);

  if ((mc->tree->flags & MDBX_DUPFIXED) && is_inner(mc)) {

    if (comparator == cmp_lexical)
      return search_foliage_lexical_dupfix;
    if (comparator == cmp_reverse)
      return search_foliage_reverse_dupfix;
    if (comparator == cmp_lenfast)
      return search_foliage_lenfast_dupfix;

    if (mc->tree->flags & MDBX_INTEGERKEY) {
      size_t ordinal = 0, keylen = mc->tree->dupfix_size;
      cASSERT0(mc, mc->clc->k.lmax == mc->clc->k.lmin && mc->clc->k.lmax == keylen && (keylen == 4 || keylen == 8));
#ifndef cmp_uint_align2
      if (comparator == cmp_uint_align2)
        ordinal = keylen;
#endif /* cmp_uint_align2 */
#ifndef cmp_uint_align4
      if (comparator == cmp_uint_align4)
        ordinal = keylen;
#endif /* cmp_uint_align4 */
      if (comparator == cmp_uint_unaligned)
        ordinal = keylen;
      if (ordinal) {
        if ((mc->txn->env->flags & MDBX_VALIDATION) == 0) {
          if (ordinal == 4)
            return search_foliage_uint32_dupfix;
          if (ordinal == 8)
            return search_foliage_uint64_dupfix;
        }
        return search_foliage_ordinal_dupfix;
      }
    }

    return search_foliage_custom_dupfix;
  }

  if (comparator == cmp_lexical)
    return search_foliage_lexical_usual;
  if (comparator == cmp_reverse)
    return search_foliage_reverse_usual;
  if (comparator == cmp_lenfast)
    return search_foliage_lenfast_usual;

  if (mc->tree->flags & MDBX_INTEGERKEY) {
    size_t ordinal = 0, keylen = mc->clc->k.lmax;
    cASSERT0(mc, mc->clc->k.lmax == mc->clc->k.lmin && (keylen == 4 || keylen == 8));
#ifndef cmp_uint_align2
    if (comparator == cmp_uint_align2)
      ordinal = keylen;
#endif /* cmp_uint_align2 */
#ifndef cmp_uint_align4
    if (comparator == cmp_uint_align4)
      ordinal = keylen;
#endif /* cmp_uint_align4 */
    if (comparator == cmp_uint_unaligned)
      ordinal = keylen;
    if (ordinal) {
      if ((mc->txn->env->flags & MDBX_VALIDATION) == 0 && ordinal == mc->clc->k.lmin) {
        if (ordinal == 4)
          return search_foliage_uint32_usual;
        if (ordinal == 8)
          return search_foliage_uint64_usual;
      }
      return search_foliage_ordinal_usual;
    }
  }

  return search_foliage_custom_usual;
}

__hot sfr_t tree_search_foliage_configure(MDBX_cursor *mc, const MDBX_val *key) {
  MDBX_search_foliage search_foliage = cursor_to_search_foliage(mc);
#if MDBX_DEBUG_SEARCH_DISPATCHING
  const unsigned snap_1 = MDBX_CURSOR_STC_GET(mc);
  sfr_t old = old_node_search(mc, key);
  int old_i = mc->ki[mc->top];
  const unsigned snap_2 = MDBX_CURSOR_STC_GET(mc);
  const unsigned old_steps = snap_2 - snap_1;

  sfr_t new = search_foliage(mc, key);
  int new_i = mc->ki[mc->top];
  const unsigned snap_3 = MDBX_CURSOR_STC_GET(mc);
  const unsigned new_steps = snap_3 - snap_2;

  if (new_i != old_i || new.exact != old.exact || new.node != old.node || new_steps > old_steps) {
    globals.loglevel = MDBX_LOG_TRACE;
    WARNING("\nfoliage-search-issue: new %i, %c, %p, steps %u | old %i, %c, %p, steps %u | retry to debug...", new_i,
            new.exact ? 'Y' : 'N', (void *)new.node, new_steps, old_i, old.exact ? 'Y' : 'N', (void *)old.node,
            old_steps);
    old_node_search((MDBX_cursor *)mc, key);
    search_foliage(mc, key);
    panic_fmt(mc, "new %i, %c, %p, steps %u | old %i, %c, %p, steps %u", new_i, new.exact ? 'Y' : 'N', (void *)new.node,
              new_steps, old_i, old.exact ? 'Y' : 'N', (void *)old.node, old_steps);
  }
  return new;
#else
  mc->clc->k.search_foliage = search_foliage;
  return search_foliage(mc, key);
#endif /* MDBX_DEBUG_SEARCH_DISPATCHING */
}

/* ---------------------------------------------------------------------------------------------------- */

#define SEARCH_BRANCH(NAME, COMPARATOR)                                                                                \
  MDBX_NOTHROW_PURE_FUNCTION __hot static size_t search_branch_##NAME(const MDBX_cursor *mc, const MDBX_val *key) {    \
    page_t *mp = mc->pg[mc->top];                                                                                      \
    ASSERT(is_branch(mp));                                                                                             \
    const size_t nkeys = page_numkeys(mp);                                                                             \
    cASSERT0(mc, nkeys >= 2);                                                                                          \
    TRACE("searching %zu keys in branch-page %" PRIaPGNO, nkeys, mp->pgno);                                            \
    intptr_t lo = 1, scope = nkeys - lo, it, cmp;                                                                      \
    TRACE(">> lo %zu, size %zu, nkeys %zu", lo, scope, nkeys);                                                         \
    if (likely(nkeys > 1))                                                                                             \
      do {                                                                                                             \
        MDBX_CURSOR_STC_INC(mc);                                                                                       \
        BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN(it, cmp, lo, scope);                                                      \
        MDBX_val node_key = get_key(page_node(mp, it));                                                                \
        cASSERT0(mc, ptr_disp(mp, mc->txn->env->ps) >= ptr_disp(node_key.iov_base, node_key.iov_len));                 \
        cmp = COMPARATOR(&node_key, key);                                                                              \
        TRACE("== i %zu, cmp %zi", it, cmp);                                                                           \
        if (unlikely(cmp == 0)) {                                                                                      \
          TRACE("<< lo %zu, size %zu, nkeys %zu, i %zu, %c", lo, scope, nkeys, it, 'Y');                               \
          return it;                                                                                                   \
        }                                                                                                              \
        BINARY_BRANCHLESS_SEARCH_CYCLE_END(it, cmp, lo, scope);                                                        \
        TRACE("== lo %zi, size %zi", lo, scope);                                                                       \
      } while (likely(scope > 0));                                                                                     \
    it = lo - 1;                                                                                                       \
    TRACE("<< lo %zu, size %zu, nkeys %zu, i %zu, %c", lo, scope, nkeys, it, 'N');                                     \
    return it;                                                                                                         \
  }

/* Branch pages have no data, so if using integer keys, alignment is guaranteed. Use faster cmp_uint_align4(). */
SEARCH_BRANCH(ordinal, cmp_uint_align4)
SEARCH_BRANCH(uint32, cmp_uint32_align4_unchecked)
SEARCH_BRANCH(uint64, cmp_uint64_align4_unchecked)
SEARCH_BRANCH(lexical, cmp_lexical)
SEARCH_BRANCH(reverse, cmp_reverse)
SEARCH_BRANCH(lenfast, cmp_lenfast)
SEARCH_BRANCH(custom, mc->clc->k.cmp)

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION __hot static MDBX_search_branch
cursor_to_search_branch(const MDBX_cursor *mc) {
  MDBX_cmp_func comparator = mc->clc->k.cmp;
  ASSERT(comparator != nullptr);

  if (comparator == cmp_lexical)
    return search_branch_lexical;
  if (comparator == cmp_reverse)
    return search_branch_reverse;
  if (comparator == cmp_lenfast)
    return search_branch_lenfast;

  if (mc->tree->flags & MDBX_INTEGERKEY) {
    size_t ordinal = 0, keylen = mc->clc->k.lmax;
#ifndef cmp_uint_align2
    if (comparator == cmp_uint_align2)
      ordinal = keylen;
#endif /* cmp_uint_align2 */
#ifndef cmp_uint_align4
    if (comparator == cmp_uint_align4)
      ordinal = keylen;
#endif /* cmp_uint_align4 */
    if (comparator == cmp_uint_unaligned)
      ordinal = keylen;
    if (ordinal) {
      if ((mc->txn->env->flags & MDBX_VALIDATION) == 0 && ordinal == mc->clc->k.lmin) {
        if (ordinal == 4)
          return search_branch_uint32;
        if (ordinal == 8)
          return search_branch_uint64;
      }
      return search_branch_ordinal;
    }
  }

  return search_branch_custom;
}

size_t tree_search_branch_configure(const MDBX_cursor *mc, const MDBX_val *key) {
  MDBX_search_branch search_branch = cursor_to_search_branch(mc);
#if MDBX_DEBUG_SEARCH_DISPATCHING
  const unsigned snap_1 = MDBX_CURSOR_STC_GET(mc);
  size_t old_i = old_branch_search((MDBX_cursor *)mc, key);
  const unsigned snap_2 = MDBX_CURSOR_STC_GET(mc);
  const unsigned old_steps = snap_2 - snap_1;

  size_t new_i = search_branch(mc, key);
  const unsigned snap_3 = MDBX_CURSOR_STC_GET(mc);
  const unsigned new_steps = snap_3 - snap_2;

  if (new_i != old_i || new_steps > old_steps) {
    globals.loglevel = MDBX_LOG_TRACE;
    WARNING("\nbranch-search-issue: new %zi, steps %u | old %zi, steps %u | retry to debug...", new_i, new_steps, old_i,
            old_steps);
    old_branch_search((MDBX_cursor *)mc, key);
    search_branch(mc, key);
    panic_fmt(mc, "new %zi, steps %u | old %zi, steps %u", new_i, new_steps, old_i, old_steps);
  }
  return new_i;
#else
  mc->clc->k.search_branch = search_branch;
  return search_branch(mc, key);
#endif /* MDBX_DEBUG_SEARCH_DISPATCHING */
}

#undef CLEAR_VALUE_PROPAGATION
#undef BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN
#undef BINARY_BRANCHLESS_SEARCH_CYCLE_END
#undef SEARCH_BRANCH
#undef SEARCH_FOLIAGE
