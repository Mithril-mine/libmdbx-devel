/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

__hot bool txn_gc_detent(const MDBX_txn *const txn) {
  const txnid_t detent = mvcc_shapshot_oldest(txn->env, txn->wr.troika.txnid[txn->wr.troika.prefer_steady]);
  if (likely(detent == txn->env->gc.detent))
    return false;

  txn->env->gc.detent = detent;
  return true;
}

void txn_done_cursors(MDBX_txn *txn) {
  tASSERT(txn, txn->flags & txn_may_have_cursors);

  TXN_FOREACH_DBI_ALL(txn, i) {
    MDBX_cursor *cursor = txn->cursors[i];
    if (cursor) {
      txn->cursors[i] = nullptr;
      do
        cursor = cursor_eot(cursor, txn);
      while (cursor);
    }
  }
  txn->flags &= ~txn_may_have_cursors;
}

int txn_shadow_cursors(const MDBX_txn *parent, const size_t dbi) {
  tASSERT(parent, dbi < parent->n_dbi);
  MDBX_cursor *cursor = parent->cursors[dbi];
  if (!cursor)
    return MDBX_SUCCESS;

  MDBX_txn *const txn = parent->nested;
  tASSERT(parent, parent->flags & txn_may_have_cursors);
  MDBX_cursor *next = nullptr;
  do {
    next = cursor->next;
    if (cursor->signature != cur_signature_live) {
      ENSURE(parent->env, cursor->signature == cur_signature_wait4eot);
      continue;
    }
    tASSERT(parent, cursor->txn == parent && dbi == cursor_dbi(cursor));

    int err = cursor_shadow(cursor, txn, dbi);
    if (unlikely(err != MDBX_SUCCESS)) {
      /* не получилось забекапить курсоры */
      txn->dbi_state[dbi] = DBI_OLDEN | DBI_LINDO;
      txn->flags |= MDBX_TXN_ERROR;
      return err;
    }
    cursor->next = txn->cursors[dbi];
    txn->cursors[dbi] = cursor;
    txn->flags |= txn_may_have_cursors;
  } while ((cursor = next) != nullptr);
  return MDBX_SUCCESS;
}

int txn_commit(MDBX_txn *txn, struct commit_timestamp *ts) {
  tASSERT(txn, !txn->nested && !(txn->flags & MDBX_TXN_FINISHED));
  if (unlikely(txn->flags & MDBX_TXN_ERROR)) {
    int err = txn_abort(txn);
    return (err == MDBX_SUCCESS) ? MDBX_RESULT_TRUE : err;
  }

  MDBX_env *const env = txn->env;
  if (txn != env->txn) {
    if (unlikely(txn->flags & txn_ro_flat) == 0) {
      ERROR("attempt to commit %s txn %p", "unknown", __Wpedantic_format_voidptr(txn));
      return MDBX_EINVAL;
    }
    if (unlikely(txn->parent || (txn->flags & MDBX_TXN_HAS_CHILD) || txn == env->basal_txn)) {
      ERROR("attempt to commit %s txn %p", "strange read-only", __Wpedantic_format_voidptr(txn));
      return MDBX_PROBLEM;
    }
    txn_ro_free(txn);
    return MDBX_SUCCESS;
  }

  if (txn == env->basal_txn) {
    int rc = txn_basal_commit(txn, ts);
    if (unlikely(rc != MDBX_SUCCESS)) {
      txn->flags |= MDBX_TXN_ERROR;
      if (rc == MDBX_RESULT_TRUE)
        rc = MDBX_NOSUCCESS_PURE_COMMIT ? MDBX_RESULT_TRUE : MDBX_SUCCESS;
    }
    int err = txn_basal_end(txn, true);
    return (err == MDBX_SUCCESS) ? rc : err;
  }

  if (unlikely(!txn->parent || txn->parent->nested != txn || txn->parent->env != env)) {
    ERROR("attempt to commit %s txn %p", "strange nested", __Wpedantic_format_voidptr(txn));
    return MDBX_PROBLEM;
  }
  return txn_nested_commit(txn, ts);
}

#if !(defined(_WIN32) || defined(_WIN64))
void txn_abort_after_resurrect(MDBX_txn *txn) {
  if (likely(txn->signature == txn_signature)) {
    if (txn->nested) {
      txn_abort_after_resurrect(txn->nested);
      tASSERT(txn, !txn->nested);
    }
    txn_abort(txn);
  }
}
#endif /* Windows */

