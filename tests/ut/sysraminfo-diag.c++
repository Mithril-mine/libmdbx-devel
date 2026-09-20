/// TEMPORARY diagnostic test for the macOS sysraminfo ENOTSUP issue.
/// To be removed after the root cause is confirmed and fixed.

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

TEST(ut_sysraminfo_diag, probe) {
  errno = 0;
#ifdef _SC_AVPHYS_PAGES
  const long avphys = sysconf(_SC_AVPHYS_PAGES);
  std::printf("diag: _SC_AVPHYS_PAGES defined=%ld sysconf=%ld errno=%d (%s)\n", (long)_SC_AVPHYS_PAGES,
              avphys, errno, errno ? std::strerror(errno) : "none");
#else
  std::printf("diag: _SC_AVPHYS_PAGES is NOT defined\n");
#endif

  errno = 0;
#ifdef _SC_PHYS_PAGES
  const long phys = sysconf(_SC_PHYS_PAGES);
  std::printf("diag: _SC_PHYS_PAGES defined=%ld sysconf=%ld errno=%d (%s)\n", (long)_SC_PHYS_PAGES,
              phys, errno, errno ? std::strerror(errno) : "none");
#else
  std::printf("diag: _SC_PHYS_PAGES is NOT defined\n");
#endif

  errno = 0;
  intptr_t page_size = 0, total_pages = 0, avail_pages = 0;
  int rc = mdbx_get_sysraminfo(&page_size, &total_pages, &avail_pages);
  std::printf("diag: mdbx_get_sysraminfo(ps,tot,av) rc=%d ps=%ld tot=%ld av=%ld errno=%d (%s)\n", rc,
              (long)page_size, (long)total_pages, (long)avail_pages, errno,
              errno ? std::strerror(errno) : "none");

  rc = mdbx_get_sysraminfo(nullptr, &total_pages, &avail_pages);
  std::printf("diag: mdbx_get_sysraminfo(_,tot,av) rc=%d tot=%ld av=%ld errno=%d (%s)\n", rc,
              (long)total_pages, (long)avail_pages, errno, errno ? std::strerror(errno) : "none");

  rc = mdbx_get_sysraminfo(&page_size, nullptr, nullptr);
  std::printf("diag: mdbx_get_sysraminfo(ps,_,_) rc=%d ps=%ld errno=%d (%s)\n", rc, (long)page_size,
              errno, errno ? std::strerror(errno) : "none");

  std::fflush(stdout);
}