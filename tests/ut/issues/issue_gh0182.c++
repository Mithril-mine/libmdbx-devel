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

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
const char *const kTestPath = "issue182.mdbx";
constexpr uint32_t kPagesize = 4096;
constexpr uint16_t kMetaFlag = 0x08;   /* P_META */
constexpr size_t kPageHdrSize = 20;    /* PAGEHDRSZ */
constexpr size_t kMetaBootidOff = 192; /* offsetof(meta_t, bootid) within pack(1) meta */

uint16_t read_u16_le(const uint8_t *p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }

std::vector<uint8_t> read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return data;
}

/* Create a database with a durable checkpoint followed by a weak checkpoint of
 * the current boot, exactly like the issue #182 reproducer. Returns the bootid
 * found in the meta-pages. */
std::array<uint8_t, 16> make_weak_checkpoint_db() {
  mdbx::env::remove(kTestPath);

  MDBX_env *env = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, kPagesize));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR | MDBX_EXCLUSIVE, 0600));
  if (!env)
    return {};

  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
  {
    MDBX_val key = {(void *)"durable", 7}, value = {(void *)"saved", 5};
    EXPECT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT));
  }
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, true));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_writethrough_threshold, 0));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  {
    MDBX_val key = {(void *)"weak", 4}, value = {(void *)"saved", 5};
    EXPECT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT));
  }
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_close_ex(env, true));

  /* Read the meta-pages and remember the bootid (16 bytes, repeated in all
   * three metas) so a crafted "other boot" database can override it. */
  const auto buf = read_file(kTestPath);
  if (buf.size() < size_t(kPagesize) * 3)
    return {};

  std::array<uint8_t, 16> boot = {};
  for (unsigned i = 0; i < 3; ++i) {
    const size_t off = size_t(kPagesize) * i;
    if ((read_u16_le(&buf[off + 10]) & kMetaFlag) == 0)
      continue;
    const uint16_t ps = read_u16_le(&buf[off + kPageHdrSize + 44]); /* meta.pagesize */
    if (ps != kPagesize)
      continue;
    memcpy(boot.data(), &buf[off + kPageHdrSize + kMetaBootidOff], 16);
    break;
  }
  if (boot == std::array<uint8_t, 16>{})
    return {};
  return boot;
}

/* Rewrites the boot id in all three meta pages of the (closed) database. */
void rewrite_boot() {
  std::fstream f(kTestPath, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(f.is_open());
  f.seekg(0, std::ios::end);
  const std::streamoff end = f.tellg();
  ASSERT_GT(end, 0);
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf(size_t(end), 0);
  f.read(reinterpret_cast<char *>(buf.data()), std::streamsize(buf.size()));
  ASSERT_EQ(f.gcount(), std::streamsize(buf.size()));

  size_t found = 0;
  for (unsigned i = 0; i < 3; ++i) {
    const size_t off = size_t(kPagesize) * i;
    if ((read_u16_le(&buf[off + 10]) & kMetaFlag) == 0)
      continue;
    const uint16_t ps = read_u16_le(&buf[off + kPageHdrSize + 44]);
    if (ps != kPagesize)
      continue;
    buf[off + kPageHdrSize + kMetaBootidOff] ^= 0x01; /* flip the first byte => a different boot id */
    ++found;
  }
  ASSERT_EQ(3u, found) << "boot id must be rewritten in all three meta pages";

  f.seekp(0, std::ios::beg);
  f.write(reinterpret_cast<const char *>(buf.data()), std::streamsize(buf.size()));
  f.close();
}

} // namespace

TEST(issue_gh0182, preopen_modes_weak_checkpoint) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  const auto boot = make_weak_checkpoint_db();
  if (boot == std::array<uint8_t, 16>{}) {
    GTEST_SKIP() << "no usable boot id on this system";
    return;
  }

  /* The database holds a durable checkpoint followed by a weak checkpoint of
   * the current boot (troika s:w:w). Each preopen mode must handle this
   * according to its contract:
   *  - LEGACY:       MDBX_CORRUPTED (the newest checkpoint is not steady);
   *  - PROPERLY:     MDBX_WANNA_RECOVERY (default mode of mdbx_preopen_snapinfo);
   *  - PESSIMISTIC:  MDBX_SUCCESS (newest steady checkpoint geometry);
   *  - OPTIMISTIC:   MDBX_SUCCESS (same-boot weak treated as steady, newest one). */
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_CORRUPTED, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_LEGACY, &info, sizeof(info)));

  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_WANNA_RECOVERY, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_PROPERLY, &info, sizeof(info)));

  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_PESSIMISTIC, &info, sizeof(info)));
  EXPECT_EQ(kPagesize, info.mi_dxb_pagesize);
  EXPECT_GT(info.mi_recent_txnid, 0u) << "the steady checkpoint txid must be reported";

  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_OPTIMISTIC, &info, sizeof(info)));
  EXPECT_EQ(kPagesize, info.mi_dxb_pagesize);
  EXPECT_GT(info.mi_recent_txnid, 0u) << "the weak same-boot checkpoint txid must be reported";
}

