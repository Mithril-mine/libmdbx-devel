/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor-mut.c — cursor mutation operations: cursor_put() with the
/// dupsort-hive insertion logic, cursor_check_multiple()/cursor_put_checklen()
/// argument checks and cursor_del(); part of the cursor.c module (see also
/// cursor-setup.c / cursor-nav.c / cursor-seek.c and cursor-internal.h).

#include "internals.h"
#include "cursor-internal.h"

__hot int cursor_put(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags) {
  int err;
  DKBUF_DEBUG;
  MDBX_env *const env = mc->txn->env;
  if (LOG_ENABLED(MDBX_LOG_DEBUG) && (flags & MDBX_RESERVE))
    data->iov_base = nullptr;
  DEBUG("==> put db %d key [%s], size %" PRIuPTR ", data [%s] size %" PRIuPTR, cursor_dbi_dbg(mc), DKEY_DEBUG(key),
        key->iov_len, DVAL_DEBUG(data), data->iov_len);

  cASSERT0(mc, mc->stash < 1);
  if ((flags & MDBX_CURRENT) != 0 && (mc->flags & z_inner) == 0) {
    if (unlikely(flags & (MDBX_APPEND | MDBX_NOOVERWRITE)))
      return MDBX_EINVAL;
    /* Запрошено обновление текущей записи, на которой сейчас стоит курсор.
     * Проверяем что переданный ключ совпадает со значением в текущей позиции
     * курсора. Здесь проще вызвать cursor_ops(), так как для обслуживания
     * таблиц с MDBX_DUPSORT также требуется текущий размер данных. */
    MDBX_val current_key, current_data;
    err = cursor_ops(mc, &current_key, &current_data, MDBX_GET_CURRENT);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (mc->clc->k.cmp(key, &current_key) != 0)
      return MDBX_EKEYMISMATCH;

    if (unlikely((flags & MDBX_MULTIPLE))) {
      if (unlikely(!mc->subcur))
        return MDBX_EINVAL;
      err = cursor_del(mc, flags & MDBX_ALLDUPS);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      if (unlikely(data[1].iov_len == 0))
        return MDBX_SUCCESS;
      flags -= MDBX_CURRENT;
      goto skip_check_samedata;
    }

    if (mc->subcur) {
      node_t *node = page_node(mc->pg[mc->top], mc->ki[mc->top]);
      if (node_flags(node) & N_DUP) {
        cASSERT0(mc, inner_pointed(mc));
        /* Если за ключом более одного значения, либо если размер данных
         * отличается, то вместо обновления требуется удаление и
         * последующая вставка. */
        if (mc->subcur->nested_tree.items > 1 || current_data.iov_len != data->iov_len) {
          err = cursor_del(mc, flags & MDBX_ALLDUPS);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
          flags -= MDBX_CURRENT;
          goto skip_check_samedata;
        }
      } else if (unlikely(node_size(key, data) > env->leaf_nodemax)) {
        /* Уже есть пара key-value хранящаяся в обычном узле. Новые данные
         * слишком большие для размещения в обычном узле вместе с ключом, но
         * могут быть размещены в вложенном дереве. Удаляем узел со старыми
         * данными, чтобы при помещении новых создать вложенное дерево. */
        err = cursor_del(mc, 0);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
        flags -= MDBX_CURRENT;
        goto skip_check_samedata;
      }
    }
    if (!(flags & MDBX_RESERVE) && unlikely(eq_fast(&current_data, data)))
      return MDBX_SUCCESS /* the same data, nothing to update */;
  skip_check_samedata:;
  }

  size_t *batch_dupfix_done = nullptr, batch_dupfix_given = 0;
  if (unlikely(flags & MDBX_MULTIPLE)) {
    MPROBE_WATCH(cursor_multiple_put, 1);
    batch_dupfix_given = data[1].iov_len;
    if (unlikely(data[1].iov_len == 0))
      return /* nothing todo */ MDBX_SUCCESS;
    batch_dupfix_done = &data[1].iov_len;
    *batch_dupfix_done = 0;
  }

  int rc = MDBX_SUCCESS;
  if (mc->tree->height == 0) {
    /* new database, cursor has nothing to point to */
    cASSERT0(mc, is_poor(mc));
    rc = MDBX_NO_ROOT;
  } else if ((flags & MDBX_CURRENT) == 0) {
    bool exact = false;
    MDBX_val old_data;
    if ((flags & MDBX_APPEND) && mc->tree->items > 0) {
      MDBX_val last_key;
      old_data.iov_base = nullptr;
      old_data.iov_len = 0;
      /* Issue #66: a persistent cursor parked at the rightmost leaf after the
       * previous append already holds the whole right edge of the tree in its
       * stack, so the last key is read from that leaf directly instead of
       * re-descending from the root via outer_last()/inner_last(). The extra
       * descent was pure overhead: one full tree_search per appended key. */
      bool fast_end = false;
      if (unlikely(is_pointed(mc) && !(mc->flags & (z_inner | z_hollow)))) {
        intptr_t i;
        for (i = 0; i < mc->top; i++)
          if ((size_t)mc->ki[i] + 1 < page_numkeys(mc->pg[i]))
            break;
        if (i == mc->top) {
          const size_t nkeys = page_numkeys(mc->pg[mc->top]);
          cASSERT0(mc, nkeys > 0);
          const node_t *const last = page_node(mc->pg[mc->top], nkeys - 1);
          last_key = get_key(last);
          const int cmp = mc->clc->k.cmp(key, &last_key);
          if (likely(cmp > 0)) {
            mc->ki[mc->top] = (indx_t)page_numkeys(mc->pg[mc->top]); /* step forward for appending */
            rc = MDBX_NOTFOUND;
            inner_gone(mc);
            goto append_done;
          }
          /* exact or smaller: the generic path must see the true last value */
          fast_end = false;
          old_data.iov_base = nullptr;
          old_data.iov_len = 0;
        }
      }
      if (!fast_end) {
        rc = (mc->flags & z_inner) ? inner_last(mc, &last_key) : outer_last(mc, &last_key, &old_data);
      }
      if (likely(rc == MDBX_SUCCESS)) {
        const int cmp = mc->clc->k.cmp(key, &last_key);
        if (likely(cmp > 0)) {
          mc->ki[mc->top]++; /* step forward for appending */
          rc = MDBX_NOTFOUND;
          inner_gone(mc);
        } else if (unlikely(cmp != 0)) {
          /* new-key < last-key */
          return MDBX_EKEYMISMATCH;
        } else {
          rc = MDBX_SUCCESS;
          exact = true;
        }
      }
    append_done:;
    } else {
      /* The `const` qualifier is cast away when passing `key` to `cursor_seek`, which accepts a non-const `MDBX_val *`.
       * This is safe in practice for `MDBX_SET` since `cursor_seek` only writes `*key` back for ops `>= MDBX_SET_KEY`.
       * Of cause, casting away `const` is fragile solution, but we accept this risk and postpone the fix until a deep
       * refactoring of related functions. */
      csr_t csr = cursor_seek(mc, (MDBX_val *)key, &old_data, MDBX_SET);
      rc = csr.err;
      exact = csr.exact;
    }
    if (exact) {
      cASSERT0(mc, rc == MDBX_SUCCESS);
      if (unlikely(flags & MDBX_NOOVERWRITE)) {
        DEBUG("duplicate key [%s]", DKEY_DEBUG(key));
        *data = old_data;
        return MDBX_KEYEXIST;
      }
      if (unlikely(mc->flags & z_inner)) {
        /* nested subtree of DUPSORT-database with the same key, nothing to update */
        return (flags & MDBX_NODUPDATA) ? MDBX_KEYEXIST : MDBX_SUCCESS;
      }
      if (inner_pointed(mc)) {
        if (unlikely(flags & MDBX_ALLDUPS)) {
          rc = cursor_del(mc, MDBX_ALLDUPS);
          if (unlikely(rc != MDBX_SUCCESS))
            return rc;
          flags -= MDBX_ALLDUPS;
          cASSERT0(mc, mc->top + 1 == mc->tree->height);
          rc = (mc->top >= 0) ? MDBX_NOTFOUND : MDBX_NO_ROOT;
        } else if ((flags & (MDBX_RESERVE | MDBX_MULTIPLE)) == 0) {
          old_data = *data;
          csr_t csr = cursor_seek(&mc->subcur->cursor, &old_data, nullptr, MDBX_SET_RANGE);
          if (unlikely(csr.exact)) {
            cASSERT0(mc, csr.err == MDBX_SUCCESS);
            if (flags & MDBX_NODUPDATA)
              return MDBX_KEYEXIST;
            if (flags & MDBX_APPENDDUP)
              return MDBX_EKEYMISMATCH;
            /* the same data, nothing to update */
            return MDBX_SUCCESS;
          } else if (csr.err != MDBX_SUCCESS && unlikely(csr.err != MDBX_NOTFOUND)) {
            be_poor(mc);
            return csr.err;
          }
        }
      } else if (!(flags & (MDBX_RESERVE | MDBX_MULTIPLE))) {
        if (unlikely(eq_fast(data, &old_data))) {
          cASSERT1(mc, mc->clc->v.cmp(data, &old_data) == 0);
          /* the same data, nothing to update */
          return (mc->subcur && (flags & MDBX_NODUPDATA)) ? MDBX_KEYEXIST : MDBX_SUCCESS;
        }
        cASSERT1(mc, mc->clc->v.cmp(data, &old_data) != 0);
      }
    } else if (unlikely(rc != MDBX_NOTFOUND))
      return rc;
  }

  mc->flags &= ~z_after_delete;
  MDBX_val xdata, *ref_data = data;

  /* Cursor is positioned, check for room in the dirty list */
  err = cursor_touch(mc, key, ref_data);
  if (unlikely(err))
    return err;

  if (unlikely(rc == MDBX_NO_ROOT)) {
    /* new database, write a root leaf page */
    DEBUG("%s", "allocating new root leaf page");
    pgr_t npr = page_new(mc, P_LEAF);
    if (unlikely(npr.err != MDBX_SUCCESS))
      return npr.err;
    npr.err = cursor_push(mc, npr.page, 0);
    if (unlikely(npr.err != MDBX_SUCCESS))
      return npr.err;
    mc->tree->root = npr.page->pgno;
    mc->tree->height++;
    if (mc->tree->flags & MDBX_INTEGERKEY) {
      ASSERT(key->iov_len >= mc->clc->k.lmin && key->iov_len <= mc->clc->k.lmax);
      mc->clc->k.lmin = mc->clc->k.lmax = key->iov_len;
    }
    if (mc->tree->flags & (MDBX_INTEGERDUP | MDBX_DUPFIXED)) {
      ASSERT(data->iov_len >= mc->clc->v.lmin && data->iov_len <= mc->clc->v.lmax);
      ASSERT(mc->subcur != nullptr);
      mc->tree->dupfix_size = /* mc->subcur->nested_tree.dupfix_size = */
          (unsigned)(mc->clc->v.lmin = mc->clc->v.lmax = data->iov_len);
      cASSERT0(mc, mc->clc->v.lmin == mc->subcur->cursor.clc->k.lmin);
      cASSERT0(mc, mc->clc->v.lmax == mc->subcur->cursor.clc->k.lmax);
      if (mc->flags & z_inner)
        npr.page->flags |= P_DUPFIX;
    }
  }

  MDBX_val old_singledup, old_data;
  tree_t nested_dupdb;
  page_t *sub_root = nullptr;
  bool insert_key, insert_data;
  uint16_t fp_flags = P_LEAF;
  page_t *fp = env->page_auxbuf;
  fp->txnid = mc->txn->front_txnid;
  insert_key = insert_data = (rc != MDBX_SUCCESS);
  old_singledup.iov_base = nullptr;
  old_singledup.iov_len = 0;
  if (insert_key) {
    /* The key does not exist */
    DEBUG("inserting key at index %i", mc->ki[mc->top]);
    if (mc->tree->flags & MDBX_DUPSORT) {
      inner_gone(mc);
      if (node_size(key, data) > env->leaf_nodemax) {
        /* Too big for a node, insert in sub-DB.  Set up an empty
         * "old sub-page" for convert_to_subtree to expand to a full page. */
        fp->dupfix_ksize = (mc->tree->flags & MDBX_DUPFIXED) ? (uint16_t)data->iov_len : 0;
        fp->lower = fp->upper = 0;
        old_data.iov_len = PAGEHDRSZ;
        goto convert_to_subtree;
      }
    }
  } else {
    /* there's only a key anyway, so this is a no-op */
    if (is_dupfix_leaf(mc->pg[mc->top])) {
      size_t ksize = mc->tree->dupfix_size;
      if (unlikely(key->iov_len != ksize))
        return MDBX_BAD_VALSIZE;
      void *ptr = page_dupfix_ptr(mc->pg[mc->top], mc->ki[mc->top], ksize);
      memcpy(ptr, key->iov_base, ksize);
    fix_parent:
      /* if overwriting slot 0 of leaf, need to update branch key if there is a parent page */
      if (mc->top && !mc->ki[mc->top]) {
        int shift = 0;
        do {
          cursor_enroot(mc, 1);
          ++shift;
        } while (mc->top && !mc->ki[mc->top]);

        err = MDBX_SUCCESS;
        if (mc->ki[mc->top]) {
          err = tree_propagate_key(mc, key);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }

        if (likely(mc->stash >= shift)) {
          if (unlikely(mc->top + mc->stash + shift >= CURSOR_STACK_SIZE)) {
            be_poor(mc);
            return MDBX_CURSOR_FULL;
          }
          cursor_undo_enroot(mc, shift);
        }
      }

      cASSERT0(mc, mc->stash == 0);
      PROBE_AGAINST_DANGLING_DBI(mc);
      if (CHECKS2_ENABLED()) {
        err = cursor_validate(mc);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      return MDBX_SUCCESS;
    }

  more:
    cASSERT0(mc, mc->stash == 0);
    PROBE_AGAINST_DANGLING_DBI(mc);
    if (CHECKS2_ENABLED()) {
      err = cursor_validate(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    node_t *const node = page_node(mc->pg[mc->top], mc->ki[mc->top]);

    /* Large/Overflow page overwrites need special handling */
    if (unlikely(node_flags(node) & N_BIG)) {
      const size_t dpages = (node_size(key, data) > env->leaf_nodemax) ? largechunk_npages(env, data->iov_len) : 0;

      const pgno_t pgno = node_largedata_pgno(node);
      pgr_t lp = page_get_large(mc, pgno, mc->pg[mc->top]->txnid);
      if (unlikely(lp.err != MDBX_SUCCESS))
        return lp.err;
      cASSERT0(mc, page_type(lp.page) == P_LARGE);

      /* Is the ov page from this txn (or a parent) and big enough? */
      const size_t ovpages = lp.page->pages;
      const size_t extra_threshold =
          (mc->tree == &mc->txn->dbs[FREE_DBI]) ? 1 : /* LY: add configurable threshold to keep reserve space */ 0;
      if (!is_frozen(mc->txn, lp.page) && ovpages >= dpages && ovpages <= dpages + extra_threshold) {
        /* yes, overwrite it. */
        if (!is_modifiable(mc->txn, lp.page)) {
          if (is_spilled(mc->txn, lp.page)) {
            lp = /* TODO: avoid search and get txn & spill-index from page_result */
                page_unspill(mc->txn, lp.page);
            if (unlikely(lp.err))
              return lp.err;
          } else {
            if (unlikely(!mc->txn->parent)) {
              ERROR("Unexpected not frozen/modifiable/spilled but shadowed %s "
                    "page %" PRIaPGNO " mod-txnid %" PRIaTXN ","
                    " without parent transaction, current txn %" PRIaTXN " front %" PRIaTXN,
                    "large/overflow", pgno, lp.page->txnid, mc->txn->txnid, mc->txn->front_txnid);
              return MDBX_PROBLEM;
            }

            /* It is writable only in a parent txn */
            page_t *np = page_shadow_alloc(mc->txn, ovpages);
            if (unlikely(!np))
              return MDBX_ENOMEM;

            memcpy(np, lp.page, PAGEHDRSZ); /* Copy header of page */
            err = page_dirty(mc->txn, lp.page = np, ovpages);
            if (unlikely(err != MDBX_SUCCESS))
              return err;
            if (MDBX_ENABLE_PGOP_STAT)
              mc->txn->env->lck->pgops.clone.weak += ovpages;
            cASSERT1(mc, txn_dpl_check(mc->txn));
          }
        }
        node_set_ds(node, data->iov_len);
        if (flags & MDBX_RESERVE)
          data->iov_base = page2payload(lp.page);
        else
          memcpy(page2payload(lp.page), data->iov_base, data->iov_len);

        cASSERT0(mc, mc->stash == 0);
        PROBE_AGAINST_DANGLING_DBI(mc);
        if (CHECKS2_ENABLED()) {
          err = cursor_validate(mc);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }
        return MDBX_SUCCESS;
      }

      cASSERT0(mc, mc->stash == 0);
      PROBE_AGAINST_DANGLING_DBI(mc);
      if ((err = page_retire(mc, lp.page)) != MDBX_SUCCESS)
        return err;
    } else {
      old_data.iov_len = node_ds(node);
      old_data.iov_base = node_data(node);
      cASSERT0(mc, ptr_disp(old_data.iov_base, old_data.iov_len) <= ptr_disp(mc->pg[mc->top], env->ps));

      /* DB has dups? */
      if (mc->tree->flags & MDBX_DUPSORT) {
        /* Prepare (sub-)page/sub-DB to accept the new item, if needed.
         * fp: old sub-page or a header faking it.
         * mp: new (sub-)page.
         * xdata: node data with new sub-page or sub-DB. */
        size_t growth = 0; /* growth in page size.*/
        page_t *mp = fp = xdata.iov_base = env->page_auxbuf;
        mp->pgno = mc->pg[mc->top]->pgno;

        /* Was a single item before, must convert now */
        if (!(node_flags(node) & N_DUP)) {
          /* does data match? */
          if (flags & MDBX_APPENDDUP) {
            const int cmp = mc->clc->v.cmp(data, &old_data);
            cASSERT1(mc, cmp != 0 || eq_fast(data, &old_data));
            if (unlikely(cmp <= 0))
              return MDBX_EKEYMISMATCH;
          } else if (unlikely(eq_fast(data, &old_data))) {
            cASSERT1(mc, mc->clc->v.cmp(data, &old_data) == 0);
            cASSERT0(mc, !"Should not happen since equal data should have been handled by batch dupfix" ||
                             batch_dupfix_done);
            if (flags & MDBX_NODUPDATA)
              return MDBX_KEYEXIST;
            /* data is match exactly byte-to-byte, nothing to update */
            rc = MDBX_SUCCESS;
            if (unlikely(batch_dupfix_done))
              goto batch_dupfix_continue;
            cASSERT0(mc, mc->stash == 0);
            return rc;
          }

          /* Just overwrite the current item */
          if (flags & MDBX_CURRENT) {
            cASSERT0(mc, node_size(key, data) <= env->leaf_nodemax);
            goto current;
          }

          /* Back up original data item */
          memcpy(old_singledup.iov_base = fp + 1, old_data.iov_base, old_singledup.iov_len = old_data.iov_len);

          /* Make sub-page header for the dup items, with dummy body */
          fp->flags = P_LEAF | P_SUBP;
          fp->lower = 0;
          xdata.iov_len = PAGEHDRSZ + old_data.iov_len + data->iov_len;
          if (mc->tree->flags & MDBX_DUPFIXED) {
            fp->flags |= P_DUPFIX;
            fp->dupfix_ksize = (uint16_t)data->iov_len;
            /* Будем создавать DUPFIX-страницу, как минимум с двумя элементами.
             * При коротких значениях и наличии свободного места можно сделать
             * некоторое резервирование места, чтобы при последующих добавлениях
             * не сразу расширять созданную под-страницу.
             * Резервирование в целом сомнительно (см ниже), но может сработать
             * в плюс (а если в минус то несущественный) при коротких ключах. */
            xdata.iov_len +=
                page_subleaf2_reserve(env, page_room(mc->pg[mc->top]) + old_data.iov_len, xdata.iov_len, data->iov_len);
            cASSERT0(mc, (xdata.iov_len & 1) == 0);
          } else {
            xdata.iov_len += 2 * (sizeof(indx_t) + NODESIZE) + (old_data.iov_len & 1) + (data->iov_len & 1);
          }
          cASSERT0(mc, (xdata.iov_len & 1) == 0);
          fp->upper = (uint16_t)(xdata.iov_len - PAGEHDRSZ);
          old_data.iov_len = xdata.iov_len; /* pretend olddata is fp */
        } else if (node_flags(node) & N_TREE) {
          /* Data is on sub-DB, just store it */
          flags |= N_DUP | N_TREE;
          goto dupsort_put;
        } else {
          /* Data is on sub-page */
          fp = old_data.iov_base;
          if (!MDBX_DISABLE_VALIDATION && unlikely(!subpage_geometry_ok(fp, old_data.iov_len))) {
            /* The nested upper/lower are crafted: the copy at
             * "fp->upper + PAGEHDRSZ" below would underflow its memcpy/memmove
             * length. (issue #176) */
            return bad_page(fp, "invalid nested %s-page lower(%u)/upper(%u) for size %zu\n",
                            is_dupfix_leaf(fp) ? "leaf2" : "leaf", fp->lower, fp->upper, old_data.iov_len - PAGEHDRSZ);
          }
          switch (flags) {
          default:
            growth = is_dupfix_leaf(fp) ? fp->dupfix_ksize : (node_size(data, nullptr) + sizeof(indx_t));
            if (page_room(fp) >= growth) {
              /* На текущей под-странице есть место для добавления элемента.
               * Оптимальнее продолжить использовать эту страницу, ибо
               * добавление вложенного дерева увеличит WAF на одну страницу. */
              goto continue_subpage;
            }
            /* На текущей под-странице нет места для еще одного элемента.
             * Можно либо увеличить эту под-страницу, либо вынести куст
             * значений во вложенное дерево.
             *
             * Продолжать использовать текущую под-страницу возможно
             * только пока и если размер после добавления элемента будет
             * меньше leaf_nodemax. Соответственно, при превышении
             * просто сразу переходим на вложенное дерево. */
            xdata.iov_len = old_data.iov_len + (growth += growth & 1);
            if (xdata.iov_len > env->subpage_limit)
              goto convert_to_subtree;

            /* Можно либо увеличить под-страницу, в том числе с некоторым
             * запасом, либо перейти на вложенное поддерево.
             *
             * Резервирование места на под-странице представляется сомнительным:
             *  - Резервирование увеличит рыхлость страниц, в том числе
             *    вероятность разделения основной/гнездовой страницы;
             *  - Сложно предсказать полезный размер резервирования,
             *    особенно для не-MDBX_DUPFIXED;
             *  - Наличие резерва позволяет съекономить только на перемещении
             *    части элементов основной/гнездовой страницы при последующих
             *    добавлениях в нее элементов. Причем после первого изменения
             *    размера под-страницы, её тело будет примыкать
             *    к неиспользуемому месту на основной/гнездовой странице,
             *    поэтому последующие последовательные добавления потребуют
             *    только передвижения в entries[].
             *
             * Соответственно, более важным/определяющим представляется
             * своевременный переход к вложеному дереву, но тут достаточно
             * сложный конфликт интересов:
             *  - При склонности к переходу к вложенным деревьям, суммарно
             *    в БД будет большее кол-во более рыхлых страниц. Это увеличит
             *    WAF, а также RAF при последовательных чтениях большой БД.
             *    Однако, при коротких ключах и большом кол-ве
             *    дубликатов/мультизначений, плотность ключей в листовых
             *    страницах основного дерева будет выше. Соответственно, будет
             *    пропорционально меньше branch-страниц. Поэтому будет выше
             *    вероятность оседания/не-вымывания страниц основного дерева из
             *    LRU-кэша, а также попадания в write-back кэш при записи.
             *  - Наоборот, при склонности к использованию под-страниц, будут
             *    наблюдаться обратные эффекты. Плюс некоторые накладные расходы
             *    на лишнее копирование данных под-страниц в сценариях
             *    нескольких обновлений дубликатов одного куста в одной
             *    транзакции.
             *
             * Суммарно наиболее рациональным представляется такая тактика:
             *  - Вводим три порога subpage_limit, subpage_room_threshold
             *    и subpage_reserve_prereq, которые могут быть
             *    заданы/скорректированы пользователем в ‰ от leaf_nodemax;
             *  - Используем под-страницу пока её размер меньше subpage_limit
             *    и на основной/гнездовой странице не-менее
             *    subpage_room_threshold свободного места;
             *  - Резервируем место только для 1-3 коротких dupfix-элементов,
             *    расширяя размер под-страницы на размер кэш-линии ЦПУ, но
             *    только если на странице не менее subpage_reserve_prereq
             *    свободного места.
             *  - По-умолчанию устанавливаем:
             *     subpage_limit = leaf_nodemax (1000‰);
             *     subpage_room_threshold = 0;
             *     subpage_reserve_prereq = leaf_nodemax (1000‰).
             */
            if (is_dupfix_leaf(fp))
              growth += page_subleaf2_reserve(env, page_room(mc->pg[mc->top]) + old_data.iov_len, xdata.iov_len,
                                              data->iov_len);
            else {
              /* TODO: Если добавить возможность для пользователя задавать
               * min/max размеров ключей/данных, то здесь разумно реализовать
               * тактику резервирования подобную dupfixed. */
            }
            break;

          case MDBX_CURRENT | MDBX_NODUPDATA:
          case MDBX_CURRENT:
          continue_subpage:
            fp->txnid = mc->txn->front_txnid;
            fp->pgno = mp->pgno;
            mc->subcur->cursor.pg[0] = fp;
            flags |= N_DUP;
            goto dupsort_put;
          }
          xdata.iov_len = old_data.iov_len + growth;
          cASSERT0(mc, (xdata.iov_len & 1) == 0);
        }

        fp_flags = fp->flags;
        if (xdata.iov_len > env->subpage_limit || node_size_len(node_ks(node), xdata.iov_len) > env->leaf_nodemax ||
            (env->subpage_room_threshold &&
             page_room(mc->pg[mc->top]) + node_size_len(node_ks(node), old_data.iov_len) <
                 env->subpage_room_threshold + node_size_len(node_ks(node), xdata.iov_len))) {
          /* Too big for a sub-page, convert to sub-DB */
        convert_to_subtree:
          fp_flags &= ~P_SUBP;
          nested_dupdb.dupfix_size = 0;
          nested_dupdb.flags = flags_db2sub(mc->tree->flags);
          if (mc->tree->flags & MDBX_DUPFIXED) {
            fp_flags |= P_DUPFIX;
            nested_dupdb.dupfix_size = fp->dupfix_ksize;
          }
          nested_dupdb.height = 1;
          nested_dupdb.branch_pages = 0;
          nested_dupdb.leaf_pages = 1;
          nested_dupdb.large_pages = 0;
          nested_dupdb.items = page_numkeys(fp);
          xdata.iov_len = sizeof(nested_dupdb);
          xdata.iov_base = &nested_dupdb;
          const pgr_t par = gc_alloc_single(mc);
          mp = par.page;
          if (unlikely(par.err != MDBX_SUCCESS))
            return par.err;
          mc->tree->leaf_pages += 1;
          cASSERT0(mc, env->ps > old_data.iov_len);
          growth = env->ps - (unsigned)old_data.iov_len;
          cASSERT0(mc, (growth & 1) == 0);
          flags |= N_DUP | N_TREE;
          nested_dupdb.root = mp->pgno;
          nested_dupdb.sequence = 0;
          nested_dupdb.mod_txnid = mc->txn->txnid;
          sub_root = mp;
          DEBUG("convert_to_subtree: new-root %p.%u flags 0x%x", __Wpedantic_format_voidptr(mp), mp->pgno, fp_flags);
        }
        if (mp != fp) {
          mp->flags = fp_flags;
          mp->txnid = mc->txn->front_txnid;
          mp->dupfix_ksize = fp->dupfix_ksize;
          mp->lower = fp->lower;
          cASSERT0(mc, fp->upper + growth < UINT16_MAX);
          mp->upper = fp->upper + (indx_t)growth;
          if (unlikely(fp_flags & P_DUPFIX)) {
            memcpy(page2payload(mp), page2payload(fp), page_numkeys(fp) * fp->dupfix_ksize);
            cASSERT0(mc, (((mp->dupfix_ksize & page_numkeys(mp)) ^ mp->upper) & 1) == 0);
          } else {
            cASSERT0(mc, (mp->upper & 1) == 0);
            memcpy(ptr_disp(mp, mp->upper + PAGEHDRSZ), ptr_disp(fp, fp->upper + PAGEHDRSZ),
                   old_data.iov_len - fp->upper - PAGEHDRSZ);
            memcpy(mp->entries, fp->entries, page_numkeys(fp) * sizeof(mp->entries[0]));
            for (size_t i = 0; i < page_numkeys(fp); i++) {
              cASSERT0(mc, mp->entries[i] + growth <= UINT16_MAX);
              mp->entries[i] += (indx_t)growth;
            }
          }
        }

        if (!insert_key)
          node_del(mc, 0);
        ref_data = &xdata;
        flags |= N_DUP;
        goto insert_node;
      }

      /* MDBX passes N_TREE in 'flags' to write a DB record */
      if (unlikely((node_flags(node) ^ flags) & N_TREE))
        return MDBX_INCOMPATIBLE;

    current:
      if (data->iov_len == old_data.iov_len) {
        cASSERT0(mc, EVEN_CEIL(key->iov_len) == EVEN_CEIL(node_ks(node)));
        /* same size, just replace it. Note that we could
         * also reuse this node if the new data is smaller,
         * but instead we opt to shrink the node in that case. */
        if (flags & MDBX_RESERVE)
          data->iov_base = old_data.iov_base;
        else if (!(mc->flags & z_inner))
          memcpy(old_data.iov_base, data->iov_base, data->iov_len);
        else {
          cASSERT0(mc, page_numkeys(mc->pg[mc->top]) == 1);
          cASSERT0(mc, page_type_compat(mc->pg[mc->top]) == P_LEAF);
          cASSERT0(mc, node_ds(node) == 0);
          cASSERT0(mc, node_flags(node) == 0);
          cASSERT0(mc, key->iov_len < UINT16_MAX);
          node_set_ks(node, key->iov_len);
          memcpy(node_key(node), key->iov_base, key->iov_len);
          cASSERT0(mc, ptr_disp(node_key(node), node_ds(node)) < ptr_disp(mc->pg[mc->top], env->ps));
          goto fix_parent;
        }

        cASSERT0(mc, mc->stash == 0);
        if (CHECKS2_ENABLED()) {
          err = cursor_validate(mc);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }
        PROBE_AGAINST_DANGLING_DBI(mc);
        return MDBX_SUCCESS;
      }
    }
    node_del(mc, 0);
  }

  ref_data = data;

insert_node:;
  const unsigned naf = flags & NODE_ADD_FLAGS;
  size_t nsize = is_dupfix_leaf(mc->pg[mc->top]) ? key->iov_len : leaf_size(env, key, ref_data);
  if (page_room(mc->pg[mc->top]) < nsize) {
    rc = page_split(mc, key, ref_data, P_INVALID, insert_key ? naf : naf | MDBX_SPLIT_REPLACE);
    if (rc == MDBX_SUCCESS && CHECKS2_ENABLED())
      rc = insert_key ? cursor_validate(mc) : cursor_validate_updating(mc);
    cASSERT0(mc, rc || mc->stash == 0);
  } else {
    /* There is room already in this leaf page. */
    if (is_dupfix_leaf(mc->pg[mc->top])) {
      cASSERT0(mc, !(naf & (N_BIG | N_TREE | N_DUP)) && ref_data->iov_len == 0);
      rc = node_add_dupfix(mc, mc->ki[mc->top], key);
    } else
      rc = node_add_leaf(mc, mc->ki[mc->top], key, ref_data, naf);
    if (likely(rc == MDBX_SUCCESS)) {
      /* Adjust other cursors pointing to mp */
      page_t *const mp = mc->pg[mc->top];
      const size_t dbi = cursor_dbi(mc);
      for (MDBX_cursor *m2 = mc->txn->cursors[dbi]; m2; m2 = m2->next) {
        MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
        if (!is_related(mc, m3) || m3->pg[mc->top] != mp)
          continue;
        if (m3->ki[mc->top] >= mc->ki[mc->top])
          m3->ki[mc->top] += insert_key;
        if (inner_pointed(m3))
          cursor_inner_refresh(m3, mp, m3->ki[mc->top]);
      }
      cASSERT0(mc, mc->stash == 0);
    }
  }

  if (likely(rc == MDBX_SUCCESS)) {
    /* Now store the actual data in the child DB. Note that we're
     * storing the user data in the keys field, so there are strict
     * size limits on dupdata. The actual data fields of the child
     * DB are all zero size. */
    if (flags & N_DUP) {
      MDBX_val empty;
    dupsort_put:
      empty.iov_len = 0;
      empty.iov_base = nullptr;
      node_t *node = page_node(mc->pg[mc->top], mc->ki[mc->top]);
#define SHIFT_MDBX_NODUPDATA_TO_MDBX_NOOVERWRITE 1
      STATIC_ASSERT((MDBX_NODUPDATA >> SHIFT_MDBX_NODUPDATA_TO_MDBX_NOOVERWRITE) == MDBX_NOOVERWRITE);
      unsigned inner_flags = MDBX_CURRENT | ((flags & MDBX_NODUPDATA) >> SHIFT_MDBX_NODUPDATA_TO_MDBX_NOOVERWRITE);
      if ((flags & MDBX_CURRENT) == 0) {
        inner_flags -= MDBX_CURRENT;
        rc = cursor_dupsort_setup(mc, node, mc->pg[mc->top]);
        if (unlikely(rc != MDBX_SUCCESS))
          goto dupsort_error;
      }
      subcur_t *const mx = mc->subcur;
      if (sub_root) {
        cASSERT0(mc, mx->nested_tree.height == 1 && mx->nested_tree.root == sub_root->pgno);
        mx->cursor.combo_state = z_inner;
        mx->cursor.pg[0] = sub_root;
        mx->cursor.ki[0] = 0;
      }
      if (old_singledup.iov_base) {
        /* converted, write the original data first */
        if (is_dupfix_leaf(mx->cursor.pg[0]))
          rc = node_add_dupfix(&mx->cursor, 0, &old_singledup);
        else
          rc = node_add_leaf(&mx->cursor, 0, &old_singledup, &empty, 0);
        if (unlikely(rc != MDBX_SUCCESS))
          goto dupsort_error;
        mx->cursor.tree->items = 1;
      }
      if (!(node_flags(node) & N_TREE) || sub_root) {
        page_t *const mp = mc->pg[mc->top];
        const intptr_t nkeys = page_numkeys(mp);
        const size_t dbi = cursor_dbi(mc);

        for (MDBX_cursor *m2 = mc->txn->cursors[dbi]; m2; m2 = m2->next) {
          if (!is_related(mc, m2) || m2->pg[mc->top] != mp)
            continue;
          if (m2->ki[mc->top] == mc->ki[mc->top]) {
            cursor_couple_t *cx = container_of(m2, cursor_couple_t, outer);
            cASSERT0(m2, cx->inner.cursor.clc == mx->cursor.clc);
            cx->inner.nested_tree = mx->nested_tree;
            cx->inner.cursor.pg[0] = mx->cursor.pg[0];
            if (old_singledup.iov_base) {
              cx->inner.cursor.combo_state = z_inner;
              cx->inner.cursor.ki[0] = 0;
            }
            DEBUG("Sub-dbi -%zu root page %" PRIaPGNO, cursor_dbi(m2), cx->inner.nested_tree.root);
          } else if (!insert_key && m2->ki[mc->top] < nkeys)
            cursor_inner_refresh(m2, mp, m2->ki[mc->top]);
        }
      }
      cASSERT0(mc, mc->subcur->nested_tree.items < PTRDIFF_MAX);
      const size_t probe = (size_t)mc->subcur->nested_tree.items;
#define SHIFT_MDBX_APPENDDUP_TO_MDBX_APPEND 1
      PROBE_AGAINST_DANGLING_DBI(mc);
      STATIC_ASSERT((MDBX_APPENDDUP >> SHIFT_MDBX_APPENDDUP_TO_MDBX_APPEND) == MDBX_APPEND);
      inner_flags |= (flags & MDBX_APPENDDUP) >> SHIFT_MDBX_APPENDDUP_TO_MDBX_APPEND;
      rc = cursor_put(&mc->subcur->cursor, data, &empty, inner_flags);
      if (flags & N_TREE) {
        void *db = node_data(node);
        mc->subcur->nested_tree.mod_txnid = mc->txn->txnid;
        memcpy(db, &mc->subcur->nested_tree, sizeof(tree_t));
      }
      insert_data = (probe != (size_t)mc->subcur->nested_tree.items);
    }
    /* Increment count unless we just replaced an existing item. */
    if (insert_data)
      mc->tree->items++;
    if (insert_key) {
      if (unlikely(rc != MDBX_SUCCESS))
        goto dupsort_error;
      /* If we succeeded and the key didn't exist before,
       * make sure the cursor is marked valid. */
      be_filled(mc);
    }
    if (likely(rc == MDBX_SUCCESS)) {
      cASSERT0(mc, mc->stash == 0);
      PROBE_AGAINST_DANGLING_DBI(mc);
      cASSERT0(mc, is_filled(mc));
      if (unlikely(batch_dupfix_done)) {
      batch_dupfix_continue:
        /* let caller know how many succeeded, if any */
        if ((*batch_dupfix_done += 1) < batch_dupfix_given) {
          data[0].iov_base = ptr_disp(data[0].iov_base, data[0].iov_len);
          insert_key = insert_data = false;
          old_singledup.iov_base = nullptr;
          sub_root = nullptr;
          goto more;
        }
      }
      if (CHECKS2_ENABLED())
        rc = cursor_validate(mc);
    }
    return rc;

  dupsort_error:
    if (unlikely(rc == MDBX_KEYEXIST)) {
      /* should not happen, we deleted that item */
      ERROR("Unexpected %i error while put to nested dupsort's hive", rc);
      rc = MDBX_PROBLEM;
    }
  }
  mc->txn->flags |= MDBX_TXN_ERROR;
  return rc;
}

int cursor_check_multiple(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags) {
  (void)key;
  if (unlikely(flags & MDBX_RESERVE))
    return MDBX_EINVAL;
  if (unlikely(!(mc->tree->flags & MDBX_DUPFIXED)))
    return MDBX_INCOMPATIBLE;
  if (unlikely(data->iov_len == 0))
    return MDBX_BAD_VALSIZE; /* MDBX_DUPFIXED element size must be non-zero */
  const size_t number = data[1].iov_len;
  if (unlikely(number > MAX_MAPSIZE / 2 / (BRANCH_NODE_MAX(MDBX_MAX_PAGESIZE) - NODESIZE))) {
    /* checking for multiplication overflow */
    if (unlikely(number > MAX_MAPSIZE / 2 / data->iov_len))
      return MDBX_TOO_LARGE;
  }
  return MDBX_SUCCESS;
}

__hot int cursor_put_checklen(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags) {
  cASSERT0(mc, (mc->flags & z_inner) == 0);
  if (unlikely(key->iov_len > mc->clc->k.lmax || key->iov_len < mc->clc->k.lmin))
    return MDBX_BAD_VALSIZE;
  if (unlikely(data->iov_len > mc->clc->v.lmax || data->iov_len < mc->clc->v.lmin))
    return MDBX_BAD_VALSIZE;

  uint64_t aligned_keybytes, aligned_databytes;
  MDBX_val aligned_key, aligned_data;
  if (mc->tree->flags & MDBX_INTEGERKEY) {
    if (key->iov_len == 8) {
      if (unlikely(7 & (uintptr_t)key->iov_base)) {
        /* copy instead of return error to avoid break compatibility */
        aligned_key.iov_base = bcopy_8(&aligned_keybytes, key->iov_base);
        aligned_key.iov_len = key->iov_len;
        key = &aligned_key;
      }
    } else if (key->iov_len == 4) {
      if (unlikely(3 & (uintptr_t)key->iov_base)) {
        /* copy instead of return error to avoid break compatibility */
        aligned_key.iov_base = bcopy_4(&aligned_keybytes, key->iov_base);
        aligned_key.iov_len = key->iov_len;
        key = &aligned_key;
      }
    } else
      return MDBX_BAD_VALSIZE;

    if (unlikely(mc->clc->k.lmin != mc->clc->k.lmax))
      mc->clc->k.lmin = mc->clc->k.lmax = key->iov_len;
  }
  if (mc->tree->flags & MDBX_INTEGERDUP) {
    if (data->iov_len == 8) {
      if (unlikely(7 & (uintptr_t)data->iov_base)) {
        if (unlikely(flags & MDBX_MULTIPLE)) {
          /* LY: использование alignof(uint64_t) тут не подходил из-за ошибок
           * MSVC и некоторых других компиляторов, когда для элементов
           * массивов/векторов обеспечивает выравнивание только на 4-х байтовых
           * границу и одновременно alignof(uint64_t) == 8. */
          if (MDBX_WORDBITS > 32 || (3 & (uintptr_t)data->iov_base) != 0)
            return MDBX_BAD_VALSIZE;
        } else {
          /* copy instead of return error to avoid break compatibility */
          aligned_data.iov_base = bcopy_8(&aligned_databytes, data->iov_base);
          aligned_data.iov_len = data->iov_len;
          data = &aligned_data;
        }
      }
    } else if (data->iov_len == 4) {
      if (unlikely(3 & (uintptr_t)data->iov_base)) {
        if (unlikely(flags & MDBX_MULTIPLE))
          return MDBX_BAD_VALSIZE;
        /* copy instead of return error to avoid break compatibility */
        aligned_data.iov_base = bcopy_4(&aligned_databytes, data->iov_base);
        aligned_data.iov_len = data->iov_len;
        data = &aligned_data;
      }
    } else
      return MDBX_BAD_VALSIZE;
    if (unlikely(mc->clc->v.lmin != mc->clc->v.lmax))
      mc->clc->v.lmin = mc->clc->v.lmax = data->iov_len;
  }
  return cursor_put(mc, key, data, flags);
}

__hot int cursor_del(MDBX_cursor *mc, unsigned flags) {
  if (unlikely(!is_filled(mc))) {
    if (!F_ISSET(mc->flags, z_inner | z_eof_hard))
      return MDBX_ENODATA;
    if (outer_next(cursor_outer(mc), nullptr, nullptr, MDBX_NEXT) != MDBX_SUCCESS)
      return MDBX_ENODATA;
  }

  int rc = cursor_touch(mc, nullptr, nullptr);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  page_t *mp = mc->pg[mc->top];
  cASSERT0(mc, is_modifiable(mc->txn, mp));
  if (!MDBX_DISABLE_VALIDATION && unlikely(!check_leaf_type(mc, mp))) {
    ERROR("unexpected leaf-page #%" PRIaPGNO " type 0x%x seen by cursor", mp->pgno, mp->flags);
    return MDBX_CORRUPTED;
  }
  if (is_dupfix_leaf(mp))
    goto del_key;

  node_t *node = page_node(mp, mc->ki[mc->top]);
  if (node_flags(node) & N_DUP) {
    if (flags & (MDBX_ALLDUPS | /* for compatibility */ MDBX_NODUPDATA)) {
      /* will subtract the final entry later */
      mc->tree->items -= mc->subcur->nested_tree.items - 1;
      PROBE_AGAINST_DANGLING_DBI(mc);
    } else {
      if ((node_flags(node) & N_TREE) == 0) {
        page_t *sp = node_data(node);
        cASSERT0(mc, is_subpage(sp));
        sp->txnid = mp->txnid;
        mc->subcur->cursor.pg[0] = sp;
      }
      rc = cursor_del(&mc->subcur->cursor, 0);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      PROBE_AGAINST_DANGLING_DBI(mc);
      /* If sub-DB still has entries, we're done */
      if (mc->subcur->nested_tree.items) {
        if (node_flags(node) & N_TREE) {
          /* update table info */
          mc->subcur->nested_tree.mod_txnid = mc->txn->txnid;
          memcpy(node_data(node), &mc->subcur->nested_tree, sizeof(tree_t));
          /* fix other sub-DB cursors pointed at the same sub-tree */
          for (MDBX_cursor *m2 = mc->txn->cursors[cursor_dbi(mc)]; m2; m2 = m2->next) {
            if (is_related(mc, m2) && m2->pg[mc->top] == mp && m2->ki[mc->top] == mc->ki[mc->top])
              *m2->subcur->cursor.tree = mc->subcur->nested_tree;
          }
        } else {
          /* shrink sub-page */
          node = node_shrink(mp, mc->ki[mc->top], node);
          mc->subcur->cursor.pg[0] = node_data(node);
          /* fix other sub-DB cursors pointed at sub-pages on this page */
          for (MDBX_cursor *m2 = mc->txn->cursors[cursor_dbi(mc)]; m2; m2 = m2->next) {
            if (!is_related(mc, m2) || m2->pg[mc->top] != mp)
              continue;
            const node_t *inner = node;
            if (unlikely(m2->ki[mc->top] >= page_numkeys(mp))) {
              m2->flags |= z_eof_hard | z_eof_soft | z_after_delete;
              inner_gone_unconditional(m2);
              continue;
            }
            if (m2->ki[mc->top] != mc->ki[mc->top]) {
              inner = page_node(mp, m2->ki[mc->top]);
              if (node_flags(inner) != N_DUP)
                continue;
            } else
              m2->subcur->nested_tree = mc->subcur->nested_tree;
            m2->subcur->cursor.pg[0] = node_data(inner);
          }
        }
        mc->tree->items -= 1;
        cASSERT0(mc, mc->tree->items > 0 && mc->tree->height > 0 && mc->tree->root != P_INVALID);
        cASSERT0(mc, mc->stash < 1);
        PROBE_AGAINST_DANGLING_DBI(mc);
        return rc;
      }
      /* otherwise fall thru and delete the sub-DB */
    }

    if ((node_flags(node) & N_TREE) && mc->subcur->cursor.tree->height) {
      /* add all the child DB's pages to the free list */
      rc = tree_drop(&mc->subcur->cursor);
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
    }
    for (MDBX_cursor *m2 = mc->txn->cursors[cursor_dbi(mc)]; m2; m2 = m2->next) {
      if (is_related_include(mc, m2) && m2->pg[mc->top] == mp && m2->ki[mc->top] == mc->ki[mc->top])
        inner_gone_unconditional(m2);
    }
    cASSERT0(mc, mc->stash < 1);
    PROBE_AGAINST_DANGLING_DBI(mc);
  } else {
    cASSERT0(mc, !inner_pointed(mc));
    /* MDBX passes N_TREE in 'flags' to delete a DB record */
    if (unlikely((node_flags(node) ^ flags) & N_TREE))
      return MDBX_INCOMPATIBLE;
  }

  /* add large/overflow pages to free list */
  if (node_flags(node) & N_BIG) {
    pgr_t lp = page_get_large(mc, node_largedata_pgno(node), mp->txnid);
    if (unlikely((rc = lp.err) || (rc = page_retire(mc, lp.page))))
      goto fail;
  }

del_key:
  mc->tree->items -= 1;
  const MDBX_dbi dbi = (MDBX_dbi)cursor_dbi(mc);
  indx_t ki = mc->ki[mc->top];
  mp = mc->pg[mc->top];
  cASSERT0(mc, is_leaf(mp));
  node_del(mc, mc->tree->dupfix_size);

  /* Adjust other cursors pointing to mp */
  for (MDBX_cursor *m2 = mc->txn->cursors[dbi]; m2; m2 = m2->next) {
    MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
    if (!is_related(mc, m3) || m3->pg[mc->top] != mp)
      continue;
    if (m3->ki[mc->top] == ki) {
      m3->flags |= z_after_delete;
      inner_gone(m3);
    } else {
      m3->ki[mc->top] -= m3->ki[mc->top] > ki;
      if (inner_pointed(m3))
        cursor_inner_refresh(m3, mp, m3->ki[mc->top]);
    }
  }
  inner_gone(mc);
  cASSERT0(mc, mc->stash == 0);
  PROBE_AGAINST_DANGLING_DBI(mc);

  rc = tree_rebalance(mc);
  if (unlikely(rc != MDBX_SUCCESS))
    goto fail;

  cASSERT0(mc, mc->stash < 1);
  mc->flags |= z_after_delete;
  if (unlikely(mc->top < 0)) {
    /* DB is totally empty now, just bail out.
     * Other cursors adjustments were already done
     * by rebalance and aren't needed here. */
    cASSERT0(mc, mc->tree->items == 0 && (mc->tree->root == P_INVALID || (is_inner(mc) && !mc->tree->root)) &&
                     mc->flags < 0);
    return MDBX_SUCCESS;
  }

  ki = mc->ki[mc->top];
  mp = mc->pg[mc->top];
  cASSERT0(mc, is_leaf(mc->pg[mc->top]));
  size_t nkeys = page_numkeys(mp);
  cASSERT0(mc, (mc->tree->items > 0 && nkeys > 0) || ((mc->flags & z_inner) && mc->tree->items == 0 && nkeys == 0));

  /* Adjust this and other cursors pointing to mp */
  const intptr_t top = /* может быть сброшен в -1 */ mc->top;
  for (MDBX_cursor *m2 = mc->txn->cursors[dbi]; m2; m2 = m2->next) {
    MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
    if (top > m3->top || m3->pg[top] != mp)
      continue;
    /* if m3 points past last node in page, find next sibling */
    if (m3->ki[top] >= nkeys) {
      rc = cursor_sibling_right(m3);
      if (rc == MDBX_NOTFOUND) {
        inner_gone(m3);
        rc = MDBX_SUCCESS;
        continue;
      }
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
    }
    if (!m3->subcur)
      continue;
    if (!is_filled(m3)) {
      inner_gone_unconditional(m3);
      continue;
    }
    if (m3->ki[top] >= ki || /* уже переместились вправо */ m3->pg[top] != mp) {
      node = page_node(m3->pg[m3->top], m3->ki[m3->top]);
      if ((node_flags(node) & N_DUP) == 0) {
        inner_gone_unconditional(m3);
        continue;
      }
      /* Тут три варианта событий:
       * 1) Вложенный курсор уже инициализирован, у узла есть флаг N_TREE,
       *    соответственно дубликаты вынесены в отдельное дерево с корнем
       *    в отдельной странице = ничего корректировать не требуется.
       * 2) Вложенный курсор уже инициализирован, у узла нет флага N_TREE,
       *    соответственно дубликаты размещены на вложенной sub-странице.
       * 3) Курсор стоял на удалённом элементе, который имел одно значение,
       *    а после удаления переместился на следующий элемент с дубликатами.
       *    В этом случае вложенный курсор не инициализирован и теперь его
       *    нужно установить на первый дубликат. */
      if (is_pointed(&m3->subcur->cursor)) {
        if ((node_flags(node) & N_TREE) == 0) {
          cASSERT0(m3, m3->subcur->cursor.top == 0 && m3->subcur->nested_tree.height == 1);
          m3->subcur->cursor.pg[0] = node_data(node);
        }
      } else {
        rc = cursor_dupsort_setup(m3, node, m3->pg[m3->top]);
        if (unlikely(rc != MDBX_SUCCESS))
          goto fail;
        if (node_flags(node) & N_TREE) {
          rc = inner_first(&m3->subcur->cursor, nullptr);
          if (unlikely(rc != MDBX_SUCCESS))
            goto fail;
        }
        m3->flags |= z_after_delete;
      }
    }
  }

  cASSERT0(mc, mc->stash < 1);
  PROBE_AGAINST_DANGLING_DBI(mc);
  cASSERT0(mc, rc == MDBX_SUCCESS);
  if (CHECKS2_ENABLED())
    rc = cursor_validate(mc);
  return rc;

fail:
  mc->txn->flags |= MDBX_TXN_ERROR;
  return rc;
}
