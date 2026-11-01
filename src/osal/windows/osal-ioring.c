/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-ioring.c (Windows part) — asynchronous/overlapped IO-queue for
/// file writes.

#include "../../internals.h"

/*----------------------------------------------------------------------------*/

#define ior_alignment_mask (ior->pagesize - 1)
#define ior_WriteFile_flag 1
#define OSAL_IOV_MAX (4096 / sizeof(ior_sgv_element))

void ior_put_event(osal_ioring_t *ior, HANDLE event) {
  ASSERT(event && event != INVALID_HANDLE_VALUE && event != ior);
  ASSERT(ior->event_stack < ior->allocated);
  ior->event_pool[ior->event_stack] = event;
  ior->event_stack += 1;
}

HANDLE ior_get_event(osal_ioring_t *ior) {
  ASSERT(ior->event_stack <= ior->allocated);
  if (ior->event_stack > 0) {
    ior->event_stack -= 1;
    ASSERT(ior->event_pool[ior->event_stack] != 0);
    return ior->event_pool[ior->event_stack];
  }
  return CreateEventW(nullptr, true, false, nullptr);
}

static void WINAPI ior_wocr(DWORD err, DWORD bytes, OVERLAPPED *ov) {
  osal_ioring_t *ior = ov->hEvent;
  ov->Internal = err;
  ov->InternalHigh = bytes;
  if (++ior->async_completed >= ior->async_waiting)
    SetEvent(ior->async_done);
}

int osal_ioring_create(osal_ioring_t *ior, bool enable_direct, mdbx_filehandle_t overlapped_fd) {
  memset(ior, 0, sizeof(osal_ioring_t));

  ior->overlapped_fd = overlapped_fd;
  ior->direct = enable_direct && overlapped_fd;
  ior->pagesize = globals.sys_pagesize;
  ior->pagesize_ln2 = globals.sys_pagesize_ln2;
  ior->async_done = ior_get_event(ior);
  if (!ior->async_done)
    return GetLastError();

  ior->boundary = ptr_disp(ior->pool, ior->allocated);
  return MDBX_SUCCESS;
}

static inline size_t ior_offset(const ior_item_t *item) {
  return item->ov.Offset |
         (size_t)((sizeof(size_t) > sizeof(item->ov.Offset)) ? (uint64_t)item->ov.OffsetHigh << 32 : 0);
}

static inline ior_item_t *ior_next(ior_item_t *item, size_t sgvcnt) {
#if defined(ior_sgv_element)
  ASSERT(sgvcnt > 0);
  return (ior_item_t *)ptr_disp(item, sizeof(ior_item_t) - sizeof(ior_sgv_element) + sizeof(ior_sgv_element) * sgvcnt);
#else
  ASSERT(sgvcnt == 1);
  (void)sgvcnt;
  return item + 1;
#endif
}

