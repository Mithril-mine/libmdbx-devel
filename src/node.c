/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

__hot int __must_check_result node_add_dupfix(MDBX_cursor *mc, size_t indx, const MDBX_val *key) {
  page_t *mp = mc->pg[mc->top];
  MDBX_ANALYSIS_ASSUME(key != nullptr);
  DKBUF_DEBUG;
  DEBUG("add to leaf2-%spage %" PRIaPGNO " index %zi, "
        " key size %" PRIuPTR " [%s]",
        is_subpage(mp) ? "sub-" : "", mp->pgno, indx, key ? key->iov_len : 0, DKEY_DEBUG(key));

  cASSERT0(mc, key);
  cASSERT0(mc, page_type_compat(mp) == (P_LEAF | P_DUPFIX));
  const size_t ksize = mc->tree->dupfix_size;
  cASSERT0(mc, ksize == key->iov_len);
  const size_t nkeys = page_numkeys(mp);
  cASSERT0(mc, (((ksize & page_numkeys(mp)) ^ mp->upper) & 1) == 0);

  /* Just using these for counting */
  const intptr_t lower = mp->lower + sizeof(indx_t);
  const intptr_t upper = mp->upper - (ksize - sizeof(indx_t));
  if (unlikely(lower > upper)) {
    mc->txn->flags |= MDBX_TXN_ERROR;
    return MDBX_PAGE_FULL;
  }
  mp->lower = (indx_t)lower;
  mp->upper = (indx_t)upper;

  void *const ptr = page_dupfix_ptr(mp, indx, ksize);
  cASSERT0(mc, nkeys >= indx);
  const size_t diff = nkeys - indx;
  if (likely(diff > 0))
    /* Move higher keys up one slot. */
    memmove(ptr_disp(ptr, ksize), ptr, diff * ksize);
  /* insert new key */
  memcpy(ptr, key->iov_base, ksize);

  cASSERT0(mc, (((ksize & page_numkeys(mp)) ^ mp->upper) & 1) == 0);
  return MDBX_SUCCESS;
}

int __must_check_result node_add_branch(MDBX_cursor *mc, size_t indx, const MDBX_val *key, pgno_t pgno) {
  page_t *mp = mc->pg[mc->top];
  DKBUF_DEBUG;
  DEBUG("add to branch-%spage %" PRIaPGNO " index %zi, node-pgno %" PRIaPGNO " key size %" PRIuPTR " [%s]",
        is_subpage(mp) ? "sub-" : "", mp->pgno, indx, pgno, key ? key->iov_len : 0, DKEY_DEBUG(key));

  cASSERT0(mc, page_type(mp) == P_BRANCH);
  cASSERT0(mc, mp->txnid >= mc->txn->front_txnid);
  STATIC_ASSERT(NODESIZE % 2 == 0);

  /* Adjust free space offsets. */
  const size_t branch_bytes = branch_size(mc->txn->env, key);
  const intptr_t lower = mp->lower + sizeof(indx_t);
  const intptr_t upper = mp->upper - (branch_bytes - sizeof(indx_t));
  if (unlikely(lower > upper)) {
    mc->txn->flags |= MDBX_TXN_ERROR;
    return MDBX_PAGE_FULL;
  }

  /* Move higher pointers up one slot. */
  const size_t nkeys = page_numkeys(mp);
  cASSERT0(mc, nkeys >= indx);
  for (size_t i = nkeys; i > indx; --i)
    mp->entries[i] = mp->entries[i - 1];

  mp->lower = (indx_t)lower;
  mp->entries[indx] = mp->upper = (indx_t)upper;

  /* Write the node data. */
  node_t *node = page_node(mp, indx);
  node_set_pgno(node, pgno);
  node_set_flags(node, 0);
  UNALIGNED_POKE_8(node, node_t, extra, 0);
  node_set_ks(node, 0);
  if (likely(key != nullptr)) {
    node_set_ks(node, key->iov_len);
    memcpy(node_key(node), key->iov_base, key->iov_len);
  }
  return MDBX_SUCCESS;
}