int txn_abort(MDBX_txn *txn) {
  DEBUG("txn %" PRIaTXN "%c-0x%X %p  on env %p, root page %" PRIaPGNO "/%" PRIaPGNO, txn->txnid,
        (txn->flags & txn_ro_flat) ? 'r' : 'w', txn->flags, __Wpedantic_format_voidptr(txn),
        __Wpedantic_format_voidptr(txn->env), txn->dbs[MAIN_DBI].root, txn->dbs[FREE_DBI].root);

  tASSERT(txn, /* txn->signature == txn_signature && */ !txn->nested && !(txn->flags & MDBX_TXN_HAS_CHILD));
  tASSERT(txn, (txn->flags & (MDBX_TXN_ERROR | txn_ro_flat)) || dpl_check(txn));
  txn->flags |= /* avoid merge cursors' state */ MDBX_TXN_ERROR;

  if (txn->flags & txn_may_have_cursors)
    txn_done_cursors(txn);

  MDBX_env *const env = txn->env;
  MDBX_txn *const parent = txn->parent;
  if (txn == env->basal_txn) {
    tASSERT(txn, !parent);
    if (unlikely(!txn->owner))
      return MDBX_BAD_TXN;
    return txn_basal_end(txn, true);
  }

  if (txn->parent)
    return txn_nested_abort(txn);

  tASSERT(txn, (txn->flags & (txn_ro_flat | MDBX_TXN_DIRTY | MDBX_TXN_SPILLS | MDBX_TXN_HAS_CHILD)) == txn_ro_flat);
  txn_ro_free(txn);
  return MDBX_SUCCESS;
}

typedef struct seq_latch_result {
  int err;
  uint32_t seq;
} slr_t;

__cold static slr_t latch_maindb_locked(MDBX_txn *txn, MDBX_env *const env) {
  slr_t slr = {.err = MDBX_SUCCESS, .seq = atomic_load32(&env->dbi_seqs[MAIN_DBI], mo_AcquireRelease)};
  if (env->dbs_flags[MAIN_DBI] & DB_VALID /* Флаги MainDB уже были загружены? */) {
    /* Читаем и проверяем повторно после захвата dbi-блокировки */
    if (env->dbs_flags[MAIN_DBI] == (DB_VALID | txn->dbs[MAIN_DBI].flags))
      return slr;
    if (env->basal_txn && env->txn != txn) {
      /* Параллельно выполняется пишущая транзакция, которая вероятно и произвела изменение MainDB.
       * Как-либо отказаться от использования новых атрибутов в рамках текущего процесса невозможно. */
      ERROR("MainDB db-flags changes 0x%x -> 0x%x ahead of read-txn %" PRIaTXN, txn->dbs[MAIN_DBI].flags,
            env->dbs_flags[MAIN_DBI] & ~DB_VALID, txn->txnid);
      slr.err = MDBX_INCOMPATIBLE;
      return slr;
    }

    NOTICE("renew MainDB for %s-txn %" PRIaTXN " since db-flags changes 0x%x -> 0x%x",
           (txn->flags & txn_ro_flat) ? "ro" : "rw", txn->txnid, env->dbs_flags[MAIN_DBI] & ~DB_VALID,
           txn->dbs[MAIN_DBI].flags);
    slr.seq = dbi_seq_next(env, MAIN_DBI);
    env->dbs_flags[MAIN_DBI] = DB_POISON;
    atomic_store32(&env->dbi_seqs[MAIN_DBI], slr.seq, mo_AcquireRelease);
  }

  slr.err = tbl_setup(env, &env->kvs[MAIN_DBI], &txn->dbs[MAIN_DBI]);
  if (likely(slr.seq == MDBX_SUCCESS)) {
    slr.seq = dbi_seq_next(env, MAIN_DBI);
    env->dbs_flags[MAIN_DBI] = DB_VALID | txn->dbs[MAIN_DBI].flags;
    atomic_store32(&env->dbi_seqs[MAIN_DBI], slr.seq, mo_AcquireRelease);
  }
  return slr;
}

