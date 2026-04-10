/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

__hot txnid_t tbl_root_txnid(const MDBX_txn *txn, const size_t dbi) {
  if ((txn->flags & txn_ro_flat) == 0) {
    const MDBX_txn *scan = txn;
    do
      if ((scan->flags & MDBX_TXN_DIRTY) && (dbi == MAIN_DBI || (scan->dbi_state[dbi] & DBI_DIRTY))) {
        /* После коммита вложенных тразакций может быть mod_txnid > front */
        return scan->front_txnid;
      }
    while (unlikely((scan = scan->parent) != nullptr));
  }
  return /* tree->mod_txnid maybe zero in a legacy DB */ txn->dbs[dbi].mod_txnid ? txn->dbs[dbi].mod_txnid : txn->txnid;
}

int tbl_setup(const MDBX_env *env, volatile kvx_t *const kvx, const tree_t *const db) {
  osal_memory_fence(mo_AcquireRelease, false);

  if (unlikely(!check_table_flags(db->flags))) {
    ERROR("incompatible or invalid db.flags (0x%x) ", db->flags);
    return MDBX_INCOMPATIBLE;
  }

  size_t v_lmin = valsize_min(db->flags);
  size_t v_lmax = env_valsize_max(env, db->flags);
  if ((db->flags & (MDBX_DUPFIXED | MDBX_INTEGERDUP)) != 0 && db->dupfix_size) {
    if (!MDBX_DISABLE_VALIDATION && unlikely(db->dupfix_size < v_lmin || db->dupfix_size > v_lmax)) {
      ERROR("db.dupfix_size (%u) <> min/max value-length (%zu/%zu)", db->dupfix_size, v_lmin, v_lmax);
      return MDBX_CORRUPTED;
    }
    v_lmin = v_lmax = db->dupfix_size;
  }

  kvx->clc.k.lmin = keysize_min(db->flags);
  kvx->clc.k.lmax = env_keysize_max(env, db->flags);
  clc_reset_methods(&kvx->clc.k);
  clc_reset_methods(&kvx->clc.v);
  if (unlikely(!kvx->clc.k.cmp)) {
    kvx->clc.v.cmp = builtin_datacmp(db->flags);
    kvx->clc.k.cmp = builtin_keycmp(db->flags);
  }
  kvx->clc.v.lmin = v_lmin;
  osal_memory_fence(mo_Relaxed, true);
  kvx->clc.v.lmax = v_lmax;
  osal_memory_fence(mo_AcquireRelease, true);

  eASSERT0(env, kvx->clc.k.lmax >= kvx->clc.k.lmin);
  eASSERT0(env, kvx->clc.v.lmax >= kvx->clc.v.lmin);
  return MDBX_SUCCESS;
}

