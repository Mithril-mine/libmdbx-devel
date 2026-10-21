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

/* nested child spills, then is aborted -> spilled.list leaks. */
#include "mdbx.h++"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

#include "../test-env.h"

TEST(issue_gh0010, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "issue10";
  mdbx::env::remove(testdb);
  mdbx::env_managed::create_parameters cp;
  cp.geometry = mdbx::env::geometry(mdbx::env::geometry::minimal_value, mdbx::env::geometry::default_value,
                                    mdbx::env::geometry::default_value, mdbx::env::geometry::default_value,
                                    mdbx::env::geometry::default_value, mdbx::env::geometry::minimal_value);
  mdbx::env_managed::operate_parameters op;
  op.mode = mdbx::env_managed::mode::nested_transactions;
  op.max_maps = 42;
  mdbx::env_managed env(testdb, cp, op);

  env.set_extra_option(mdbx::env::extra_runtime_option::dp_limit, 128) /* must be >= CURSOR_STACK_SIZE*4 */;
  auto t = env.start_write();
  auto d = t.create_map("dbi");

  char v[4096], k[32];
  memset(v, 7, 4096);
  auto c = t.start_nested();
  for (size_t i = 0; i < 2000; i++) { /* child spills */
    mdbx::slice K(k, snprintf(k, 32, "k%08zd", i));
    mdbx::slice V(v, 1200u + (i % 1500u));
    c.put(d, K, V, mdbx::upsert);
  }
  c.abort();
}