int txn_setup_primal(MDBX_txn *txn) {
  MDBX_env *const env = txn->env;

  if (unlikely(txn->txnid < MIN_TXNID || txn->txnid > MAX_TXNID)) {
    ERROR("%s", "environment corrupted by died writer, must shutdown!");
    return MDBX_CORRUPTED;
  }
  txn->front_txnid = txn->txnid + ((txn->flags & (MDBX_WRITEMAP | MDBX_RDONLY)) == 0);

  /* Setup db info */
  tASSERT(txn, txn->dbs[FREE_DBI].flags == MDBX_INTEGERKEY);
  tASSERT(txn, check_table_flags(txn->dbs[MAIN_DBI].flags));
  VALGRIND_MAKE_MEM_UNDEFINED(txn->dbi_state, env->max_dbi);
#if MDBX_ENABLE_DBI_SPARSE
  txn->n_dbi = CORE_DBS;
  VALGRIND_MAKE_MEM_UNDEFINED(txn->dbi_sparse,
                              ceil_powerof2(env->max_dbi, CHAR_BIT * sizeof(txn->dbi_sparse[0])) / CHAR_BIT);
  txn->dbi_sparse[0] = (1u << CORE_DBS) - 1;
#else
  txn->n_dbi = (env->n_dbi < 8) ? env->n_dbi : 8;
  if (txn->n_dbi > CORE_DBS)
    memset(txn->dbi_state + CORE_DBS, 0, txn->n_dbi - CORE_DBS);
#endif /* MDBX_ENABLE_DBI_SPARSE */
  txn->dbi_state[FREE_DBI] = DBI_LINDO | DBI_VALID;
  txn->dbi_state[MAIN_DBI] = DBI_LINDO | DBI_VALID;
  txn->cursors[FREE_DBI] = nullptr;
  txn->cursors[MAIN_DBI] = nullptr;
  txn->dbi_seqs[FREE_DBI] = 0;
  txn->dbi_seqs[MAIN_DBI] = atomic_load32(&env->dbi_seqs[MAIN_DBI], mo_AcquireRelease);

  if (unlikely(env->dbs_flags[MAIN_DBI] != (DB_VALID | txn->dbs[MAIN_DBI].flags) || !txn->dbi_seqs[MAIN_DBI])) {
    int err = osal_fastmutex_acquire(&env->dbi_lock);
    if (unlikely(err != MDBX_SUCCESS)) {
      ERROR("dbi_lock failed, err %d", err);
      return err;
    }

    slr_t slr = latch_maindb_locked(txn, env);
    ENSURE(env, osal_fastmutex_release(&env->dbi_lock) == MDBX_SUCCESS);
    if (unlikely((err = slr.err) != MDBX_SUCCESS))
      return err;
    txn->dbi_seqs[MAIN_DBI] = slr.seq;
  }
  tASSERT(txn, check_table_flags(txn->dbs[MAIN_DBI].flags) && txn->dbi_seqs[MAIN_DBI]);

  if (unlikely(txn->dbs[FREE_DBI].flags != MDBX_INTEGERKEY)) {
    ERROR("unexpected/invalid db-flags 0x%x for %s", txn->dbs[FREE_DBI].flags, "GC/FreeDB");
    return MDBX_INCOMPATIBLE;
  }

  if (unlikely(env->flags & ENV_FATAL_ERROR)) {
    WARNING("%s", "environment had fatal error, must shutdown!");
    return MDBX_PANIC;
  }

  const size_t size_bytes = pgno2bytes(env, txn->geo.end_pgno);
  const size_t used_bytes = pgno2bytes(env, txn->geo.first_unallocated);
  const size_t required_bytes = (txn->flags & txn_ro_flat) ? used_bytes : size_bytes;
  eASSERT(env, env->dxb_mmap.limit >= env->dxb_mmap.current);
  int err = MDBX_SUCCESS;
  if (unlikely(required_bytes > env->dxb_mmap.current)) {
    /* Размер БД (для пишущих транзакций) или используемых данных (для
     * читающих транзакций) больше предыдущего/текущего размера внутри
     * процесса, увеличиваем. Сюда также попадает случай увеличения верхней
     * границы размера БД и отображения. */
    if (txn->geo.upper > MAX_PAGENO + 1 || bytes2pgno(env, pgno2bytes(env, txn->geo.upper)) != txn->geo.upper)
      return MDBX_UNABLE_EXTEND_MAPSIZE;
    err = dxb_resize(env, txn->geo.first_unallocated, txn->geo.end_pgno, txn->geo.upper, implicit_grow);
    eASSERT(env, err != MDBX_SUCCESS || env->dxb_mmap.limit >= env->dxb_mmap.current);
    return err;
  }

  if (unlikely(size_bytes < env->dxb_mmap.current)) {
    /* Размер БД меньше предыдущего/текущего размера внутри процесса, можно
     * уменьшить, но всё сложнее:
     *  - размер файла ДОЛЖЕН бать согласован со всеми читаемыми снимками
     *    на момент коммита последней транзакции;
     *  - в читающей транзакции размер файла может быть больше и него нельзя
     *    изменять, в том числе менять madvise (меньша размера файла нельзя,
     *    а за размером нет смысла).
     *  - в пишущей транзакции уменьшать размер файла можно только после
     *    проверки размера читаемых снимков, но в этом нет смысла, так как
     *    это будет сделано при фиксации транзакции.
     *
     *  В сухом остатке, можно только установить dxb_mmap.current равным
     *  размеру файла, а это проще сделать без вызова dxb_resize() и усложения
     *  внутренней логики.
     *
     *  В этой тактике есть недостаток: если пишущите транзакции не регулярны,
     *  и при завершении такой транзакции файл БД остаётся не-уменьшеным из-за
     *  читающих транзакций использующих предыдущие снимки. */
#if defined(_WIN32) || defined(_WIN64)
    imports.srwl_AcquireShared(&env->remap_lock);
#else
    err = osal_fastmutex_acquire(&env->remap_lock);
    if (unlikely(err != MDBX_SUCCESS)) {
      ERROR("remap_lock failed, err %d", err);
      return err;
    }
#endif
    eASSERT(env, env->dxb_mmap.limit >= env->dxb_mmap.current);
    err = osal_filesize(env->dxb_mmap.fd, &env->dxb_mmap.filesize);
    if (likely(err == MDBX_SUCCESS)) {
      eASSERT(env, env->dxb_mmap.filesize >= required_bytes);
      if (env->dxb_mmap.current > env->dxb_mmap.filesize)
        env->dxb_mmap.current =
            (env->dxb_mmap.limit < env->dxb_mmap.filesize) ? env->dxb_mmap.limit : (size_t)env->dxb_mmap.filesize;
    }
#if defined(_WIN32) || defined(_WIN64)
    imports.srwl_ReleaseShared(&env->remap_lock);
#else
    ENSURE(env, osal_fastmutex_release(&env->remap_lock) == MDBX_SUCCESS);
#endif
  }

  return err;
}