int tbl_fetch(MDBX_txn *txn, MDBX_cursor *mc, size_t dbi, const MDBX_val *name, unsigned wanna_flags) {
  int err = cursor_init(mc, txn, MAIN_DBI);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  err = tree_search(mc, name, 0);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (err == MDBX_NOTFOUND)
      goto notfound;
    return err;
  }

  sfr_t sr = tree_search_foliage(mc, name);
  if (unlikely(!sr.exact)) {
  notfound:
    if (dbi < txn->env->n_dbi && (txn->env->dbs_flags[dbi] & DB_VALID) && !(wanna_flags & MDBX_CREATE))
      NOTICE("dbi %zu refs to non-existing table `%.*s` for txn %" PRIaTXN " (err %d)", dbi, (int)name->iov_len,
             (const char *)name->iov_base, txn->txnid, err);
    return MDBX_NOTFOUND;
  }

  if (unlikely((node_flags(sr.node) & (N_DUP | N_TREE)) != N_TREE)) {
    NOTICE("dbi %zu refs to not a named table `%.*s` for txn %" PRIaTXN " (%s)", dbi, (int)name->iov_len,
           (const char *)name->iov_base, txn->txnid, "wrong node-flags");
    return MDBX_INCOMPATIBLE /* not a named DB */;
  }

  MDBX_val data;
  err = node_read(mc, sr.node, &data, mc->pg[mc->top]);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (unlikely(data.iov_len < sizeof(tree_t))) {
    NOTICE("dbi %zu refs to not a named table `%.*s` for txn %" PRIaTXN " (%s)", dbi, (int)name->iov_len,
           (const char *)name->iov_base, txn->txnid, "wrong record-size");
    return MDBX_INCOMPATIBLE /* not a named DB */;
  }

  const unsigned db_flags = UNALIGNED_PEEK_16(data.iov_base, tree_t, flags);
  const pgno_t db_root_pgno = peek_pgno(ptr_disp(data.iov_base, offsetof(tree_t, root)));
  /* The txn may not know this DBI, or another process may
   * have dropped and recreated the DB with other flags. */
  if (unlikely((wanna_flags ^ db_flags) & DB_PERSISTENT_FLAGS) && !(wanna_flags & MDBX_DB_ACCEDE) &&
      !((wanna_flags & MDBX_CREATE) && db_root_pgno == P_INVALID)) {
    NOTICE("dbi %zu refs to the re-created table `%.*s` for txn %" PRIaTXN
           " with different flags (present 0x%X != wanna 0x%X)",
           dbi, (int)name->iov_len, (const char *)name->iov_base, txn->txnid, db_flags & DB_PERSISTENT_FLAGS,
           wanna_flags & DB_PERSISTENT_FLAGS);
    return MDBX_INCOMPATIBLE /* not a named DB */;
  }

  tree_t *const db = &txn->dbs[dbi];
  memcpy(db, data.iov_base, sizeof(tree_t));
#if !MDBX_DISABLE_VALIDATION
  const txnid_t maindb_leafpage_txnid = mc->pg[mc->top]->txnid;
  cASSERT0(txn, txn->front_txnid >= maindb_leafpage_txnid);
  if (unlikely(db->mod_txnid > maindb_leafpage_txnid)) {
    ERROR("db.mod_txnid (%" PRIaTXN ") > page-txnid (%" PRIaTXN ")", db->mod_txnid, maindb_leafpage_txnid);
    return MDBX_CORRUPTED;
  }
#endif /* !MDBX_DISABLE_VALIDATION */

  return MDBX_SUCCESS;
}

int tbl_create(MDBX_txn *txn, MDBX_cursor *mc, size_t slot, const MDBX_val *name, unsigned db_flags) {
  cASSERT0(txn, db_flags & MDBX_CREATE);
  MDBX_val body;
  body.iov_base = memset(&txn->dbs[slot], 0, body.iov_len = sizeof(tree_t));
  txn->dbs[slot].root = P_INVALID;
  txn->dbs[slot].mod_txnid = txn->txnid;
  txn->dbs[slot].flags = db_flags & DB_PERSISTENT_FLAGS;
  mc->next = txn->cursors[MAIN_DBI];
  txn->cursors[MAIN_DBI] = mc;
  int err = cursor_put_checklen(mc, name, &body, N_TREE | MDBX_NOOVERWRITE);
  txn->cursors[MAIN_DBI] = mc->next;
  if (likely(err == MDBX_SUCCESS)) {
    txn->flags |= MDBX_TXN_DIRTY;
    cASSERT0(txn, (txn->dbi_state[MAIN_DBI] & DBI_DIRTY) != 0);
  }
  return err;
}

int tbl_refresh(MDBX_txn *txn, size_t dbi) {
  cursor_couple_t couple;
  kvx_t *const kvx = &txn->env->kvs[dbi];
  int rc = tbl_fetch(txn, &couple.outer, dbi, &kvx->name, txn->dbs[dbi].flags);
  if (likely(rc != MDBX_SUCCESS))
    return dbi_gone(txn, dbi, rc);

  rc = tbl_setup_ifneed(txn->env, kvx, &txn->dbs[dbi]);
  if (unlikely(rc != MDBX_SUCCESS))
    return dbi_gone(txn, dbi, rc);

  if (unlikely(dbi_changed(txn, dbi)))
    return MDBX_BAD_DBI;

  txn->dbi_state[dbi] &= ~DBI_STALE;
  return MDBX_SUCCESS;
}

