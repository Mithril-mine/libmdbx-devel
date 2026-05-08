/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanations license change,
/// credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

#if !defined(NDEBUG)
MDBX_MAYBE_UNUSED static char *cursor_dump_stack(const MDBX_cursor *mc, const char *caption, bool print_subcursor,
                                                 bool print_pageptr) {
  static char buf[1024];
  char *const end = buf + sizeof(buf) - 1;
  char *tail = buf;

loop:
  tail += snprintf(tail, end - tail, "%s[%i]", caption, mc->top);
  for (intptr_t i = 0; i <= mc->top; ++i)
    if (print_pageptr)
      tail +=
          snprintf(tail, end - tail, ".%p#%u->%u", __Wpedantic_format_voidptr(mc->pg[i]), mc->pg[i]->pgno, mc->ki[i]);
    else
      tail += snprintf(tail, end - tail, ".%u", mc->ki[i]);
  if (mc->flags & z_inner)
    tail += snprintf(tail, end - tail, "%s", "-inner");
  if (mc->flags & z_after_delete)
    tail += snprintf(tail, end - tail, "%s", "-after.delete");
  if (mc->flags & z_hollow)
    tail += snprintf(tail, end - tail, "%s", "-hollow");
  else if (mc->flags & z_eof_hard)
    tail += snprintf(tail, end - tail, "%s", "-eof.hard");
  else if (mc->flags & z_eof_soft)
    tail += snprintf(tail, end - tail, "%s", "-eof.soft");

  if (print_subcursor && mc->subcur) {
    caption = "->subcur";
    mc = &mc->subcur->cursor;
    goto loop;
  }
  return buf;
}
#endif /* NDEBUG */

/* mostly for debug */
static int cutoff_leaf(MDBX_cursor *axe, unsigned alldups) {
#ifndef NDEBUG
  DKBUF_DEBUG;
  page_t *mp;
  MDBX_val k, v;
  MDBX_cursor *blade = nullptr;
  const char *s = nullptr;
  const char *sk = nullptr;
  const char *sv = nullptr;

  if (alldups && inner_pointed(axe)) {
    blade = axe;
    s = "outer.dups-node";
    mp = axe->pg[axe->top];
    node_t *node = page_node(mp, axe->ki[axe->top]);
    k.iov_base = node_key(node);
    k.iov_len = node_ks(node);
    sv = "*";
  } else {
    blade = inner_pointed(axe) ? &axe->subcur->cursor : axe;
    s = inner_pointed(axe) ? "outer.inner.entry" : (is_inner(axe) ? "inner.entry" : "outer.entry");

    if (is_inner(blade)) {
      MDBX_cursor *outer = cursor_outer(blade);
      node_t *node = page_node(outer->pg[outer->top], outer->ki[outer->top]);
      k.iov_base = node_key(node);
      k.iov_len = node_ks(node);
    }

    mp = blade->pg[blade->top];
    if (is_dupfix_leaf(mp)) {
      v.iov_base = page_dupfix_ptr(mp, blade->ki[blade->top], mp->dupfix_ksize);
      v.iov_len = mp->dupfix_ksize;
      sv = DVAL_DEBUG(&v);
    } else {
      node_t *node = page_node(mp, blade->ki[blade->top]);
      v.iov_len = node_ks(node);
      v.iov_base = node_key(node);
      if (!is_inner(blade)) {
        k = v;
        v.iov_len = node_ds(node);
        v.iov_base = node_data(node);
      }
    }
    sv = DVAL_DEBUG(&v);
  }
  sk = DKEY_DEBUG(&k);

  (void)k;
  (void)v;
  (void)sk;
  (void)sv;
  VERBOSE("cut %s => \"%s\".\"%s\"", cursor_dump_stack(axe, s, true, false), sk, sv);
#endif /* !NDEBUG */
  return cursor_del(axe, alldups);
}

#if MDBX_ENABLE_BUNCHES_REMOVAL

intptr_t tree_diff_level(const MDBX_cursor *left, const MDBX_cursor *right) {
  ASSERT(left->top == right->top);
  for (intptr_t i = 0; i <= left->top; ++i) {
    ASSERT(left->pg[i] == right->pg[i]);
    if (left->ki[i] != right->ki[i])
      return (left->ki[i] < right->ki[i]) ? i : INT_MIN;
  }
  return MDBX_RESULT_TRUE;
}