int osal_ioring_add(osal_ioring_t *ior, const size_t offset, void *data, const size_t bytes) {
  ASSERT(bytes && data);
  ASSERT(bytes % MDBX_MIN_PAGESIZE == 0 && bytes <= MAX_IO_BYTES);
  ASSERT(offset % MDBX_MIN_PAGESIZE == 0 && offset + (uint64_t)bytes <= MAX_MAPSIZE);

  const unsigned segments = (unsigned)(bytes >> ior->pagesize_ln2);
  const bool use_gather = ior->direct && ior->overlapped_fd && ior->slots_left >= segments;

  ior_item_t *item = ior->pool;
  if (likely(ior->last)) {
    item = ior->last;
    if (unlikely(ior_offset(item) + ior_last_bytes(ior, item) == offset) &&
        likely(ior_last_bytes(ior, item) + bytes <= MAX_IO_BYTES)) {
      if (use_gather &&
          ((bytes | (uintptr_t)data | ior->last_bytes | (uintptr_t)(uint64_t)item->sgv[0].Buffer) &
           ior_alignment_mask) == 0 &&
          (item->single.iov_len & ior_WriteFile_flag) == 0 && ior->last_sgvcnt + (size_t)segments < OSAL_IOV_MAX) {
        ASSERT(ior->overlapped_fd);
        ASSERT((item->single.iov_len & ior_WriteFile_flag) == 0);
        ASSERT(item->sgv[ior->last_sgvcnt].Buffer == 0);
        ior->last_bytes += bytes;
        size_t i = 0;
        do {
          item->sgv[ior->last_sgvcnt + i].Buffer = PtrToPtr64(data);
          data = ptr_disp(data, ior->pagesize);
        } while (++i < segments);
        ior->slots_left -= segments;
        item->sgv[ior->last_sgvcnt += segments].Buffer = 0;
        ASSERT((item->single.iov_len & ior_WriteFile_flag) == 0);
        return MDBX_SUCCESS;
      }
      const void *end = ptr_disp(item->single.iov_base, item->single.iov_len - ior_WriteFile_flag);
      if (unlikely(end == data)) {
        ASSERT((item->single.iov_len & ior_WriteFile_flag) != 0);
        item->single.iov_len += bytes;
        ior->last_bytes += bytes;
        return MDBX_SUCCESS;
      }
    }
    item = ior_next(item, ior_last_sgvcnt(ior, item));
  }

  if (unlikely(ior->slots_left < 1))
    return MDBX_RESULT_TRUE;

  unsigned slots_used = 1;
  item->ov.Internal = item->ov.InternalHigh = 0;
  item->ov.Offset = (DWORD)offset;
  item->ov.OffsetHigh = HIGH_DWORD(offset);
  item->ov.hEvent = 0;
  if (!use_gather || ((bytes | (uintptr_t)(data)) & ior_alignment_mask) != 0 || segments > OSAL_IOV_MAX) {
    /* WriteFile()
     * Encoding contract for later decoding in osal_ioring_walk()/osal_ioring_write():
     *   single.iov_len = payload_bytes + ior_WriteFile_flag.
     * The low bit marks "single-buffer WriteFile item". */
    item->single.iov_base = data;
    item->single.iov_len = bytes + ior_WriteFile_flag;
    ASSERT((item->single.iov_len & ior_WriteFile_flag) != 0);
  } else {
    /* WriteFileGather()
     * Deliberately leave single.iov_len == 0 (item is zero-initialized).
     * Decoders subtract ior_WriteFile_flag from iov_len and test the low bit:
     *   0 - 1 => SIZE_MAX, low bit set, therefore recognized as gather item. */
    ASSERT(ior->overlapped_fd);
    item->sgv[0].Buffer = PtrToPtr64(data);
    for (size_t i = 1; i < segments; ++i) {
      data = ptr_disp(data, ior->pagesize);
      item->sgv[i].Buffer = PtrToPtr64(data);
    }
    item->sgv[slots_used = segments].Buffer = 0;
    ASSERT((item->single.iov_len & ior_WriteFile_flag) == 0);
  }
  ior->last_bytes = bytes;
  ior_last_sgvcnt(ior, item) = slots_used;
  ior->slots_left -= slots_used;
  ior->last = item;
  return MDBX_SUCCESS;
}

void osal_ioring_walk(osal_ioring_t *ior, iov_ctx_t *ctx,
                      void (*callback)(iov_ctx_t *ctx, size_t offset, void *data, size_t bytes)) {
  for (ior_item_t *item = ior->pool; item <= ior->last;) {
    size_t offset = ior_offset(item);
    char *data = item->single.iov_base;
    /* Decode counterpart of enqueue-time encoding:
     *   WriteFile item:      iov_len = payload + flag, so bytes = payload.
     *   WriteFileGather item iov_len = 0, so bytes = (size_t)-1 (intentional underflow).
     * We use only the low-bit test below to detect gather items, then overwrite bytes
     * before any callback/use, so the transient SIZE_MAX value is safe. */
    size_t bytes = item->single.iov_len - ior_WriteFile_flag;
    size_t i = 1;
    if (bytes & ior_WriteFile_flag) {
      data = Ptr64ToPtr(item->sgv[0].Buffer);
      bytes = ior->pagesize;
      /* Zap: Reading invalid data from 'item->sgv' */
      MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6385);
      while (item->sgv[i].Buffer) {
        if (data + ior->pagesize != item->sgv[i].Buffer) {
          callback(ctx, offset, data, bytes);
          offset += bytes;
          data = Ptr64ToPtr(item->sgv[i].Buffer);
          bytes = 0;
        }
        bytes += ior->pagesize;
        ++i;
      }
    }
    ASSERT(bytes < MAX_IO_BYTES);
    callback(ctx, offset, data, bytes);
    item = ior_next(item, i);
  }
}