__hot int __must_check_result node_add_leaf(MDBX_cursor *mc, size_t indx, const MDBX_val *key, MDBX_val *data,
                                            unsigned flags) {
  MDBX_ANALYSIS_ASSUME(key != nullptr);
  MDBX_ANALYSIS_ASSUME(data != nullptr);
  page_t *mp = mc->pg[mc->top];
  DKBUF_DEBUG;
  DEBUG("add to leaf-%spage %" PRIaPGNO " index %zi, data size %" PRIuPTR " key size %" PRIuPTR " [%s]",
        is_subpage(mp) ? "sub-" : "", mp->pgno, indx, data ? data->iov_len : 0, key ? key->iov_len : 0,
        DKEY_DEBUG(key));
  cASSERT0(mc, key != nullptr && data != nullptr);
  cASSERT0(mc, page_type_compat(mp) == P_LEAF);
  page_t *largepage = nullptr;

  size_t node_bytes;
  if (unlikely(flags & N_BIG)) {
    /* Data already on large/overflow page. */
    STATIC_ASSERT(sizeof(pgno_t) % 2 == 0);
    node_bytes = node_size_len(key->iov_len, 0) + sizeof(pgno_t) + sizeof(indx_t);
    cASSERT0(mc, page_room(mp) >= node_bytes);
  } else if (unlikely(node_size(key, data) > mc->txn->env->leaf_nodemax)) {
    /* Put data on large/overflow page. */
    if (unlikely(mc->tree->flags & MDBX_DUPSORT)) {
      ERROR("Unexpected target %s flags 0x%x for large data-item", "dupsort-db", mc->tree->flags);
      return MDBX_PROBLEM;
    }
    if (unlikely(flags & (N_DUP | N_TREE))) {
      ERROR("Unexpected target %s flags 0x%x for large data-item", "node", flags);
      return MDBX_PROBLEM;
    }
    cASSERT0(mc, page_room(mp) >= leaf_size(mc->txn->env, key, data));
    const pgno_t ovpages = largechunk_npages(mc->txn->env, data->iov_len);
    const pgr_t npr = page_new_large(mc, ovpages);
    if (unlikely(npr.err != MDBX_SUCCESS))
      return npr.err;
    largepage = npr.page;
    DEBUG("allocated %u large/overflow page(s) %" PRIaPGNO "for %" PRIuPTR " data bytes", largepage->pages,
          largepage->pgno, data->iov_len);
    flags |= N_BIG;
    node_bytes = node_size_len(key->iov_len, 0) + sizeof(pgno_t) + sizeof(indx_t);
    cASSERT0(mc, node_bytes == leaf_size(mc->txn->env, key, data));
  } else {
    cASSERT0(mc, page_room(mp) >= leaf_size(mc->txn->env, key, data));
    node_bytes = node_size(key, data) + sizeof(indx_t);
    cASSERT0(mc, node_bytes == leaf_size(mc->txn->env, key, data));
  }

  /* Move higher pointers up one slot. */
  const size_t nkeys = page_numkeys(mp);
  cASSERT0(mc, nkeys >= indx);
  for (size_t i = nkeys; i > indx; --i)
    mp->entries[i] = mp->entries[i - 1];

  /* Adjust free space offsets. */
  const intptr_t lower = mp->lower + sizeof(indx_t);
  const intptr_t upper = mp->upper - (node_bytes - sizeof(indx_t));
  if (unlikely(lower > upper)) {
    mc->txn->flags |= MDBX_TXN_ERROR;
    return MDBX_PAGE_FULL;
  }
  mp->lower = (indx_t)lower;
  mp->entries[indx] = mp->upper = (indx_t)upper;

  /* Write the node data. */
  node_t *node = page_node(mp, indx);
  node_set_ks(node, key->iov_len);
  node_set_flags(node, (uint8_t)flags);
  UNALIGNED_POKE_8(node, node_t, extra, 0);
  node_set_ds(node, data->iov_len);
  memcpy(node_key(node), key->iov_base, key->iov_len);

  void *nodedata = node_data(node);
  if (likely(largepage == nullptr)) {
    if (unlikely(flags & N_BIG)) {
      memcpy(nodedata, data->iov_base, sizeof(pgno_t));
      return MDBX_SUCCESS;
    }
  } else {
    poke_pgno(nodedata, largepage->pgno);
    nodedata = page2payload(largepage);
  }
  if (unlikely(flags & MDBX_RESERVE))
    data->iov_base = nodedata;
  else if (likely(data->iov_len /* to avoid UBSAN traps */))
    memcpy(nodedata, data->iov_base, data->iov_len);
  return MDBX_SUCCESS;
}