int tree_cutoff_twig(MDBX_cursor *mc, const pgno_t pgno, size_t deep, txnid_t parent_txnid, const bool whole_tree) {
  /* Пытаемся избежать чтения листовых страниц и связанных с этим page faults. */
  if (whole_tree && (mc->checking & z_pagecheck) == 0 && deep == mc->tree->height && !mc->tree->large_pages &&
      parent_txnid < mc->txn->txnid /* is_frozen (mc->txn, pagent_page) */
      && !mc->subcur && cursor_dbi(mc) != MAIN_DBI) {
    /* Если не запрошена проверка структуры страниц, и узел ссылается по последний уровень дерева,
     * и нет large/overlow страниц, и не может быть вложенных dupsort-деревьев,
     * то можно избежать обращения к листовой странице, при условии что родительская в статусе FROZEN. */
    return page_retire_ex(mc, pgno, nullptr, P_FROZEN + (mc->checking & (P_LEAF | P_DUPFIX)));
  }

  int err;
  const pgr_t pgr = page_get_three(mc, pgno, parent_txnid);
  if (unlikely(pgr.err != MDBX_SUCCESS))
    return pgr.err;

  if (is_branch(pgr.page)) {
    const size_t branch_nkeys = page_numkeys(pgr.page);
    for (size_t i = 0; i < branch_nkeys; ++i) {
      const node_t *const branch_node = page_node(pgr.page, i);
      cASSERT0(mc, node_flags(branch_node) == 0);
      err = tree_cutoff_twig(mc, node_pgno(branch_node), deep + 1, pgr.page->txnid, whole_tree);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
  } else {
    tASSERT0(mc, is_leaf(pgr.page));
    if (!whole_tree) {
#ifndef NDEBUG
      MDBX_val key;
      if (is_dupfix_leaf(pgr.page)) {
        key.iov_base = page_dupfix_ptr(pgr.page, 0, pgr.page->dupfix_ksize);
        key.iov_len = pgr.page->dupfix_ksize;
      } else {
        node_t *key_node = page_node(pgr.page, 0);
        key.iov_base = node_key(key_node);
        key.iov_len = node_ks(key_node);
      }
      (void)key;
      DKBUF_DEBUG;
      VERBOSE("cut leaf page %u with %zu items (%" PRIi64 "->%" PRIi64 ") \"%s\"", pgr.page->pgno,
              page_numkeys(pgr.page), mc->tree->items, mc->tree->items - page_numkeys(pgr.page), DKEY_DEBUG(&key));
#endif /* NDEBUG */
      mc->tree->items -= page_numkeys(pgr.page);
    }
    if (mc->tree->large_pages || mc->subcur) {
      tASSERT0(mc, !is_dupfix_leaf(pgr.page));
      const size_t leaf_nkeys = page_numkeys(pgr.page);
      for (size_t i = 0; i < leaf_nkeys; ++i) {
        const node_t *const leaf_node = page_node(pgr.page, i);
        if (leaf_node->flags & (N_DUP | N_TREE)) {
          if (unlikely((leaf_node->flags & N_DUP) == 0))
            return /* disallowing implicit table deletion */ MDBX_INCOMPATIBLE;
          err = cursor_push(mc, pgr.page, 0);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
          err = cursor_dupsort_setup(mc, leaf_node, pgr.page);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
          if (!whole_tree) {
#ifndef NDEBUG
            DKBUF_DEBUG;
            MDBX_val key;
            key.iov_base = node_key(leaf_node);
            key.iov_len = node_ks(leaf_node);
            (void)key;
            VERBOSE("cut leaf %s %u with %" PRIi64 " items (%" PRIi64 "->%" PRIi64 ") \"%s\"",
                    (leaf_node->flags & N_TREE) ? "tree" : "sub-page", mc->subcur->cursor.tree->root,
                    mc->subcur->cursor.tree->items, mc->tree->items,
                    mc->tree->items - (mc->subcur->cursor.tree->items - 1), DKEY_DEBUG(&key));
#endif /* NDEBUG */
            mc->tree->items -= mc->subcur->cursor.tree->items - 1;
          }
          if (leaf_node->flags & N_TREE) {
            MDBX_cursor *const inner = &mc->subcur->cursor;
            err = tree_cutoff_twig(inner, inner->tree->root, 1, tbl_root_txnid(mc->txn, cursor_dbi(mc)), true);
            if (unlikely(err != MDBX_SUCCESS))
              return err;
          }
          cursor_pop(mc);
        } else if (leaf_node->flags & N_BIG) {
          err = page_retire_ex(mc, node_largedata_pgno(leaf_node), nullptr, 0);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }
      }
    }
  }

  return page_retire(mc, pgr.page);
}

static int cutoff_zikkurat(MDBX_cursor *begin, MDBX_cursor *end, intptr_t level, bool end_including) {
  ASSERT(begin->txn == end->txn && is_pointed(begin) && is_pointed(end));
  ASSERT(begin->clc == end->clc && begin->top == end->top);
  ASSERT(level >= 0 && level <= begin->top);
  ASSERT(level < end->top);

  if (end_including) {
    for (intptr_t i = level; i <= end->top; ++i)
      if (end->ki[i] < page_numkeys(end->pg[i]) - 1) {
        end_including = false;
        break;
      }
    if (end_including && inner_pointed(end)) {
      for (intptr_t i = 0; i <= end->subcur->cursor.top; ++i)
        if (end->subcur->cursor.ki[i] < page_numkeys(end->subcur->cursor.pg[i]) - 1) {
          end_including = false;
          break;
        }
    }
  }

#ifndef NDEBUG
  VERBOSE(">> %s", cursor_dump_stack(begin, "begin", false, false));
  VERBOSE(">> %s", cursor_dump_stack(end, end_including ? "end-including" : "end-excluding", false, false));
#endif /* NDEBUG */

  cursor_couple_t dozer;
  MDBX_cursor *const mc = cursor_clone_complete(end, &dozer);
  dozer.outer.next = mc->txn->cursors[cursor_dbi(mc)];
  mc->txn->cursors[cursor_dbi(mc)] = &dozer.outer;
  int err = MDBX_SUCCESS;
  do {
    cursor_cpstk(end, mc);
    mc->top = level;

    while (true) {
      if (mc->tree->items < 4)
        goto bailout;

#ifndef NDEBUG
      VERBOSE(">= %s", cursor_dump_stack(begin, "begin", false, false));
      VERBOSE(">= %s", cursor_dump_stack(end, end_including ? "end-including" : "end-excluding", false, false));
#endif /* NDEBUG */

      page_t *mp = mc->pg[level];
      const indx_t nkeys = page_numkeys(mp);
      if (nkeys < 3)
        /* пропускаем, чтобы избежать слияния страниц, так как при этом
         * может сильно изменится конфигурация дерева, что потребует перезапуска процедуры быстрого удаления */
        goto next;
      const intptr_t ki_begin = 1 /* Пропускаем начальные узлы на branch-страницах,
                                   * чтобы не заниматься слиянием и обновлением ключей в родительских страницах */
                                + ((mp != begin->pg[level]) ? 0 : begin->ki[level]);
      const intptr_t ki_end = (mp != end->pg[level]) ? nkeys : end->ki[level] + end_including;
      if (ki_begin >= ki_end)
        goto next;

      ASSERT((begin->flags & z_after_delete) == 0);
      err = cursor_touch(mc, nullptr, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        goto bailout;
      mp = mc->pg[mc->top];
      mc->ki[level] = ki_end;
      do {
        node_t *const node = page_node(mp, mc->ki[level] -= 1);
        cASSERT0(mc, is_branch(mp) && node_flags(node) == 0);
#ifndef NDEBUG
        VERBOSE("== %s", cursor_dump_stack(mc, "del-tree-node", false, false));
#endif /* NDEBUG */
        err = tree_cutoff_twig(mc, node_pgno(node), level + 1, mp->txnid, false);
        if (unlikely(err != MDBX_SUCCESS))
          goto bailout;
        node_del(mc, 0);
      } while (page_numkeys(mp) > 2 && mc->ki[level] > ki_begin);

      const indx_t dropped = nkeys - page_numkeys(mp);
      for (MDBX_cursor *m2 = dozer.outer.next; m2; m2 = m2->next) {
        MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
        if (m3->top < level || m3->pg[level] != mp || m3->ki[level] < mc->ki[level])
          continue;
        if (m3->ki[level] >= ki_end) {
          m3->ki[level] -= dropped;
          ASSERT(level != m3->top);
          /* if (level == m3->top && inner_pointed(m3))
            cursor_inner_refresh(m3, mp, m3->ki[level]); */
        } else {
          ASSERT(m3 != begin);
          ASSERT(m3 != end || end_including);
          end_including &= m3 != end;
          m3->top = level;
          if (mc->ki[level] < page_numkeys(mp))
            m3->ki[level] = mc->ki[level];
          else
            err = cursor_sibling_right(m3);
          if (unlikely(err != MDBX_SUCCESS) && err != MDBX_NOTFOUND)
            goto bailout;
          err = tree_deepen_edge(m3, (err == MDBX_NOTFOUND) ? Z_LAST : Z_FIRST);
          if (unlikely(err != MDBX_SUCCESS))
            goto bailout;
          if (inner_pointed(m3))
            cursor_inner_refresh(m3, m3->pg[m3->top], m3->ki[m3->top]);
          m3->flags |= z_after_delete;
        }
      }

#ifndef NDEBUG
      VERBOSE("<= %s", cursor_dump_stack(begin, "begin", false, false));
      VERBOSE("<= %s", cursor_dump_stack(end, end_including ? "end-including" : "end-excluding", false, false));
#endif /* NDEBUG */

    next:
      err = cursor_sibling_left(mc);
      if (unlikely(err == MDBX_NOTFOUND))
        break;
      if (unlikely(err != MDBX_SUCCESS))
        goto bailout;
      for (intptr_t i = 0; i <= level; ++i)
        if (mc->ki[i] < begin->ki[i])
          goto bailout;
    }
  } while (++level < begin->top);
  err = MDBX_SUCCESS;

bailout:
#ifndef NDEBUG
  VERBOSE(">> %s", cursor_dump_stack(begin, "begin", false, false));
  VERBOSE(">> %s", cursor_dump_stack(end, end_including ? "end-including" : "end-excluding", false, false));
#endif /* NDEBUG */
  mc->txn->cursors[cursor_dbi(mc)] = dozer.outer.next;
  if (is_inner(mc)) {
    *begin->tree = *mc->tree;
    *end->tree = *mc->tree;
  }
  if (likely(err == MDBX_SUCCESS) && (end->flags & z_after_delete))
    err = MDBX_RESULT_TRUE;
  return err;
}

static int cutoff_storey(MDBX_cursor *begin, MDBX_cursor *end, intptr_t level, bool end_including) {
  ASSERT(begin->txn == end->txn && is_pointed(begin) && is_pointed(end));
  ASSERT(begin->clc == end->clc && begin->top == end->top);
  ASSERT(level >= 0 && level <= begin->top);
  ASSERT(memcmp(begin->tree, end->tree, sizeof(tree_t)) == 0);
  cASSERT0(begin, (begin->flags & (z_after_delete | z_eof_soft | z_eof_hard)) == 0);

  int err;
  intptr_t top = end->top;
  if (!is_inner(end)) {
    err = cursor_touch(begin, nullptr, nullptr);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (end->pg[top] != begin->pg[top]) {
      err = cursor_touch(end, nullptr, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    if (MDBX_ENABLE_BUNCHES_REMOVAL && level < top) {
      err = cutoff_zikkurat(begin, end, level, end_including);
      if (unlikely(err != MDBX_SUCCESS)) {
        if (unlikely(err != MDBX_RESULT_TRUE))
          return err;
        if (begin->flags & z_after_delete)
          return MDBX_SUCCESS;
        end_including = false;
      }
      cASSERT0(begin, is_filled(begin));
      ASSERT(tree_diff_level(begin, end) >= MDBX_RESULT_TRUE);
    }
    ASSERT(is_pointed(begin) && is_pointed(end));
    while (end->pg[top] != begin->pg[top] || end->ki[top] > begin->ki[top]) {
      if (!end_including) {
        err = outer_prev(end, nullptr, nullptr, MDBX_PREV_NODUP);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
        if (end->pg[top] == begin->pg[top] && end->ki[top] == begin->ki[top])
          break;
      }
      ASSERT(is_pointed(begin) && is_pointed(end));
      ASSERT(tree_diff_level(begin, end) >= 0);
      err = cutoff_leaf(end, MDBX_ALLDUPS);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      ASSERT(end->top == begin->top);
      top = end->top;
      end_including = false;
    }
    ASSERT(is_pointed(begin) && is_pointed(end));
    ASSERT(begin->tree->items > 1);
    ASSERT(tree_diff_level(begin, end) >= MDBX_RESULT_TRUE);
    return cutoff_leaf(begin, MDBX_ALLDUPS);
  }

  MDBX_cursor *outer = cursor_outer(end);
  ASSERT(begin->pg[0] == end->pg[0]);
  err = cursor_touch(outer, nullptr, nullptr);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  node_t *outer_node = page_node(outer->pg[outer->top], outer->ki[outer->top]);
  ASSERT(node_flags(outer_node) & N_DUP);
  if (node_flags(outer_node) & N_TREE) {
    err = cursor_touch(end, nullptr, nullptr);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    begin->tree->mod_txnid = end->tree->mod_txnid = end->txn->txnid;
    begin->tree->root = end->tree->root;
    memcpy(node_data(outer_node), end->tree, sizeof(tree_t));

    if (end->pg[top] != begin->pg[top]) {
      err = cursor_touch(begin, nullptr, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
  } else {
#ifndef MDBX_EVENBUG20260405_FIX
    end->pg[0] = begin->pg[0];
#endif /* !MDBX_EVENBUG20260405_FIX */
  }
  ASSERT(memcmp(begin->tree, end->tree, sizeof(tree_t)) == 0);

  if (end->tree->items > 2) {
    const uint64_t dups = end->tree->items;
    if (MDBX_ENABLE_BUNCHES_REMOVAL && level < top) {
      err = cutoff_zikkurat(begin, end, level, end_including);
      if (unlikely(err != MDBX_SUCCESS)) {
        if (unlikely(err != MDBX_RESULT_TRUE))
          return err;
        end_including = false;
        if (begin->flags & z_after_delete)
          return MDBX_SUCCESS;
      }
      cASSERT0(begin, is_filled(begin));
      ASSERT(tree_diff_level(begin, end) >= MDBX_RESULT_TRUE);
    }
    ASSERT(is_pointed(begin) && is_pointed(end));
    while (end->pg[top] != begin->pg[top] || end->ki[top] > begin->ki[top]) {
      if (!end_including) {
        err = inner_prev(end, nullptr);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
        if (end->pg[top] == begin->pg[top] && end->ki[top] == begin->ki[top])
          break;
      }
      ASSERT(is_pointed(begin) && is_pointed(end));
      ASSERT(tree_diff_level(begin, end) >= 0);
      err = cutoff_leaf(end, 0);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      ASSERT(end->top == begin->top);
      top = end->top;
      end_including = false;
      if (end->tree->items == 2)
        break;
    }
    ASSERT(is_pointed(begin) && is_pointed(end));
    ASSERT(end->tree->items > 1);
    outer->tree->items -= dups - end->tree->items;
    *begin->tree = *end->tree;
  }

  ASSERT(is_pointed(begin) && is_pointed(end));
  ASSERT(tree_diff_level(begin, end) >= MDBX_RESULT_TRUE);
  intptr_t left = end->ki[top] - begin->ki[top] + end_including;
  /* Алгоритмически такая проверка корректна. Однако, на практике возможны ситуации, когда end->pg[top] !=
   * begin->pg[top], когда на каждую листовую страницу помещается только по одному ключу, это последние два ключа в
   * под-дереве и один из них должен быть удалён. При этом проверки результатов tree_diff_level() ниже достаточно для
   * проверки корректности удаления. Поэтому проще отключить эту проверку, нежели усложнять условия.
   * ASSERT(end->pg[top] == begin->pg[top] && left >= 0); */
  outer = cursor_outer(begin);
  do {
    ASSERT(tree_diff_level(outer, cursor_outer(end)) >= MDBX_RESULT_TRUE ||
           tree_diff_level(begin, end) >= MDBX_RESULT_TRUE);
    err = cutoff_leaf(outer, 0);
    if (unlikely(err != MDBX_SUCCESS))
      break;
    end->pg[top] = begin->pg[top];
    *end->tree = *begin->tree;
  } while (--left > 0);
  return err;
}

#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */

int tree_curoff_range(MDBX_cursor *begin, MDBX_cursor *end, bool end_including) {
  cASSERT1(begin, cursor_is_tracked(begin));
  cASSERT1(end, cursor_is_tracked(end));
  ASSERT(begin->txn == end->txn && begin->clc == end->clc && begin->dbi_state == end->dbi_state &&
         is_inner(begin) == is_inner(end) && (begin->tree == end->tree || is_inner(begin)) && begin->top == end->top);

  if (unlikely(begin->clc != end->clc))
    return MDBX_PROBLEM;
  if (unlikely(!is_pointed(begin) || !is_pointed(end)))
    return MDBX_ENODATA;
  if (unlikely(begin->top != end->top))
    return MDBX_PROBLEM;

#ifndef NDEBUG
  VERBOSE(">> %s", cursor_dump_stack(begin, "begin", true, false));
  VERBOSE(">> %s", cursor_dump_stack(end, end_including ? "end-including" : "end-excluding", true, false));
#endif /* NDEBUG */

#if MDBX_ENABLE_BUNCHES_REMOVAL
  /* ищем ближайший к корню ярус с различием в позициях */
  const intptr_t level = tree_diff_level(begin, end);
  if (level < 0) {
    if (unlikely(level != MDBX_RESULT_TRUE))
      return MDBX_ENODATA;

    /* позиции основных курсоров совпадают, смотрим на вложенные dupsort-курсоры */
    if (!begin->subcur)
      /* вложенных dupsort-курсоров нет, нечего удалять */
      return end_including ? cutoff_leaf(begin, 0) : MDBX_SUCCESS;

    const intptr_t inner_level = tree_diff_level(&begin->subcur->cursor, &end->subcur->cursor);
    if (unlikely(inner_level < 0)) {
      if (unlikely(inner_level != MDBX_RESULT_TRUE))
        return MDBX_ENODATA;
      /* у вложенных dupsort-курсоров позиции тоже совпадают, нечего удалять */
      return end_including ? cutoff_leaf(begin, 0) : MDBX_SUCCESS;
    }

    if (end_including && cursor_on_first(&begin->subcur->cursor) && cursor_on_last(&end->subcur->cursor))
      /* удаляем целиком dupsort-узел */
      return cutoff_leaf(begin, MDBX_ALLDUPS);
    else
      /* удаляем диапазон во вложенном dupsort-дереве */
      return cutoff_storey(&begin->subcur->cursor, &end->subcur->cursor, inner_level, end_including);
  }

  if (end_including && cursor_on_first(begin) && cursor_on_last(end)) {
    if (is_inner(begin))
      /* специфический случай когда функция была вызвана для вложенного dupsort-дерева */
      return cutoff_leaf(cursor_outer(begin), MDBX_ALLDUPS);
    else if (!begin->subcur || (cursor_on_first(&begin->subcur->cursor) && cursor_on_last(&end->subcur->cursor)))
      /* удаляем дерево целиком */
      return tbl_purge(begin);
  }

  intptr_t cmp = -1;
  /* coverity[ASSERT_SIDE_EFFECT] */
  ASSERT((cmp = cursor_cmp(begin, end)) < 0 || (cmp == 0 && end_including));
  if (begin->subcur) {
    cursor_couple_t inner_trimmer;
    if (is_pointed(&end->subcur->cursor) && (!end_including || !cursor_on_last(&end->subcur->cursor))) {
      /* в конце подлежащего удалению диапазона находится часть dupsort-куста,
       * удаляем начало этого вложенного dupsort-дерева между end и предыдущим узлом основного дерева. */
      MDBX_cursor *const tail = &end->subcur->cursor;
      MDBX_cursor *const head = &cursor_clone_complete(end, &inner_trimmer)->subcur->cursor;
      int err = inner_first(head, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;

      const intptr_t inner_level = tree_diff_level(head, tail);
      ASSERT(inner_level >= MDBX_RESULT_TRUE);
      if (inner_level >= 0) {
        inner_trimmer.outer.next = head->txn->cursors[cursor_dbi(head)];
        head->txn->cursors[cursor_dbi(head)] = &inner_trimmer.outer;
        err = cutoff_storey(head, tail, inner_level, end_including);
        head->txn->cursors[cursor_dbi(head)] = inner_trimmer.outer.next;
      } else if (end_including) {
        err = cutoff_leaf(end, 0);
      }
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      cASSERT0(end, cursor_on_first(tail));
      err = outer_prev(end, nullptr, nullptr, MDBX_PREV_NODUP);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      cmp = cursor_cmp(begin, end);
      end_including = cmp < 0;
    }

    if (!cursor_on_first(&begin->subcur->cursor)) {
      /* в начале подлежащего удалению диапазона находится часть dupsort-куста,
       * удаляем конец этого вложенного dupsort-дерева между begin и следующим узлом основного дерева. */
      MDBX_cursor *const head = &begin->subcur->cursor;
      MDBX_cursor *const tail = &cursor_clone_complete(begin, &inner_trimmer)->subcur->cursor;
      int err = inner_last(tail, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;

      const intptr_t inner_level = tree_diff_level(head, tail);
      ASSERT(inner_level >= MDBX_RESULT_TRUE);
      if (inner_level >= 0) {
        inner_trimmer.outer.next = tail->txn->cursors[cursor_dbi(tail)];
        tail->txn->cursors[cursor_dbi(tail)] = &inner_trimmer.outer;
        err = cutoff_storey(head, tail, inner_level, true);
        tail->txn->cursors[cursor_dbi(tail)] = inner_trimmer.outer.next;
      } else {
        err = cutoff_leaf(begin, 0);
      }
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      cASSERT0(begin, cursor_on_last(head));
      err = outer_next(begin, nullptr, nullptr, MDBX_NEXT_NODUP);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      cASSERT0(begin, cursor_on_first(head));
      cmp = cursor_cmp(begin, end);
      if (cmp >= end_including)
        return MDBX_SUCCESS;
    }
  }

  return cutoff_storey(begin, end, level, end_including);

#else /* MDBX_ENABLE_BUNCHES_REMOVAL */

  intptr_t cmp = tree_cmp(begin, end);
  if (unlikely(cmp > 0))
    return MDBX_ENODATA;
  if (cmp == 0 && inner_pointed(begin) && unlikely(tree_cmp(&begin->subcur->cursor, &end->subcur->cursor) > 0))
    return MDBX_ENODATA;

  int err;
  while (cmp < 0) {
    err = cutoff_leaf(begin, (begin->subcur && cursor_on_first(&begin->subcur->cursor)) ? MDBX_ALLDUPS : 0);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if ((begin->flags & (z_eof_soft | z_eof_hard)) != 0 ||
        (inner_pointed(begin) && (begin->subcur->cursor.flags & (z_eof_soft | z_eof_hard))) != 0) {
      err = outer_next(begin, nullptr, nullptr, MDBX_NEXT);
      if (unlikely(err != MDBX_SUCCESS) && err != MDBX_NOTFOUND)
        return err;
    }
    cmp = tree_cmp(begin, end);
  }

  if (cmp == 0 && inner_pointed(begin)) {
    cmp = tree_cmp(&begin->subcur->cursor, &end->subcur->cursor);
    while (cmp < 0) {
      err = cutoff_leaf(begin, 0);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      cmp = tree_cmp(begin, end);
      if (cmp == 0 && inner_pointed(begin))
        cmp = tree_cmp(&begin->subcur->cursor, &end->subcur->cursor);
    }
  }

  return (end_including && cmp == 0 && is_filled(end)) ? cutoff_leaf(end, 0) : MDBX_SUCCESS;

#endif /* MDBX_ENABLE_BUNCHES_REMOVAL */
}
