#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>
#include <string>

#include "../probe-ctl.h"
#include "../test-env.h"

namespace {

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

struct txn_ptr {
  MDBX_txn *txn = nullptr;
  ~txn_ptr() {
    if (txn)
      mdbx_txn_abort(txn);
  }
  MDBX_txn **operator&() { return &txn; }
  MDBX_txn *get() const { return txn; }
  MDBX_txn *release() {
    MDBX_txn *t = txn;
    txn = nullptr;
    return t;
  }
};

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const char *value) {
  MDBX_val k = {const_cast<void *>(static_cast<const void *>(key)), strlen(key)};
  MDBX_val v = {const_cast<void *>(static_cast<const void *>(value)), strlen(value)};
  ASSERT_EQ(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
}

/* Returns the hits of the probe site, or SIZE_MAX when unavailable. */
/* Returns how many times the probe site has been evaluated ("seen"), or
 * SIZE_MAX when the probe-bus is unavailable. A site that was never
 * evaluated is absent from the reply, i.e. zero. */
size_t site_seen(const char *site) {
  char reply[512];
  if (mdbx_probe_ctl((std::string("query ") + site).c_str(), reply, sizeof reply) != 0)
    return SIZE_MAX;
  const char *const found = strstr(reply, site);
  if (!found)
    return 0;
  unsigned kind, armed;
  size_t seen, hits;
  return (sscanf(found, "%*s %u %u %zu %zu", &kind, &armed, &seen, &hits) == 4) ? seen : SIZE_MAX;
}

} // namespace

/* Regression for https://github.com/Mithril-mine/libmdbx-devel/issues/448:
 * when a nested transaction's begin fails at its first allocation, the
 * never-started nested's undo used to run the geo-undo branch against a
 * zeroed nested->geo (the parent's geometry was snapshotted after the
 * allocation): a spurious dxb_resize() was issued and, when refused with
 * EPERM (regular for Windows with an active reader), the zeroed geometry
 * was promoted into the parent, corrupting it. Now the snapshot happens
 * before the allocation, so the undo branch is not entered at all — which
 * is asserted via the txn_nested_geo_undo probe site, deterministically on
 * every platform. Requires the probe-bus (MDBX_PROBES=ON build and
 * MDBX_PROBES=1 at run time). */
TEST(issue_gh0448, failed_nested_begin_keeps_parent_geometry) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard files({"issue448.mdbx"});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), "issue448.mdbx", MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  txn_ptr parent;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &parent), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(parent.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);

  MDBX_envinfo info_before;
  std::memset(&info_before, 0, sizeof info_before);
  ASSERT_EQ(mdbx_env_info_ex(env.get(), nullptr, &info_before, sizeof info_before), MDBX_SUCCESS);

  /* Fail the nested begin exactly at its first allocation, where nothing
   * has been mutated yet */
  char reply[256];
  ASSERT_EQ(0, mdbx_probe_ctl("reset txn_nested_start_enomem", reply, sizeof reply));
  ASSERT_EQ(0, mdbx_probe_ctl((std::string("fault txn_nested_start_enomem ") + std::to_string(MDBX_ENOMEM)).c_str(),
                              reply, sizeof reply));
  txn_ptr nested;
  const int begin_rc = mdbx_txn_begin(env.get(), parent.get(), MDBX_TXN_READWRITE, &nested);
  ASSERT_EQ(0, mdbx_probe_ctl("fault txn_nested_start_enomem none", reply, sizeof reply));
  ASSERT_EQ(MDBX_ENOMEM, begin_rc) << "the injected failure must fail the nested begin";

  /* the core assertion: the undo must not consider the never-started
   * nested transaction as having resized anything */
  EXPECT_EQ(size_t(0), site_seen("txn_nested_geo_undo"))
      << "the geo-undo branch must not run for a failed nested begin";

  /* the parent must remain fully usable */
  must_put(parent.get(), dbi, "after-failure", "value");
  ASSERT_EQ(mdbx_txn_commit(parent.release()), MDBX_SUCCESS);

  /* the parent's geometry must be intact */
  MDBX_envinfo info_after;
  std::memset(&info_after, 0, sizeof info_after);
  ASSERT_EQ(mdbx_env_info_ex(env.get(), nullptr, &info_after, sizeof info_after), MDBX_SUCCESS);
  EXPECT_EQ(info_before.mi_geo.upper, info_after.mi_geo.upper);
  EXPECT_EQ(info_before.mi_geo.current, info_after.mi_geo.current);
}