TEST(issue_gh0182, readonly_open_weak_checkpoint_reports_recovery_needed) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  auto boot = make_weak_checkpoint_db();
  if (boot == std::array<uint8_t, 16>{}) {
    GTEST_SKIP() << "no usable boot id on this system";
    return;
  }

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const int rc = mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0);
  if (env)
    mdbx_env_close(env);
  EXPECT_EQ(MDBX_WANNA_RECOVERY, rc) << "read-only open must refuse and require recovery";
}

TEST(issue_gh0182, readonly_open_other_boot_rolls_back_and_succeeds) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  auto boot = make_weak_checkpoint_db();
  if (boot == std::array<uint8_t, 16>{}) {
    GTEST_SKIP() << "no usable boot id on this system";
    return;
  }

  /* Simulate a reboot: flip the boot id in the (closed) meta pages, so the
   * weak checkpoint looks like it belongs to a previous boot and must be
   * rolled back instead of recovered/synced. */
  rewrite_boot();

  /* After the simulated reboot all meta-pages carry a foreign boot-id. Preopen
   * modes must then behave as follows:
   *  - LEGACY:    MDBX_CORRUPTED (the newest checkpoint is still not steady);
   *  - PROPERLY:  MDBX_WANNA_RECOVERY (same reason, default mode);
   *  - PESSIMISTIC: MDBX_SUCCESS (the newest steady checkpoint);
   *  - OPTIMISTIC:  MDBX_SUCCESS (a foreign-boot weak is NOT treated as
   *                steady, so the newest steady checkpoint is reported). */
  {
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof(info));
    EXPECT_EQ(MDBX_CORRUPTED, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_LEGACY, &info, sizeof(info)));
    std::memset(&info, 0, sizeof(info));
    EXPECT_EQ(MDBX_WANNA_RECOVERY, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_PROPERLY, &info, sizeof(info)));
    std::memset(&info, 0, sizeof(info));
    EXPECT_EQ(MDBX_SUCCESS, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_PESSIMISTIC, &info, sizeof(info)));
    EXPECT_EQ(kPagesize, info.mi_dxb_pagesize);
    std::memset(&info, 0, sizeof(info));
    EXPECT_EQ(MDBX_SUCCESS, mdbx_preopen_snapinfo_ex(kTestPath, MDBX_PREOPEN_OPTIMISTIC, &info, sizeof(info)));
    EXPECT_EQ(kPagesize, info.mi_dxb_pagesize);
  }

  /* In read-only mode the rollback cannot be performed automatically: the open
   * must refuse and signal that recovery is required. */
  {
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    const int rc = mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0);
    if (env)
      mdbx_env_close(env);
    EXPECT_EQ(MDBX_WANNA_RECOVERY, rc) << "read-only open after a simulated reboot must require recovery";
  }

  /* A writable open with a different boot id must roll back the weak
   * checkpoint automatically (purge to the last steady one). */
  {
    MDBX_env *env = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR, 0644));
    MDBX_txn *rtxn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rtxn));
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(rtxn, nullptr, MDBX_DB_DEFAULTS, &dbi));
    MDBX_val key = {(void *)"durable", 7}, value = {};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(rtxn, dbi, &key, &value));
    EXPECT_EQ((size_t)5, value.iov_len);
    EXPECT_EQ(0, memcmp(value.iov_base, "saved", 5));
    mdbx_txn_abort(rtxn);
    mdbx_env_close(env);
  }

  /* After the automatic rollback the weak checkpoint is gone: a fresh preopen
   * reports success. */
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_preopen_snapinfo(kTestPath, &info, sizeof(info)));
}

TEST(issue_gh0182, writable_open_same_boot_promotes_weak_to_steady) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  auto boot = make_weak_checkpoint_db();
  if (boot == std::array<uint8_t, 16>{}) {
    GTEST_SKIP() << "no usable boot id on this system";
    return;
  }

  /* A writable open with the same boot id must promote the weak checkpoint
   * into a steady one automatically (steady-sync), then a preopen inspection
   * sees a fully steady database. */
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR, 0644));
  mdbx_env_close(env);

  MDBX_envinfo info;
  std::memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_preopen_snapinfo(kTestPath, &info, sizeof(info)));
}