int txn_check_badbits_parked(const MDBX_txn *txn, int bad_bits) {
  tASSERT(txn, (bad_bits & MDBX_TXN_PARKED) && (txn->flags & bad_bits));
  /* Здесь осознано заложено отличие в поведении припаркованных транзакций:
   *  - некоторые функции (например mdbx_env_info_ex()), допускают
   *    использование поломанных транзакций (с флагом MDBX_TXN_ERROR), но
   *    не могут работать с припаркованными транзакциями (требуют распарковки).
   *  - но при распарковке поломанные транзакции завершаются.
   *  - получается что транзакцию можно припарковать, потом поломать вызвав
   *    mdbx_txn_break(), но далее любое её использование приведет к завершению
   *    при распарковке.
   *
   * Поэтому для припаркованных транзакций возвращается ошибка если не-включена
   * авто-распарковка, либо есть другие плохие биты. */
  if ((txn->flags & (bad_bits | MDBX_TXN_AUTOUNPARK)) != (MDBX_TXN_PARKED | MDBX_TXN_AUTOUNPARK))
    return LOG_IFERR(MDBX_BAD_TXN);

  tASSERT(txn, bad_bits == MDBX_TXN_BLOCKED || bad_bits == MDBX_TXN_BLOCKED - MDBX_TXN_ERROR);
  return mdbx_txn_unpark((MDBX_txn *)txn, false);
}

MDBX_txn *txn_alloc(const unsigned flags, MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  const intptr_t bitmap_bytes =
#if MDBX_ENABLE_DBI_SPARSE
      ceil_powerof2(env->max_dbi, CHAR_BIT * sizeof(txn->dbi_sparse[0])) / CHAR_BIT;
#else
      0;
#endif /* MDBX_ENABLE_DBI_SPARSE */
  STATIC_ASSERT(sizeof(txn->wr) > sizeof(txn->ro));
  const size_t base = (flags & txn_ro_flat) ? sizeof(MDBX_txn) - sizeof(txn->wr) + sizeof(txn->ro) : sizeof(MDBX_txn);
  const size_t size = base +
                      ((flags & txn_ro_flat) ? (size_t)bitmap_bytes + env->max_dbi * sizeof(txn->dbi_seqs[0]) : 0) +
                      env->max_dbi * (sizeof(txn->dbs[0]) + sizeof(txn->cursors[0]) + sizeof(txn->dbi_state[0]));
  txn = osal_malloc(size);
  if (unlikely(!txn))
    return txn;
  MDBX_ANALYSIS_ASSUME(size > base);
  memset(txn, 0, (MDBX_GOOFY_MSVC_STATIC_ANALYZER && base > size) ? size : base);

  txn->dbs = ptr_disp(txn, base);
  txn->cursors = ptr_disp(txn->dbs, env->max_dbi * sizeof(txn->dbs[0]));
#if MDBX_DEBUG
  txn->cursors[FREE_DBI] = nullptr; /* avoid SIGSEGV in an assertion later */
#endif
  txn->dbi_state = ptr_disp(txn, size - env->max_dbi * sizeof(txn->dbi_state[0]));
  txn->flags = flags;
  txn->env = env;

  if (flags & txn_ro_flat) {
    txn->dbi_seqs = ptr_disp(txn->cursors, env->max_dbi * sizeof(txn->cursors[0]));
#if MDBX_ENABLE_DBI_SPARSE
    txn->dbi_sparse = ptr_disp(txn->dbi_state, -bitmap_bytes);
#endif /* MDBX_ENABLE_DBI_SPARSE */
  }
  return txn;
}
