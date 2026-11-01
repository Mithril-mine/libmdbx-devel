/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file osal-ioring.c (POSIX part) — asynchronous/overlapped IO-queue for
/// file writes. On POSIX this is currently a stub over pwrite/pwritev.

#include "../../internals.h"

/*----------------------------------------------------------------------------*/

#if MDBX_HAVE_PWRITEV
#if defined(_SC_IOV_MAX) || defined(ENABLE_MEMCHECK)
size_t osal_iov_max;
#define OSAL_IOV_MAX osal_iov_max
#else
#define OSAL_IOV_MAX IOV_MAX
#endif
#else
#undef OSAL_IOV_MAX
#endif /* OSAL_IOV_MAX */

int osal_ioring_create(osal_ioring_t *ior) {
  memset(ior, 0, sizeof(osal_ioring_t));

#if MDBX_HAVE_PWRITEV && defined(_SC_IOV_MAX)
  ASSERT(osal_iov_max > 0);
#endif /* MDBX_HAVE_PWRITEV && _SC_IOV_MAX */

  ior->boundary = ptr_disp(ior->pool, ior->allocated);
  return MDBX_SUCCESS;
}

static inline size_t ior_offset(const ior_item_t *item) { return item->offset; }

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

  ior_item_t *item = ior->pool;
  if (likely(ior->last)) {
    item = ior->last;
    if (unlikely(ior_offset(item) + ior_last_bytes(ior, item) == offset) &&
        likely(ior_last_bytes(ior, item) + bytes <= MAX_IO_BYTES)) {
#if MDBX_HAVE_PWRITEV
      ASSERT((int)item->sgvcnt > 0);
      const void *end = ptr_disp(item->sgv[item->sgvcnt - 1].iov_base, item->sgv[item->sgvcnt - 1].iov_len);
      if (unlikely(end == data)) {
        item->sgv[item->sgvcnt - 1].iov_len += bytes;
        ior->last_bytes += bytes;
        return MDBX_SUCCESS;
      }
      if (likely(item->sgvcnt < OSAL_IOV_MAX)) {
        if (unlikely(ior->slots_left < 1))
          return MDBX_RESULT_TRUE;
        item->sgv[item->sgvcnt].iov_base = data;
        item->sgv[item->sgvcnt].iov_len = bytes;
        ior->last_bytes += bytes;
        item->sgvcnt += 1;
        ior->slots_left -= 1;
        return MDBX_SUCCESS;
      }
#else
      const void *end = ptr_disp(item->single.iov_base, item->single.iov_len);
      if (unlikely(end == data)) {
        item->single.iov_len += bytes;
        return MDBX_SUCCESS;
      }
#endif
    }
    item = ior_next(item, ior_last_sgvcnt(ior, item));
  }

  if (unlikely(ior->slots_left < 1))
    return MDBX_RESULT_TRUE;

  unsigned slots_used = 1;
#if MDBX_HAVE_PWRITEV
  item->offset = offset;
  item->sgv[0].iov_base = data;
  item->sgv[0].iov_len = bytes;
  ior->last_bytes = bytes;
  ior_last_sgvcnt(ior, item) = slots_used;
#else
  item->offset = offset;
  item->single.iov_base = data;
  item->single.iov_len = bytes;
#endif /* !Windows */
  ior->slots_left -= slots_used;
  ior->last = item;
  return MDBX_SUCCESS;
}

void osal_ioring_walk(osal_ioring_t *ior, iov_ctx_t *ctx,
                      void (*callback)(iov_ctx_t *ctx, size_t offset, void *data, size_t bytes)) {
  for (ior_item_t *item = ior->pool; item <= ior->last;) {
#if MDBX_HAVE_PWRITEV
    ASSERT(item->sgvcnt > 0);
    size_t offset = item->offset;
    size_t i = 0;
    do {
      callback(ctx, offset, item->sgv[i].iov_base, item->sgv[i].iov_len);
      offset += item->sgv[i].iov_len;
    } while (++i != item->sgvcnt);
#else
    const size_t i = 1;
    callback(ctx, item->offset, item->single.iov_base, item->single.iov_len);
#endif
    item = ior_next(item, i);
  }
}

osal_ioring_write_result_t osal_ioring_write(osal_ioring_t *ior, mdbx_filehandle_t fd) {
  osal_ioring_write_result_t r = {MDBX_SUCCESS, 0};

  STATIC_ASSERT_MSG(sizeof(off_t) >= sizeof(size_t), "libmdbx requires 64-bit file I/O on 64-bit systems");
  for (ior_item_t *item = ior->pool; item <= ior->last;) {
#if MDBX_HAVE_PWRITEV
    ASSERT(item->sgvcnt > 0);
    if (item->sgvcnt == 1)
      r.err = osal_pwrite(fd, item->sgv[0].iov_base, item->sgv[0].iov_len, item->offset);
    else
      r.err = osal_pwritev(fd, item->sgv, item->sgvcnt, item->offset);

    // TODO: io_uring_prep_write(sqe, fd, ...);

    item = ior_next(item, item->sgvcnt);
#else
    r.err = osal_pwrite(fd, item->single.iov_base, item->single.iov_len, item->offset);
    item = ior_next(item, 1);
#endif
    r.wops += 1;
    if (unlikely(r.err != MDBX_SUCCESS))
      break;
  }

  // TODO: io_uring_submit(&ring)
  // TODO: err = io_uring_wait_cqe(&ring, &cqe);
  // TODO: io_uring_cqe_seen(&ring, cqe);

  return r;
}

void osal_ioring_reset(osal_ioring_t *ior) {
  ior->slots_left = ior->allocated;
  ior->last = nullptr;
}

static void ior_cleanup(osal_ioring_t *ior, const size_t since) {
  osal_ioring_reset(ior);
  (void)since;
}

int osal_ioring_resize(osal_ioring_t *ior, size_t items) {
  ASSERT(items > 0 && items < INT_MAX / sizeof(ior_item_t));

  if (items != ior->allocated) {
    ASSERT(items >= osal_ioring_used(ior));
    if (items < ior->allocated)
      ior_cleanup(ior, items);

    void *ptr = osal_realloc(ior->pool, sizeof(ior_item_t) * items);
    if (unlikely(!ptr))
      return MDBX_ENOMEM;

    ior->pool = ptr;

    if (items > ior->allocated)
      memset(ior->pool + ior->allocated, 0, sizeof(ior_item_t) * (items - ior->allocated));
    ior->allocated = (unsigned)items;
    ior->boundary = ptr_disp(ior->pool, ior->allocated);
  }
  return MDBX_SUCCESS;
}

void osal_ioring_destroy(osal_ioring_t *ior) {
  if (ior->allocated)
    ior_cleanup(ior, 0);
  osal_free(ior->pool);
  memset(ior, 0, sizeof(osal_ioring_t));
}