osal_ioring_write_result_t osal_ioring_write(osal_ioring_t *ior, mdbx_filehandle_t fd) {
  osal_ioring_write_result_t r = {MDBX_SUCCESS, 0};

  HANDLE *const end_wait_for = ior->event_pool + ior->allocated +
                               /* был выделен один дополнительный элемент для async_done */ 1;
  HANDLE *wait_for = end_wait_for;
  LONG async_started = 0;
  for (ior_item_t *item = ior->pool; item <= ior->last;) {
    item->ov.Internal = STATUS_PENDING;
    size_t i = 1, bytes = item->single.iov_len - ior_WriteFile_flag;
    r.wops += 1;
    if (bytes & ior_WriteFile_flag) {
      ASSERT(ior->overlapped_fd && fd == ior->overlapped_fd);
      bytes = ior->pagesize;
      /* Zap: Reading invalid data from 'item->sgv' */
      MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6385);
      while (item->sgv[i].Buffer) {
        bytes += ior->pagesize;
        ++i;
      }
      ASSERT(bytes < MAX_IO_BYTES);
      item->ov.hEvent = ior_get_event(ior);
      if (unlikely(!item->ov.hEvent)) {
      bailout_geterr:
        r.err = GetLastError();
      bailout_rc:
        ASSERT(r.err != MDBX_SUCCESS);
        CancelIo(fd);
        return r;
      }
      if (WriteFileGather(fd, item->sgv, (DWORD)bytes, nullptr, &item->ov)) {
        ASSERT(item->ov.Internal == 0 && WaitForSingleObject(item->ov.hEvent, 0) == WAIT_OBJECT_0);
        ior_put_event(ior, item->ov.hEvent);
        item->ov.hEvent = 0;
      } else {
        r.err = (int)GetLastError();
        if (unlikely(r.err != ERROR_IO_PENDING)) {
          void *data = Ptr64ToPtr(item->sgv[0].Buffer);
          ERROR("%s: fd %p, item %p (%zu), addr %p pgno %u, bytes %zu,"
                " offset %" PRId64 ", err %d",
                "WriteFileGather", fd, __Wpedantic_format_voidptr(item), item - ior->pool, data, ((page_t *)data)->pgno,
                bytes, item->ov.Offset + ((uint64_t)item->ov.OffsetHigh << 32), r.err);
          goto bailout_rc;
        }
        ASSERT(wait_for > ior->event_pool + ior->event_stack);
        *--wait_for = item->ov.hEvent;
      }
    } else if (fd == ior->overlapped_fd) {
      ASSERT(bytes < MAX_IO_BYTES);
    retry:
      item->ov.hEvent = ior;
      if (WriteFileEx(fd, item->single.iov_base, (DWORD)bytes, &item->ov, ior_wocr)) {
        async_started += 1;
      } else {
        r.err = (int)GetLastError();
        switch (r.err) {
        default:
          ERROR("%s: fd %p, item %p (%zu), addr %p pgno %u, bytes %zu,"
                " offset %" PRId64 ", err %d",
                "WriteFileEx", fd, __Wpedantic_format_voidptr(item), item - ior->pool, item->single.iov_base,
                ((page_t *)item->single.iov_base)->pgno, bytes, item->ov.Offset + ((uint64_t)item->ov.OffsetHigh << 32),
                r.err);
          goto bailout_rc;
        case ERROR_NOT_FOUND:
        case ERROR_USER_MAPPED_FILE:
        case ERROR_LOCK_VIOLATION:
          WARNING("%s: fd %p, item %p (%zu), addr %p pgno %u, bytes %zu,"
                  " offset %" PRId64 ", err %d",
                  "WriteFileEx", fd, __Wpedantic_format_voidptr(item), item - ior->pool, item->single.iov_base,
                  ((page_t *)item->single.iov_base)->pgno, bytes,
                  item->ov.Offset + ((uint64_t)item->ov.OffsetHigh << 32), r.err);
          SleepEx(0, true);
          goto retry;
        case ERROR_INVALID_USER_BUFFER:
        case ERROR_NOT_ENOUGH_MEMORY:
          if (SleepEx(0, true) == WAIT_IO_COMPLETION)
            goto retry;
          goto bailout_rc;
        case ERROR_IO_PENDING:
          async_started += 1;
        }
      }
    } else {
      ASSERT(bytes < MAX_IO_BYTES);
      DWORD written = 0;
      if (!WriteFile(fd, item->single.iov_base, (DWORD)bytes, &written, &item->ov)) {
        r.err = (int)GetLastError();
        ERROR("%s: fd %p, item %p (%zu), addr %p pgno %u, bytes %zu,"
              " offset %" PRId64 ", err %d",
              "WriteFile", fd, __Wpedantic_format_voidptr(item), item - ior->pool, item->single.iov_base,
              ((page_t *)item->single.iov_base)->pgno, bytes, item->ov.Offset + ((uint64_t)item->ov.OffsetHigh << 32),
              r.err);
        goto bailout_rc;
      } else if (unlikely(written != bytes)) {
        r.err = ERROR_WRITE_FAULT;
        goto bailout_rc;
      }
    }
    item = ior_next(item, i);
  }

  ASSERT(ior->async_waiting > ior->async_completed && ior->async_waiting == INT_MAX);
  ior->async_waiting = async_started;
  if (async_started > ior->async_completed) {
    /* Spare slot for async_done is preallocated (end_wait_for =
     * event_pool + allocated + 1); pushing it last keeps it among the first
     * MAXIMUM_WAIT_OBJECTS handles. Without this, a MIXED batch that has no
     * gather events waits on the gather events alone and returns while
     * WriteFileEx APCs are still in flight -> STATUS_PENDING read as an error,
     * ring reset under a live APC (issue #47 defect 2). */
    ASSERT(wait_for > ior->event_pool + ior->event_stack);
    *--wait_for = ior->async_done;
  }

  const size_t pending_count = end_wait_for - wait_for;
  if (pending_count) {
    /* Ждем до MAXIMUM_WAIT_OBJECTS (64) последних хендлов, а после избирательно
     * ждем посредством GetOverlappedResult(), если какие-то более ранние
     * элементы еще не завершены. В целом, так получается меньше системных
     * вызовов, т.е. меньше накладных расходов. Однако, не факт что эта экономия
     * не будет перекрыта неэффективностью реализации
     * WaitForMultipleObjectsEx(), но тогда это проблемы на стороне M$. */
    DWORD madness;
    do
      madness = WaitForMultipleObjectsEx(
          (pending_count < MAXIMUM_WAIT_OBJECTS) ? (DWORD)pending_count : MAXIMUM_WAIT_OBJECTS, wait_for, true,
          /* сутки */ 86400000ul, true);
    while (madness == WAIT_IO_COMPLETION);
    STATIC_ASSERT(WAIT_OBJECT_0 == 0);
    if (/* madness >= WAIT_OBJECT_0 && */
        madness < WAIT_OBJECT_0 + MAXIMUM_WAIT_OBJECTS)
      r.err = MDBX_SUCCESS;
    else if (madness >= WAIT_ABANDONED_0 && madness < WAIT_ABANDONED_0 + MAXIMUM_WAIT_OBJECTS) {
      r.err = ERROR_ABANDONED_WAIT_0;
      goto bailout_rc;
    } else if (madness == WAIT_TIMEOUT) {
      r.err = WAIT_TIMEOUT;
      goto bailout_rc;
    } else {
      r.err = /* madness == WAIT_FAILED */ MDBX_PROBLEM;
      goto bailout_rc;
    }

    ASSERT(ior->async_waiting == ior->async_completed);
    for (ior_item_t *item = ior->pool; item <= ior->last;) {
      size_t i = 1, bytes = item->single.iov_len - ior_WriteFile_flag;
      void *data = item->single.iov_base;
      if (bytes & ior_WriteFile_flag) {
        data = Ptr64ToPtr(item->sgv[0].Buffer);
        bytes = ior->pagesize;
        /* Zap: Reading invalid data from 'item->sgv' */
        MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6385);
        while (item->sgv[i].Buffer) {
          bytes += ior->pagesize;
          ++i;
        }
        if (!HasOverlappedIoCompleted(&item->ov)) {
          DWORD written = 0;
          if (unlikely(!GetOverlappedResult(fd, &item->ov, &written, true))) {
            ERROR("%s: item %p (%zu), addr %p pgno %u, bytes %zu,"
                  " offset %" PRId64 ", err %d",
                  "GetOverlappedResult", __Wpedantic_format_voidptr(item), item - ior->pool, data,
                  ((page_t *)data)->pgno, bytes, item->ov.Offset + ((uint64_t)item->ov.OffsetHigh << 32),
                  (int)GetLastError());
            goto bailout_geterr;
          }
          ASSERT(MDBX_SUCCESS == item->ov.Internal);
          ASSERT(written == item->ov.InternalHigh);
        }
      } else {
        ASSERT(HasOverlappedIoCompleted(&item->ov));
      }
      ASSERT(item->ov.Internal != ERROR_IO_PENDING);
      if (unlikely(item->ov.Internal != MDBX_SUCCESS)) {
        DWORD written = 0;
        r.err = (int)item->ov.Internal;
        if ((r.err & 0x80000000) && GetOverlappedResult(nullptr, &item->ov, &written, true))
          r.err = (int)GetLastError();
        ERROR("%s: item %p (%zu), addr %p pgno %u, bytes %zu,"
              " offset %" PRId64 ", err %d",
              "Result", __Wpedantic_format_voidptr(item), item - ior->pool, data, ((page_t *)data)->pgno, bytes,
              item->ov.Offset + ((uint64_t)item->ov.OffsetHigh << 32), (int)GetLastError());
        goto bailout_rc;
      }
      if (unlikely(item->ov.InternalHigh != bytes)) {
        r.err = ERROR_WRITE_FAULT;
        goto bailout_rc;
      }
      item = ior_next(item, i);
    }
    ASSERT(ior->async_waiting == ior->async_completed);
  } else {
    ASSERT(r.err == MDBX_SUCCESS);
  }
  ASSERT(ior->async_waiting == ior->async_completed);

  return r;
}

