/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", WITHOUT WARRANTY OF ANY KIND, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Weak anchor definitions for the white-box units that compile library
 * sources directly into the test (the details_rkl.c pattern).
 *
 * The library is built as ONE translation unit (src/alloy.c), where the
 * optimizer inlines the standalone definitions of many internal
 * (hidden-visibility) functions and drops them - and on several linkers
 * (the macOS ld, lld-link, whole-program LTO configurations) the surviving
 * hidden globals are not resolvable from another object either. An included
 * source references those symbols from its unused parts too, so a unit that
 * needs only a few functions of the source cannot link without anchors.
 *
 * Every anchor is WEAK: a strong definition from a library that still
 * provides one always wins the link, and where none does the anchor only
 * satisfies a reference from code the test never executes - the anchored
 * functions are never actually called. */

#pragma once

/* included after the library source(s), so all prototypes are known */

#if defined(__GNUC__) || defined(__clang__)

MDBX_MAYBE_UNUSED __attribute__((weak)) int osal_filesize(mdbx_filehandle_t fd, uint64_t *length) {
  (void)fd;
  (void)length;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED __attribute__((weak)) int osal_pwrite(mdbx_filehandle_t fd, const void *buf, size_t bytes,
                                                        uint64_t offset) {
  (void)fd;
  (void)buf;
  (void)bytes;
  (void)offset;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED __attribute__((weak)) bin128_t osal_guid(const MDBX_env *env) {
  (void)env;
  bin128_t zero;
  static const uint64_t nil[2] = {0, 0};
  memcpy(&zero, nil, sizeof zero);
  return zero;
}

MDBX_MAYBE_UNUSED __attribute__((weak)) int dxb_fsync(const MDBX_env *env, enum osal_syncmode_bits mode_bits) {
  (void)env;
  (void)mode_bits;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED __attribute__((weak)) int dxb_msync(const MDBX_env *env, size_t length_pages,
                                                      enum osal_syncmode_bits mode_bits) {
  (void)env;
  (void)length_pages;
  (void)mode_bits;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED __attribute__((weak)) bool coherency_check_meta(const MDBX_env *env, const volatile meta_t *meta,
                                                                  bool report) {
  (void)env;
  (void)meta;
  (void)report;
  return true;
}

#endif /* GNUC/clang */