__hot void node_del(MDBX_cursor *mc, size_t ksize) {
  page_t *mp = mc->pg[mc->top];
  const size_t hole = mc->ki[mc->top];
  const size_t nkeys = page_numkeys(mp);

  DEBUG("delete node %zu on %s page %" PRIaPGNO, hole, is_leaf(mp) ? "leaf" : "branch", mp->pgno);
  cASSERT0(mc, hole < nkeys);

  if (is_dupfix_leaf(mp)) {
    cASSERT0(mc, ksize >= sizeof(indx_t));
    size_t diff = nkeys - 1 - hole;
    void *const base = page_dupfix_ptr(mp, hole, ksize);
    if (diff)
      memmove(base, ptr_disp(base, ksize), diff * ksize);
    cASSERT0(mc, mp->lower >= sizeof(indx_t));
    mp->lower -= sizeof(indx_t);
    cASSERT0(mc, (size_t)UINT16_MAX - mp->upper >= ksize - sizeof(indx_t));
    mp->upper += (indx_t)(ksize - sizeof(indx_t));
    cASSERT0(mc, (((ksize & page_numkeys(mp)) ^ mp->upper) & 1) == 0);
    return;
  }

  node_t *node = page_node(mp, hole);
  cASSERT0(mc, !is_branch(mp) || hole || node_ks(node) == 0);
  size_t hole_size = NODESIZE + node_ks(node);
  if (is_leaf(mp))
    hole_size += (node_flags(node) & N_BIG) ? sizeof(pgno_t) : node_ds(node);
  hole_size = EVEN_CEIL(hole_size);

  const indx_t hole_offset = mp->entries[hole];
  size_t r, w;
  for (r = w = 0; r < nkeys; r++)
    if (r != hole)
      mp->entries[w++] = (mp->entries[r] < hole_offset) ? mp->entries[r] + (indx_t)hole_size : mp->entries[r];

  void *const base = ptr_disp(mp, mp->upper + PAGEHDRSZ);
  memmove(ptr_disp(base, hole_size), base, hole_offset - mp->upper);

  cASSERT0(mc, mp->lower >= sizeof(indx_t));
  mp->lower -= sizeof(indx_t);
  cASSERT0(mc, (size_t)UINT16_MAX - mp->upper >= hole_size);
  mp->upper += (indx_t)hole_size;

  if (CHECKS2_ENABLED()) {
    const uint8_t checking = mc->checking;
    mc->checking |= z_updating;
    const int page_check_err = page_check(mc, mp);
    mc->checking = checking;
    cASSERT0(mc, page_check_err == MDBX_SUCCESS);
  }
}

__noinline int node_read_bigdata(MDBX_cursor *mc, const node_t *node, MDBX_val *data, const page_t *mp) {
  cASSERT0(mc, node_flags(node) == N_BIG && data->iov_len == node_ds(node));

  pgr_t lp = page_get_large(mc, node_largedata_pgno(node), mp->txnid);
  if (unlikely((lp.err != MDBX_SUCCESS))) {
    DEBUG("read large/overflow page %" PRIaPGNO " failed", node_largedata_pgno(node));
    return lp.err;
  }

  cASSERT0(mc, page_type(lp.page) == P_LARGE);
  data->iov_base = page2payload(lp.page);
  if (!MDBX_DISABLE_VALIDATION) {
    const MDBX_env *env = mc->txn->env;
    const size_t dsize = data->iov_len;
    const unsigned npages = largechunk_npages(env, dsize);
    if (unlikely(lp.page->pages < npages))
      return bad_page(lp.page, "too less n-pages %u for bigdata-node (%zu bytes)", lp.page->pages, dsize);
  }
  return MDBX_SUCCESS;
}

node_t *node_shrink(page_t *mp, size_t indx, node_t *node) {
  ASSERT(node == page_node(mp, indx));
  page_t *sp = (page_t *)node_data(node);
  ASSERT(is_subpage(sp) && page_numkeys(sp) > 0);
  const size_t delta = EVEN_FLOOR(page_room(sp) /* avoid the node uneven-sized */);
  if (unlikely(delta) == 0)
    return node;

  /* Prepare to shift upward, set len = length(subpage part to shift) */
  size_t nsize = node_ds(node) - delta, len = nsize;
  ASSERT(nsize % 1 == 0);
  if (!is_dupfix_leaf(sp)) {
    len = PAGEHDRSZ;
    page_t *xp = ptr_disp(sp, delta); /* destination subpage */
    for (intptr_t i = page_numkeys(sp); --i >= 0;) {
      ASSERT(sp->entries[i] >= delta);
      xp->entries[i] = (indx_t)(sp->entries[i] - delta);
    }
  }
  ASSERT(sp->upper >= sp->lower + delta);
  sp->upper -= (indx_t)delta;
  sp->pgno = mp->pgno;
  node_set_ds(node, nsize);

  /* Shift <lower nodes...initial part of subpage> upward */
  void *const base = ptr_disp(mp, mp->upper + PAGEHDRSZ);
  memmove(ptr_disp(base, delta), base, ptr_dist(sp, base) + len);

  const size_t pivot = mp->entries[indx];
  for (intptr_t i = page_numkeys(mp); --i >= 0;) {
    if (mp->entries[i] <= pivot) {
      ASSERT((size_t)UINT16_MAX - mp->entries[i] >= delta);
      mp->entries[i] += (indx_t)delta;
    }
  }
  ASSERT((size_t)UINT16_MAX - mp->upper >= delta);
  mp->upper += (indx_t)delta;

  return ptr_disp(node, delta);
}
