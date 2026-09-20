/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

namespace {
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}

void seed(mdbx::txn_managed &txn, mdbx::map_handle map) {
  for (int i = 0; i < 8; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "k%d", i);
    txn.upsert(map, mdbx::slice(key, strlen(key)), "v");
  }
}
} // namespace

TEST(ut_txn_park, park_unpark_roundtrip) {
  auto env = make_env("test-txn-park");
  {
    auto w = env.start_write();
    auto map = w.create_map("table");
    seed(w, map);
    w.commit();
  }

  auto txn = env.start_read();
  auto map = txn.open_map("table");
  EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("v"));

  txn.park_reading(false);
  EXPECT_THROW((void)txn.get(map, "k1"), mdbx::bad_transaction) << "parked txn must reject data ops";

  EXPECT_FALSE(txn.unpark_reading()) << "ordinary unpark is not a restart";
  EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("v")) << "unparked txn works again";
  EXPECT_FALSE(txn.unpark_reading()) << "unparking an active txn is a no-op (returns false)";

  txn.abort();
  env.close();
  mdbx::env::remove("test-txn-park");
}

TEST(ut_txn_park, autounpark) {
  auto env = make_env("test-txn-park");
  {
    auto w = env.start_write();
    auto map = w.create_map("table");
    seed(w, map);
    w.commit();
  }

  auto txn = env.start_read();
  auto map = txn.open_map("table");
  txn.park_reading(); /* autounpark = true */
  EXPECT_EQ(txn.get(map, "k3"), mdbx::slice("v")) << "autounpark must transparently resume the txn";

  txn.abort();
  env.close();
  mdbx::env::remove("test-txn-park");
}

TEST(ut_txn_park, write_txn_cannot_park) {
  auto env = make_env("test-txn-park");
  auto w = env.start_write();
  EXPECT_ANY_THROW(w.park_reading());
  w.abort();
  env.close();
  mdbx::env::remove("test-txn-park");
}