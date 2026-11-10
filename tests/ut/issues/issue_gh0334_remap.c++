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

#include "../../../src/internals.h"
#include "../env-guard.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/334 (read-write part)
//
// When osal_mresize() (POSIX) could not map the bigger range and restored the old mapping, it recorded the new size
// although the mapping kept the old limit. The next shrink then discarded memory beyond the end of the mapping with
// madvise(), which may belong to the C library, the heap or another mapping (Debug builds abort on the size/limit
// assertion of dxb_resize()).
//
// Growing past the limit reaches the restore only when mremap() fails other than for lack of space (EFAULT when the
// mapping spans several areas), appending to the mapping fails (the address after it is busy) and mapping the whole
// range fails (ENOMEM). The FAULT sites mresize_mremap_errno (Linux only) and mresize_mmap_errno simulate the two
// syscall failures; the test keeps the address after the mapping busy with its own mapping, filled with a pattern.
// After the failed growth the recorded size must not exceed the restored mapping (checked first, so a defective build
// stops before its shrink could discard unknown memory), and the shrink must leave the pattern intact. Requires
// MDBX_PROBES=ON and MDBX_PROBES=1; skips on Windows, which has its own osal_mresize().

namespace {

const char *const kPath = "issue334-remap.mdbx";

#if !defined(_WIN32)
constexpr intptr_t kMiB = intptr_t(1) << 20;
const char *const kSites[] = {"mresize_mremap_errno", "mresize_mmap_errno"};

/* Disarms the FAULT sites on every exit path. */
struct faults_guard {
  ~faults_guard() {
    char reply[256];
    for (const char *site : kSites) {
      const std::string request = std::string("fault ") + site + " none";
      mdbx_probe_ctl(request.c_str(), reply, sizeof reply);
    }
  }
};

size_t site_hits(const char *site) {
  char reply[512];
  const std::string request = std::string("query ") + site;
  if (mdbx_probe_ctl(request.c_str(), reply, sizeof reply) != 0)
    return 0;
  const char *const line = strstr(reply, "site ");
  char name[64];
  unsigned kind, armed;
  size_t seen, hits;
  return (line && sscanf(line, "site %63s %u %u %zu %zu", name, &kind, &armed, &seen, &hits) == 5) ? hits : 0;
}

/* Maps memory right after the mapping of the environment and fills it with a pattern, until destruction. When the
 * address is in use by something else, which keeps the append failing as well, nothing is mapped. */
class neighbour_mapping {
  unsigned char *addr = nullptr;
  size_t len = 0;

public:
  static constexpr unsigned char kPattern = 0xAB;

  neighbour_mapping(const MDBX_env *env, size_t length) : len(length) {
    void *const want = static_cast<char *>(env->dxb_mmap.base) + env->dxb_mmap.limit;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
    void *const got = mmap(want, len, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (got == want) {
      addr = static_cast<unsigned char *>(got);
      std::memset(addr, kPattern, len);
    } else if (got != MAP_FAILED)
      munmap(got, len);
  }
  neighbour_mapping(const neighbour_mapping &) = delete;
  neighbour_mapping &operator=(const neighbour_mapping &) = delete;
  ~neighbour_mapping() {
    if (addr)
      munmap(addr, len);
  }

  bool placed() const { return addr != nullptr; }
  size_t changed_bytes() const {
    size_t changed = 0;
    for (size_t i = 0; i < len; ++i)
      changed += addr[i] != kPattern;
    return changed;
  }
};
#endif /* !_WIN32 */

} // namespace

TEST(issue_gh0334_remap, failed_remap_keeps_size_within_mapping) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
#if defined(_WIN32)
  GTEST_SKIP() << "Windows has its own osal_mresize() without these syscalls";
#else
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  /* an explicit lower bound, since the default one is 4 MiB with 64 KiB pages and would block the shrink below */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, kMiB, 4 * kMiB, 8 * kMiB, kMiB, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPath, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644));
  {
    MDBX_txn *txn = nullptr;
    txn_handle_guard txn_owner(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_val k = {const_cast<char *>("key"), 3}, v = {const_cast<char *>("value"), 5};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, MAIN_DBI, &k, &v, MDBX_UPSERT));
    txn_owner.release();
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  const neighbour_mapping neighbour(env, 8 * kMiB);
  {
    const faults_guard faults;
    char reply[256];
    for (const char *site : kSites) {
      const std::string reset = std::string("reset ") + site;
      ASSERT_EQ(0, mdbx_probe_ctl(reset.c_str(), reply, sizeof reply)) << reply;
    }
    const std::string efault = "fault mresize_mremap_errno " + std::to_string(EFAULT);
    const std::string enomem = "fault mresize_mmap_errno " + std::to_string(ENOMEM);
    ASSERT_EQ(0, mdbx_probe_ctl(efault.c_str(), reply, sizeof reply)) << reply;
    ASSERT_EQ(0, mdbx_probe_ctl(enomem.c_str(), reply, sizeof reply)) << reply;
    /* grow past the limit: mremap() fails, the append fails at the busy address, mapping the whole range fails,
     * and the old mapping is restored */
    const int rc = mdbx_env_set_geometry(env, -1, 16 * kMiB, 32 * kMiB, -1, -1, -1);
    EXPECT_EQ(MDBX_UNABLE_EXTEND_MAPSIZE, rc) << mdbx_strerror(rc);
  }
  ASSERT_GT(site_hits("mresize_mmap_errno"), size_t(0)) << "the whole-range remap was not reached";
  ASSERT_LE(env->dxb_mmap.current, env->dxb_mmap.limit) << "the recorded size exceeds the restored mapping";

  /* the shrink discards pages from the recorded size down to the new one */
  const int rc = mdbx_env_set_geometry(env, -1, 2 * kMiB, 8 * kMiB, -1, -1, -1);
  EXPECT_EQ(MDBX_SUCCESS, rc) << mdbx_strerror(rc);
  EXPECT_LE(env->dxb_mmap.current, env->dxb_mmap.limit);
  if (neighbour.placed()) {
    EXPECT_EQ(size_t(0), neighbour.changed_bytes()) << "the shrink discarded memory after the mapping";
  }

  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_val k = {const_cast<char *>("key"), 3}, v;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_get(txn, MAIN_DBI, &k, &v));
  EXPECT_EQ(0, std::memcmp(v.iov_base, "value", 5));
#endif /* !_WIN32 */
}