void osal_ioring_reset(osal_ioring_t *ior) {
  if (ior->last) {
    for (ior_item_t *item = ior->pool; item <= ior->last;) {
      if (!HasOverlappedIoCompleted(&item->ov)) {
        ASSERT(ior->overlapped_fd);
        CancelIoEx(ior->overlapped_fd, &item->ov);
      }
      if (item->ov.hEvent && item->ov.hEvent != ior)
        ior_put_event(ior, item->ov.hEvent);
      size_t i = 1;
      if ((item->single.iov_len & ior_WriteFile_flag) == 0) {
        /* Zap: Reading invalid data from 'item->sgv' */
        MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6385);
        while (item->sgv[i].Buffer)
          ++i;
      }
      item = ior_next(item, i);
    }
  }
  ior->async_waiting = INT_MAX;
  ior->async_completed = 0;
  ResetEvent(ior->async_done);
  ior->slots_left = ior->allocated;
  ior->last = nullptr;
}

static void ior_cleanup(osal_ioring_t *ior, const size_t since) {
  osal_ioring_reset(ior);
  for (size_t i = since; i < ior->event_stack; ++i) {
    /* Zap: Using uninitialized memory '**ior.event_pool' */
    MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6001);
    CloseHandle(ior->event_pool[i]);
  }
  ior->event_stack = 0;
}