int tbl_purge(MDBX_cursor *mc) {
  if (mc->tree->height) {
    int err = tree_drop(mc, cursor_is_main(mc) || (mc->tree->flags & MDBX_DUPSORT));
    if (unlikely(err != MDBX_SUCCESS)) {
      mc->txn->flags |= MDBX_TXN_ERROR;
      return err;
    }

    /* reset the DB record, mark it dirty */
    *mc->dbi_state |= DBI_DIRTY;
    mc->tree->height = 0;
    mc->tree->branch_pages = 0;
    mc->tree->leaf_pages = 0;
    mc->tree->large_pages = 0;
    mc->tree->items = 0;
    mc->tree->root = P_INVALID;
    mc->tree->sequence = 0;
    /* mc->tree->mod_txnid = txn->txnid; */
    mc->txn->flags |= MDBX_TXN_DIRTY;

    /* Invalidate the dropped DB's cursors */
    const size_t dbi = cursor_dbi(mc);
    for (mc = mc->txn->cursors[dbi]; mc; mc = mc->next)
      be_poor(mc);
  }

  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

static void tbl_stat_add(const tree_t *db, MDBX_stat *const st) {
  st->ms_depth += db->height;
  st->ms_branch_pages += db->branch_pages;
  st->ms_leaf_pages += db->leaf_pages;
  st->ms_overflow_pages += db->large_pages;
  st->ms_entries += db->items;
  st->ms_mod_txnid = (st->ms_mod_txnid > db->mod_txnid) ? st->ms_mod_txnid : db->mod_txnid;
}

int tbl_stat_summary(const MDBX_txn *txn, MDBX_stat *st) {
  memset(st, 0, sizeof(*st));

  const MDBX_env *const env = txn->env;
  st->ms_psize = env->ps;
  size_t done = 0;
  TXN_FOREACH_DBI_FROM(txn, dbi,
                       /* assuming GC is internal and not subject for accounting */ MAIN_DBI) {
    if ((txn->dbi_state[dbi] & (DBI_VALID | DBI_STALE)) == DBI_VALID) {
      done += 1;
      tbl_stat_add(txn->dbs + dbi, st);
    }
  }

  if ((txn->dbs[MAIN_DBI].flags & MDBX_DUPSORT) == 0 &&
      done < txn->dbs[MAIN_DBI].items /* TODO: use `md_subs` field */ + /* MainDB */ 1) {
    cursor_couple_t cx;
    int err = cursor_init(&cx.outer, (MDBX_txn *)txn, MAIN_DBI);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    /* scan and account not opened named tables */
    err = tree_search(&cx.outer, nullptr, Z_FIRST);
    while (err == MDBX_SUCCESS) {
      const page_t *mp = cx.outer.pg[cx.outer.top];
      for (size_t i = 0; i < page_numkeys(mp); i++) {
        const node_t *const node = page_node(mp, i);
        if (node_flags(node) != N_TREE)
          continue;
        if (unlikely(node_ds(node) != sizeof(tree_t))) {
          ERROR("%s/%d: %s %zu", "MDBX_CORRUPTED", MDBX_CORRUPTED, "invalid table node size", node_ds(node));
          return MDBX_CORRUPTED;
        }

        /* skip opened and already accounted */
        const MDBX_val name = {node_key(node), node_ks(node)};
        TXN_FOREACH_DBI_USER(txn, dbi)
        if ((txn->dbi_state[dbi] & (DBI_VALID | DBI_STALE)) == DBI_VALID &&
            env->kvs[MAIN_DBI].clc.k.cmp(&name, &env->kvs[dbi].name) == 0)
          goto next;

        tree_t db;
        memcpy(&db, node_data(node), sizeof(db));
        tbl_stat_add(&db, st);
      }
    next:
      err = cursor_sibling_right(&cx.outer);
    }
    if (unlikely(err != MDBX_NOTFOUND))
      return err;
  }

  return MDBX_SUCCESS;
}
