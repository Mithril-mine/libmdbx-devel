/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

typedef struct walk_ctx {
  void *userctx;
  walk_options_t options;
  int deep;
  walk_func *visitor;
  MDBX_txn *txn;
  MDBX_cursor *cursor;
  /* A valid copy-on-write b-tree references every page exactly once, so the
   * walk marks each visited page in a one-bit-per-page bitmap and rejects any
   * second visit (a cycle or a reconverging DAG) as corruption, which also
   * bounds the whole traversal (issue #82). */
  uint8_t *visited;
  size_t visited_size;
  size_t walk_limit;
  size_t nwalked;
} walk_ctx_t;

static void walk_ctx_reset(walk_ctx_t *ctx) {
  if (likely(ctx->visited)) {
    /* the whole bitset lives in a single allocation: memset over it */
    memset(ctx->visited, 0, ctx->visited_size);
  }
  ctx->nwalked = 0;
}

/* Mark a page as walked; return MDBX_CORRUPTED on a second visit, or when the
 * total number of walked pages exceeds the file's page count (a valid tree
 * never enters more pages than exist and never references a page twice).
 * With `dont_check_revisit` both guards are disabled: the caller (the `chk`
 * machinery) has its own loop/dup detection and must see such trees whole. */
static int walk_visit(walk_ctx_t *ctx, const pgno_t pgno) {
  if (likely(!(ctx->options & dont_check_revisit))) {
    if (unlikely(++ctx->nwalked > ctx->walk_limit)) {
      ERROR("walk: too many pages, %zu > limit %zu", ctx->nwalked, ctx->walk_limit);
      return MDBX_CORRUPTED;
    }
    if (likely(ctx->visited)) {
      if (pgno < ctx->visited_size * 8) {
        const uint8_t bit = (uint8_t)(1u << (pgno & 7));
        uint8_t *const byte = ctx->visited + (pgno >> 3);
        if (unlikely(*byte & bit))
          return MDBX_CORRUPTED;
        *byte |= bit;
      }
    }
  }
  return MDBX_SUCCESS;
}

__cold static int walk_tbl(walk_ctx_t *ctx, walk_tbl_t *tbl);

static page_type_t walk_page_type(const page_t *mp) {
  if (mp)
    switch (mp->flags & ~P_SPILLED) {
    case P_BRANCH:
      return page_branch;
    case P_LEAF:
      return page_leaf;
    case P_LEAF | P_DUPFIX:
      return page_dupfix_leaf;
    case P_LARGE:
      return page_large;
    }
  return page_broken;
}

static page_type_t walk_subpage_type(const page_t *sp) {
  switch (sp->flags & /* ignore legacy P_DIRTY flag */ ~P_LEGACY_DIRTY) {
  case P_LEAF | P_SUBP:
    return page_sub_leaf;
  case P_LEAF | P_DUPFIX | P_SUBP:
    return page_sub_dupfix_leaf;
  default:
    return page_sub_broken;
  }
}

