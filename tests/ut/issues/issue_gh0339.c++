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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "mdbx.h"
#include <gtest/gtest.h>

#include <unistd.h>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/339
// On Linux mdbx_get_sysraminfo() ignored sysinfo.mem_unit (sizes are in mem_unit
// units, not bytes, which is greater than 1 exactly when the value does not fit
// 32 bits), shifted size_t values left in a way that wraps for 32-bit processes
// above 4 GiB, and its /proc/meminfo parsing passed the very buffer being parsed
// to sscanf() as the %s destination (undefined behavior, C11 7.21.6.7).
//
// The mem_unit defect is only observable when mem_unit > 1, i.e. in a 32-bit
// process on a host with 4 GiB of RAM or more; the cross-check below still
// catches it in such environments, while on usual 64-bit hosts it verifies
// the general sanity of the reported values.

namespace {

constexpr long kTenPercent = 10;

} // namespace

TEST(issue_gh0339, sanity) {
  intptr_t page_size = -1, total_pages = -1, avail_pages = -1;
  ASSERT_EQ(mdbx_get_sysraminfo(&page_size, &total_pages, &avail_pages), MDBX_SUCCESS);
  EXPECT_GT(page_size, 0);
  EXPECT_EQ(page_size, sysconf(_SC_PAGESIZE));
  EXPECT_GT(total_pages, 0);
  EXPECT_GT(avail_pages, 0);
  EXPECT_LE(avail_pages, total_pages);
}

TEST(issue_gh0339, partial_outputs) {
  intptr_t page_size = -1, total_pages = -1, avail_pages = -1;
  ASSERT_EQ(mdbx_get_sysraminfo(&page_size, nullptr, nullptr), MDBX_SUCCESS);
  EXPECT_GT(page_size, 0);
  ASSERT_EQ(mdbx_get_sysraminfo(nullptr, &total_pages, nullptr), MDBX_SUCCESS);
  EXPECT_GT(total_pages, 0);
  ASSERT_EQ(mdbx_get_sysraminfo(nullptr, nullptr, &avail_pages), MDBX_SUCCESS);
  EXPECT_GT(avail_pages, 0);
  EXPECT_EQ(mdbx_get_sysraminfo(nullptr, nullptr, nullptr), MDBX_EINVAL);
}

/* Cross-check the total against sysconf(_SC_PHYS_PAGES), which glibc derives
   from /proc/meminfo independently of sysinfo(). Not all POSIX systems provide
   _SC_PHYS_PAGES (e.g. macOS does not). */
#if defined(_SC_PHYS_PAGES)
TEST(issue_gh0339, total_crosscheck) {
  const long phys_pages = sysconf(_SC_PHYS_PAGES);
  ASSERT_GT(phys_pages, 0);
  intptr_t page_size = -1, total_pages = -1;
  ASSERT_EQ(mdbx_get_sysraminfo(&page_size, &total_pages, nullptr), MDBX_SUCCESS);
  const intptr_t expected = (intptr_t)(((uint64_t)phys_pages * (uint64_t)sysconf(_SC_PAGESIZE)) / (uint64_t)page_size);
  const intptr_t tolerance = expected / kTenPercent;
  EXPECT_NEAR(total_pages, expected, tolerance) << "mem_unit ignored or a shift wrapped";
}
#endif /* _SC_PHYS_PAGES definition available */
