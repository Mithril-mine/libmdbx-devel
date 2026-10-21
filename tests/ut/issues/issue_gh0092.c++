/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/92
// A txn_managed returned by start_nested() keeps its handle after the parent's
// commit()/abort() has already committed or aborted the child and freed it; the
// child's destructor then aborts a freed (often reused) pointer. The C++ layer
// now refuses to commit()/abort() a parent while a child is live, like
// checkpoint() already does.

TEST(issue_gh0092, commit_abort_with_live_child_is_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue92.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(),
                        mdbx::env::operate_parameters(4).nested_transactions(true));

  auto parent = env.start_write();
  auto map = parent.create_map("t");
  auto child = parent.start_nested();
  child.upsert(map, mdbx::slice("k1"), mdbx::slice("from child 1"));

  /* The parent must not commit while the child is owned by a live
   * txn_managed: the C core would commit and free the child, and the child's
   * destructor would then abort the freed handle. */
  EXPECT_THROW(parent.commit(), mdbx::bad_transaction);
  EXPECT_NE(nullptr, parent.handle()) << "the rejected commit must keep the parent handle";
  child.commit();
  parent.commit();

  /* The same for abort. */
  auto parent2 = env.start_write();
  auto child2 = parent2.start_nested();
  child2.upsert(map, mdbx::slice("k2"), mdbx::slice("from child 2"));
  EXPECT_THROW(parent2.abort(), mdbx::bad_transaction);
  EXPECT_NE(nullptr, parent2.handle()) << "the rejected abort must keep the parent handle";
  child2.abort();
  parent2.abort();

  env.close();
  mdbx::env::remove(testdb);
}