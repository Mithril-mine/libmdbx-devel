/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <dlfcn.h>

/* Regression for the defect V-076 of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * mdbx_txn_release_all_cursors() — the legacy two-argument
 * LIBMDBX_INLINE_API wrapper — was the only one of the 27 inline APIs
 * without an exported non-inline fallback in api-cold.c, so the symbol
 * was missing from the shared library and applications linking it
 * non-inline had nothing to bind against. The dynamic symbol must be
 * present; on the unfixed library dlsym() returns NULL.
 *
 * The shared library path comes through the environment (the MDBX_LIB_PATH
 * target property), so the check works no matter how the test binary itself
 * links the library. */

TEST(issue_gh0485, release_cursors_exported) {
  const char *lib_path = std::getenv("MDBX_LIB_PATH");
  ASSERT_NE(nullptr, lib_path) << "MDBX_LIB_PATH must point to the shared library";
  void *handle = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(nullptr, handle) << "dlopen(" << lib_path << "): " << dlerror();
  void *legacy = dlsym(handle, "mdbx_txn_release_all_cursors");
  void *extended = dlsym(handle, "mdbx_txn_release_all_cursors_ex");
  dlclose(handle);
  EXPECT_NE(nullptr, legacy) << "the legacy inline API must have an exported fallback";
  EXPECT_NE(nullptr, extended) << "sanity: the _ex variant must be exported as well";
}