/* Depth-first tree traversal. */
__cold static int walk_pgno(walk_ctx_t *ctx, walk_tbl_t *tbl, const pgno_t pgno, txnid_t parent_txnid) {
  assert(pgno != P_INVALID);
  int err = walk_visit(ctx, pgno);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  page_t *mp = nullptr;
  err = page_get(ctx->cursor, pgno, &mp, parent_txnid);

  const page_type_t type = walk_page_type(mp);
  const size_t nentries = mp ? page_numkeys(mp) : 0;
  size_t header_size = (mp && !is_dupfix_leaf(mp)) ? PAGEHDRSZ + mp->lower : PAGEHDRSZ;
  size_t payload_size = 0;
  size_t unused_size = (mp ? page_room(mp) : ctx->txn->env->ps - header_size) - payload_size;
  size_t align_bytes = 0;

  for (size_t i = 0; err == MDBX_SUCCESS && i < nentries; ++i) {
    if (type == page_dupfix_leaf) {
      /* DUPFIX pages have no entries[] or node headers */
      payload_size += mp->dupfix_ksize;
      continue;
    }

    const node_t *node = page_node(mp, i);
    header_size += NODESIZE;
    const size_t node_key_size = node_ks(node);
    payload_size += node_key_size;

    if (type == page_branch) {
      assert(i > 0 || node_ks(node) == 0);
      align_bytes += node_key_size & 1;
      continue;
    }

    const size_t node_data_size = node_ds(node);
    assert(type == page_leaf);
    switch (node_flags(node)) {
    case 0 /* usual node */:
      payload_size += node_data_size;
      align_bytes += (node_key_size + node_data_size) & 1;
      break;

    case N_BIG /* long data on the large/overflow page */: {
      const pgno_t large_pgno = node_largedata_pgno(node);
      const int wrc = walk_visit(ctx, large_pgno);
      if (unlikely(wrc != MDBX_SUCCESS))
        return wrc;
      const size_t over_payload = node_data_size;
      const size_t over_header = PAGEHDRSZ;

      assert(err == MDBX_SUCCESS);
      pgr_t lp = page_get_large(ctx->cursor, large_pgno, mp->txnid);
      const size_t npages = ((err = lp.err) == MDBX_SUCCESS) ? lp.page->pages : 1;
      const size_t pagesize = pgno2bytes(ctx->txn->env, npages);
      const size_t over_unused = pagesize - over_payload - over_header;
      const int rc = ctx->visitor(large_pgno, (unsigned)npages, ctx->userctx, ctx->deep, tbl, pagesize, page_large, err,
                                  1, over_payload, over_header, over_unused);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_RESULT_TRUE) ? MDBX_SUCCESS : rc;
      payload_size += sizeof(pgno_t);
      align_bytes += node_key_size & 1;
    } break;

    case N_TREE /* sub-db */: {
      if (unlikely(node_data_size != sizeof(tree_t))) {
        ERROR("%s/%d: %s %u", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid table node size", (unsigned)node_data_size);
        assert(err == MDBX_CORRUPTED);
        err = MDBX_CORRUPTED;
      }
      header_size += node_data_size;
      align_bytes += (node_key_size + node_data_size) & 1;
    } break;

    case N_TREE | N_DUP /* dupsorted sub-tree */:
      if (unlikely(node_data_size != sizeof(tree_t))) {
        ERROR("%s/%d: %s %u", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid sub-tree node size", (unsigned)node_data_size);
        assert(err == MDBX_CORRUPTED);
        err = MDBX_CORRUPTED;
      }
      header_size += node_data_size;
      align_bytes += (node_key_size + node_data_size) & 1;
      break;

    case N_DUP /* short sub-page */: {
      if (unlikely(node_data_size <= PAGEHDRSZ || (node_data_size & 1))) {
        ERROR("%s/%d: %s %u", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid sub-page node size", (unsigned)node_data_size);
        assert(err == MDBX_CORRUPTED);
        err = MDBX_CORRUPTED;
        break;
      }

      const page_t *const sp = node_data(node);
      const page_type_t subtype = walk_subpage_type(sp);
      const size_t nsubkeys = page_numkeys(sp);
      if (unlikely(subtype == page_sub_broken)) {
        ERROR("%s/%d: %s 0x%x", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid sub-page flags", sp->flags);
        assert(err == MDBX_CORRUPTED);
        err = MDBX_CORRUPTED;
      }

      size_t subheader_size = is_dupfix_leaf(sp) ? PAGEHDRSZ : PAGEHDRSZ + sp->lower;
      size_t subunused_size = page_room(sp);
      size_t subpayload_size = 0;
      size_t subalign_bytes = 0;

      for (size_t ii = 0; err == MDBX_SUCCESS && ii < nsubkeys; ++ii) {
        if (subtype == page_sub_dupfix_leaf) {
          /* DUPFIX pages have no entries[] or node headers */
          subpayload_size += sp->dupfix_ksize;
        } else {
          assert(subtype == page_sub_leaf);
          const node_t *subnode = page_node(sp, ii);
          const size_t subnode_size = node_ks(subnode) + node_ds(subnode);
          subheader_size += NODESIZE;
          subpayload_size += subnode_size;
          subalign_bytes += subnode_size & 1;
          if (unlikely(node_flags(subnode) != 0)) {
            ERROR("%s/%d: %s 0x%x", "MDBX_CORRUPTED", MDBX_CORRUPTED, "unexpected sub-node flags", node_flags(subnode));
            assert(err == MDBX_CORRUPTED);
            err = MDBX_CORRUPTED;
          }
        }
      }

      const int rc = ctx->visitor(pgno, 0, ctx->userctx, ctx->deep + 1, tbl, node_data_size, subtype, err, nsubkeys,
                                  subpayload_size, subheader_size, subunused_size + subalign_bytes);
      if (unlikely(rc != MDBX_SUCCESS))
        return (rc == MDBX_RESULT_TRUE) ? MDBX_SUCCESS : rc;
      header_size += subheader_size;
      unused_size += subunused_size;
      payload_size += subpayload_size;
      align_bytes += subalign_bytes + (node_key_size & 1);
    } break;

    default:
      ERROR("%s/%d: %s 0x%x", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid node flags", node_flags(node));
      assert(err == MDBX_CORRUPTED);
      err = MDBX_CORRUPTED;
    }
  }

  int rc = ctx->visitor(pgno, 1, ctx->userctx, ctx->deep, tbl, ctx->txn->env->ps, type, err, nentries, payload_size,
                        header_size, unused_size + align_bytes);
  if (unlikely(rc != MDBX_SUCCESS || !mp))
    return rc;

  for (size_t i = 0; err == MDBX_SUCCESS && i < nentries; ++i) {
    if (type == page_dupfix_leaf)
      continue;

    node_t *node = page_node(mp, i);
    if (type == page_branch) {
      assert(err == MDBX_SUCCESS);
      ctx->deep += 1;
      err = walk_pgno(ctx, tbl, node_pgno(node), mp->txnid);
      ctx->deep -= 1;
      if (unlikely(err != MDBX_SUCCESS)) {
        if (err == MDBX_RESULT_TRUE)
          break;
        return err;
      }
      continue;
    }

    assert(type == page_leaf);
    switch (node_flags(node)) {
    default:
      continue;

    case N_TREE /* sub-db */:
      if (unlikely(node_ds(node) != sizeof(tree_t))) {
        ERROR("%s/%d: %s %u", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid sub-tree node size", (unsigned)node_ds(node));
        assert(err == MDBX_CORRUPTED);
        err = MDBX_CORRUPTED;
      } else {
        tree_t aligned_db;
        memcpy(&aligned_db, node_data(node), sizeof(aligned_db));
        walk_tbl_t table = {{node_key(node), node_ks(node)}, nullptr, nullptr};
        table.internal = &aligned_db;
        assert(err == MDBX_SUCCESS);
        ctx->deep += 1;
        err = walk_tbl(ctx, &table);
        ctx->deep -= 1;
      }
      break;

    case N_TREE | N_DUP /* dupsorted sub-tree */:
      if (unlikely(node_ds(node) != sizeof(tree_t))) {
        ERROR("%s/%d: %s %u", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid dupsort sub-tree node size",
              (unsigned)node_ds(node));
        assert(err == MDBX_CORRUPTED);
        err = MDBX_CORRUPTED;
      } else {
        tree_t aligned_db;
        memcpy(&aligned_db, node_data(node), sizeof(aligned_db));
        assert(err == MDBX_SUCCESS);
        err = cursor_dupsort_setup(ctx->cursor, node, mp);
        if (likely(err == MDBX_SUCCESS)) {
          assert(ctx->cursor->subcur == &container_of(ctx->cursor, cursor_couple_t, outer)->inner);
          ctx->cursor = &ctx->cursor->subcur->cursor;
          ctx->deep += 1;
          tbl->nested = &aligned_db;
          err = walk_pgno(ctx, tbl, aligned_db.root, mp->txnid);
          tbl->nested = nullptr;
          ctx->deep -= 1;
          subcur_t *inner_xcursor = container_of(ctx->cursor, subcur_t, cursor);
          cursor_couple_t *couple = container_of(inner_xcursor, cursor_couple_t, inner);
          ctx->cursor = &couple->outer;
        }
      }
      break;
    }
  }

  return MDBX_SUCCESS;
}

__cold static int walk_tbl(walk_ctx_t *ctx, walk_tbl_t *tbl) {
  tree_t *const db = tbl->internal;
  if (unlikely(db->root == P_INVALID))
    return MDBX_SUCCESS; /* empty db */

  kvx_t kvx = {.clc = {.k = {.lmin = INT_MAX}, .v = {.lmin = INT_MAX}}};
  cursor_couple_t couple;
  int rc = cursor_init4walk(&couple, ctx->txn, db, &kvx);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const uint8_t cursor_checking = (ctx->options & dont_check_keys_ordering) ? z_pagecheck | z_ignord : z_pagecheck;
  couple.outer.checking |= cursor_checking;
  couple.inner.cursor.checking |= cursor_checking;
  couple.outer.next = ctx->cursor;
  couple.outer.top_and_flags = z_disable_tree_search_fastpath;
  ctx->cursor = &couple.outer;
  rc = walk_pgno(ctx, tbl, db->root, db->mod_txnid ? db->mod_txnid : ctx->txn->txnid);
  ctx->cursor = couple.outer.next;
  return rc;
}

__cold int walk_pages(MDBX_txn *txn, walk_func *visitor, void *user, walk_options_t options) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  walk_ctx_t ctx = {.txn = txn, .userctx = user, .visitor = visitor, .options = options};

  /* A legitimate walk enters each page exactly once, so the total number of
   * entries never exceeds the file's page count; use it both as a hard limit
   * and (capped to keep the allocation bounded) as a one-bit-per-page marker
   * to reject duplicate references (cycles and DAGs) on the spot. With
   * dont_check_revisit both are skipped and no marker is allocated. */
  const size_t npages = txn->geo.first_unallocated;
  ctx.walk_limit = npages;
  if (likely(!(options & dont_check_revisit))) {
    const size_t visited_max = (npages < (1u << 24)) ? npages : (1u << 24); /* 2 MiB bitmap cap */
    if (likely(visited_max > 0)) {
      ctx.visited = (uint8_t *)osal_malloc((visited_max + 7) / 8);
      if (unlikely(!ctx.visited))
        return MDBX_ENOMEM;
      ctx.visited_size = (visited_max + 7) / 8;
    }
  }
  walk_ctx_reset(&ctx);

  walk_tbl_t tbl = {.name = {.iov_base = MDBX_CHK_GC}, .internal = &txn->dbs[FREE_DBI]};
  rc = walk_tbl(&ctx, &tbl);
  if (!MDBX_IS_ERROR(rc)) {
    tbl.name.iov_base = MDBX_CHK_MAIN;
    tbl.internal = &txn->dbs[MAIN_DBI];
    rc = walk_tbl(&ctx, &tbl);
  }
  osal_free(ctx.visited);
  return rc;
}