int osal_ioring_resize(osal_ioring_t *ior, size_t items) {
  ASSERT(items > 0 && items < INT_MAX / sizeof(ior_item_t));
  if (ior->state & IOR_STATE_LOCKED)
    return MDBX_SUCCESS;
  const bool useSetFileIoOverlappedRange = ior->overlapped_fd && imports.SetFileIoOverlappedRange && items > 42;
  const size_t ceiling =
      useSetFileIoOverlappedRange ? ((items < 65536 / 2 / sizeof(ior_item_t)) ? 65536 : 65536 * 4) : 1024;
  const size_t bytes = ceil_powerof2(sizeof(ior_item_t) * items, ceiling);
  items = bytes / sizeof(ior_item_t);

  if (items != ior->allocated) {
    ASSERT(items >= osal_ioring_used(ior));
    if (items < ior->allocated)
      ior_cleanup(ior, items);
    void *ptr = osal_realloc(ior->event_pool, (items + /* extra for waiting the async_done */ 1) * sizeof(HANDLE));
    if (unlikely(!ptr))
      return MDBX_ENOMEM;
    ior->event_pool = ptr;

    int err = osal_memalign_alloc(ceiling, bytes, &ptr);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if (ior->pool) {
      memcpy(ptr, ior->pool, ior->allocated * sizeof(ior_item_t));
      osal_memalign_free(ior->pool);
    }
    ior->pool = ptr;

    if (items > ior->allocated)
      memset(ior->pool + ior->allocated, 0, sizeof(ior_item_t) * (items - ior->allocated));
    ior->allocated = (unsigned)items;
    ior->boundary = ptr_disp(ior->pool, ior->allocated);
    if (useSetFileIoOverlappedRange) {
      if (imports.SetFileIoOverlappedRange(ior->overlapped_fd, ptr, (ULONG)bytes))
        ior->state += IOR_STATE_LOCKED;
      else
        return (int)GetLastError();
    }
  }
  return MDBX_SUCCESS;
}

void osal_ioring_destroy(osal_ioring_t *ior) {
  if (ior->allocated)
    ior_cleanup(ior, 0);
  osal_memalign_free(ior->pool);
  osal_free(ior->event_pool);
  CloseHandle(ior->async_done);
  if (ior->overlapped_fd)
    CloseHandle(ior->overlapped_fd);
  memset(ior, 0, sizeof(osal_ioring_t));
}
