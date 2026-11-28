/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025-2026

/* Probe-bus self-test: exercises the mdbx-probes mechanism end-to-end without
 * relying on the engine internals - sites placed in this very test through
 * the MDBX_TST_MPROBE_* fire macros (tests/api.h), driven via the exported
 * mdbx_probe_ctl() control API through the testing API seam. Requires
 * MDBX_PROBES=ON at configure time; the probe machinery itself is activated
 * by setting MDBX_PROBES=1 in the environment before the first probe fires
 * (a GoogleTest unit per tests/AGENTS.md §12, issue #519 stage 6). */

#include "../test-env.h++"

#include "mdbx.h"
#include <gtest/gtest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*-----------------------------------------------------------------------------*/

namespace {

void do_ctl(const char *request, char *reply, size_t size) { EXPECT_EQ(0, mdbx_tst_probe_ctl(request, reply, size)); }

void fire_sites(void) {
  MDBX_TST_MPROBE_COLLECT(test_counter, 42);
  MDBX_TST_MPROBE_WATCH(test_event, 7);
  int err = MDBX_SUCCESS;
  MDBX_TST_MPROBE_FAULT(test_fault, err);
}

} /* namespace */

TEST(probes, bus_end_to_end) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* Activation is env-driven; the CI cell sets MDBX_PROBES=1. */
  const char *activated = getenv("MDBX_PROBES");
  ASSERT_NE(nullptr, activated) << "the probe-bus requires MDBX_PROBES=1 in the environment";
  ASSERT_EQ('1', activated[0]) << "the probe-bus requires MDBX_PROBES=1 in the environment";

  char reply[1024];

  /* 1. Firing before any control: sites register lazily, counters accumulate. */
  fire_sites();
  fire_sites();

  do_ctl("list", reply, sizeof(reply));
  EXPECT_NE(strstr(reply, "test_counter"), nullptr);
  EXPECT_NE(strstr(reply, "test_event"), nullptr);
  EXPECT_NE(strstr(reply, "test_fault"), nullptr);

  /* 2. query reports seen/hits/suppressed/armed. */
  do_ctl("query test_*", reply, sizeof(reply));
  EXPECT_NE(strstr(reply, "site test_counter"), nullptr);
  EXPECT_NE(strstr(reply, " 2 "), nullptr); /* kind=collect */

  /* 3. reset zeroes counters, then re-fire. */
  do_ctl("reset test_*", reply, sizeof(reply));
  do_ctl("query test_counter", reply, sizeof(reply));
  fire_sites();
  do_ctl("query test_counter", reply, sizeof(reply));
  /* format: site <name> <kind> <armed> <seen> <hits> <suppressed> <value> <file>:<line> */

  /* 4. disarm suppresses assert/fault behavior but keeps observation. */
  do_ctl("disarm test_event", reply, sizeof(reply));
  fire_sites();
  do_ctl("query test_event", reply, sizeof(reply));

  /* 5. fault injection: arm a return-code mutation at the FAULT site. */
  do_ctl("fault test_fault -30798", reply, sizeof(reply));
  int injected = MDBX_SUCCESS;
  {
    MDBX_tst_mprobe_site_t site = {"test_fault", __FILE__, __LINE__, MDBX_TST_MPROBE_KIND_FAULT, 0};
    mdbx_tst_probe_fault(&site, &injected);
  }
  EXPECT_EQ(-30798, injected);

  /* 6. allocation fault: force next N allocations to fail. */
  do_ctl("alloc-fault 1", reply, sizeof(reply));
  {
    void *p = mdbx_tst_mprobe_alloc(64, "test");
    EXPECT_EQ(nullptr, p);
    p = mdbx_tst_mprobe_alloc(64, "test");
    EXPECT_NE(nullptr, p);
    free(p);
  }

  /* 7. mode count: a failing DEV_ASSERT records but does not abort. */
  do_ctl("mode count", reply, sizeof(reply));
  {
    MDBX_tst_mprobe_site_t site = {"test_assert_site", __FILE__, __LINE__, MDBX_TST_MPROBE_KIND_ASSERT, 0};
    mdbx_tst_probe_assert_failed(&site, "deliberate");
  }
  do_ctl("mode panic", reply, sizeof(reply));
  do_ctl("query test_assert_site", reply, sizeof(reply));
  EXPECT_NE(strstr(reply, "site test_assert_site"), nullptr);
}
