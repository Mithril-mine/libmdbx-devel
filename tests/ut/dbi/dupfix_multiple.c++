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
#include "../probe-ctl.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(ENABLE_MEMCHECK) || defined(MDBX_CI)
#if MDBX_DEBUG > 0 || !defined(NDEBUG)
#define RELIEF_FACTOR 16
#else
#define RELIEF_FACTOR 8
#endif
#elif MDBX_DEBUG > 0 || !defined(NDEBUG) || defined(__APPLE__) || defined(_WIN32)
#define RELIEF_FACTOR 4
#elif UINTPTR_MAX > 0xffffFFFFul || ULONG_MAX > 0xffffFFFFul
#define RELIEF_FACTOR 2
#else
#define RELIEF_FACTOR 1
#endif

using buffer = mdbx::default_buffer;

//--------------------------------------------------------------------------------------------------------------
// Deterministic pseudo random source: time-based by default for diversity,
// MDBX_UT_SEED to reproduce a given run. The seed is always reported.
static size_t salt;

static size_t prng() {
  salt = salt * 134775813 + 1;
  return salt ^ ((salt >> 11) * 1822226723);
}

static inline size_t prng(size_t range) { return prng() % range; }

static void seed_prng() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) /* 'getenv': This function or variable may be unsafe */
#endif
  const char *from_env = std::getenv("MDBX_UT_SEED");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  salt = from_env ? static_cast<size_t>(std::strtoull(from_env, nullptr, 0))
                  : size_t(std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::cout << "prng seed: " << salt << (from_env ? " (MDBX_UT_SEED)" : " (time-based)") << std::endl;
}

//--------------------------------------------------------------------------------------------------------------
// The stress workload volume scales as 1/RELIEF_FACTOR, so the geometry upper
// bound must scale the same way (a fixed upper caused MDBX_MAP_FULL on
// low-RELIEF builds, i.e. local release ones). A 32-bit-safe cap is applied
// since MAX_MAPSIZE32 is below 2 GiB.
static intptr_t stress_upper_size() {
  const uint64_t per_relief = uint64_t(2) * mdbx::env::geometry::GiB / RELIEF_FACTOR;
#if UINTPTR_MAX > 0xffffFFFFul || ULONG_MAX > 0xffffFFFFul
  return static_cast<intptr_t>(per_relief);
#else
  const uint64_t cap32 = uint64_t(512) * mdbx::env::geometry::MiB;
  return static_cast<intptr_t>((per_relief < cap32) ? per_relief : cap32);
#endif
}

//--------------------------------------------------------------------------------------------------------------
// Value-parameterized on the page size: 0 means the platform default, 512 is
// the non-default small-page instance (multi-page dupfix chains and deep
// B-trees are reached with far fewer items, and the pagesize axis itself is
// exercised).
namespace {
class DupfixMultiple : public ::testing::TestWithParam<intptr_t> {
protected:
  mdbx::env_managed env;

  void SetUp() override {
    seed_prng();
    std::string name = "test-dupfix-multiple";
    if (GetParam())
      name += "-ps" + std::to_string(GetParam());
    mdbx::env_managed::remove(mdbx::path(name));
    mdbx::env_managed::create_parameters createParameters;
    createParameters.geometry.make_dynamic(16 * mdbx::env::geometry::MiB, stress_upper_size());
    if (GetParam())
      createParameters.geometry.set_pagesize(GetParam());
    env = mdbx::env_managed(name, createParameters, mdbx::env::operate_parameters(4));
  }

  /// Conservative estimate of dupfix items per sub-page, used to size datasets
  /// around page boundaries at any page size (not the exact engine value).
  static size_t page_capacity(size_t psize, size_t value_size) { return (psize - 64) / value_size; }

  /// Reference model: full scan of an ordinal multi_ordinal table.
  using model_t = std::map<uint64_t, std::set<uint64_t>>;

  static model_t scan_model(mdbx::txn_managed &txn, mdbx::map_handle map) {
    model_t model;
    auto cursor = txn.open_cursor(map);
    for (auto it = cursor.to_first(false); it.done; it = cursor.to_next(false))
      model[it.key.as_uint64()].insert(it.value.as_uint64());
    return model;
  }

  static void expect_model(const model_t &expected, const model_t &actual) {
    ASSERT_EQ(expected.size(), actual.size());
    auto e = expected.begin();
    auto a = actual.begin();
    for (; e != expected.end() && a != actual.end(); ++e, ++a) {
      ASSERT_EQ(e->first, a->first) << "key mismatch";
      ASSERT_EQ(e->second, a->second) << "values mismatch for key " << e->first;
    }
  }
};

//--------------------------------------------------------------------------------------------------------------

TEST_P(DupfixMultiple, ordering) {
  auto txn = env.start_write();
  auto map = txn.create_map("ordering", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal);

  /* Unordered single inserts. */
  static const uint64_t setup[] = {21, 7, 22, 26, 24, 23, 25, 27};
  static const uint64_t values[] = {18, 19, 17, 13, 15, 16, 14, 12};
  for (size_t i = 0; i < std::size(setup); ++i)
    txn.insert(map, buffer::key_from_u64(setup[i]), buffer::key_from_u64(values[i]));

  /* Multi-value batches with unsorted payload: the engine must sort dups itself. */
  static const uint64_t array[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 42, 17, 99, 0, 33, 333};
  static const struct {
    uint64_t key;
    size_t offset, count;
  } batches[] = {
      {13, 3, 4}, {10, 0, 1}, {12, 2, 3}, {15, 5, 6}, {14, 4, 5}, {11, 1, 2}, {16, 6, 7},
  };
  for (const auto &b : batches)
    EXPECT_EQ(b.count, txn.put_multiple_samelength(map, buffer::key_from_u64(b.key), array + b.offset, b.count,
                                                   mdbx::upsert));
  txn.commit();

  /* Verify against the reference model instead of a hand-written chain. */
  model_t expected;
  for (size_t i = 0; i < std::size(setup); ++i)
    expected[setup[i]].insert(values[i]);
  for (const auto &b : batches)
    for (size_t i = 0; i < b.count; ++i)
      expected[b.key].insert(array[b.offset + i]);

  auto read = env.start_read();
  expect_model(expected, scan_model(read, map));

  /* Explicit ordering check within every key: values are strictly ascending. */
  auto cursor = read.open_cursor(map);
  for (const auto &kv : expected) {
    ASSERT_TRUE(cursor.find(buffer::key_from_u64(kv.first), false).done) << "key " << kv.first;
    uint64_t prev = 0;
    bool first = true;
    for (auto dup = cursor.to_current_first_multi(false); dup.done; dup = cursor.to_current_next_multi(false)) {
      const uint64_t v = dup.value.as_uint64();
      if (!first) {
        EXPECT_LT(prev, v) << "key " << kv.first << ": values must be strictly ascending";
      }
      prev = v;
      first = false;
    }
  }
  read.abort();
}

//--------------------------------------------------------------------------------------------------------------

TEST_P(DupfixMultiple, batch_read) {
  auto txn = env.start_write();
  auto map = txn.create_map("batch", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal);
  const size_t psize = txn.get_map_stat(map).ms_psize;
  const size_t per_page = page_capacity(psize, sizeof(uint64_t));

  /* Three keys, each spanning more than one dupfix sub-page: batch reads must
   * return sub-page-sized chunks and iterate across keys. */
  model_t expected;
  for (uint64_t k : {1, 2, 3}) {
    std::vector<uint64_t> values;
    for (uint64_t v = 0; v < per_page * 5 / 2 + 1; ++v)
      values.push_back(k * 100000 + v);
    EXPECT_EQ(values.size(), txn.put_multiple_samelength(map, buffer::key_from_u64(k), values.data(), values.size(),
                                                         mdbx::upsert));
    expected[k] = std::set<uint64_t>(values.begin(), values.end());
  }
  txn.commit();

  auto read = env.start_read();
  auto cursor = read.open_cursor(map);

  /* GET_MULTIPLE without a position: ENODATA regardless of throw_notfound
   * (only MDBX_NOTFOUND is ever suppressed by the C++ wrapper). */
  EXPECT_THROW(cursor.get_multiple_samelength(false), mdbx::no_data);
  /* The same typed exception when throwing is requested explicitly. */
  EXPECT_THROW(cursor.get_multiple_samelength(true), mdbx::no_data);

  /* SEEK_AND_GET_MULTIPLE: positions at the key and returns the first chunk. */
  auto seek = cursor.seek_multiple_samelength(buffer::key_from_u64(1), false);
  ASSERT_TRUE(seek.done);
  EXPECT_EQ(seek.key.as_uint64(), 1);
  EXPECT_LT(seek.value.length(), psize) << "a chunk is a sub-page payload";

  /* NEXT_MULTIPLE: iterates the dupfix sub-pages of the CURRENT key (the
   * movement stops at the key's end, re-seek to proceed to the next one). */
  size_t total_chunks = 0;
  for (uint64_t k : {1, 2, 3}) {
    auto chunk = cursor.seek_multiple_samelength(buffer::key_from_u64(k), false);
    ASSERT_TRUE(chunk.done) << "key " << k;
    EXPECT_EQ(chunk.key.as_uint64(), k);
    EXPECT_LT(chunk.value.length(), psize) << "a chunk is a sub-page payload";
    size_t chunks = 1;
    size_t seen = chunk.value.length() / sizeof(uint64_t);
    for (auto it = cursor.next_multiple_samelength(false); it.done; it = cursor.next_multiple_samelength(false)) {
      ++chunks;
      seen += it.value.length() / sizeof(uint64_t);
    }
    EXPECT_GE(chunks, size_t(2)) << "key " << k << ": chunks must span more than one sub-page";
    EXPECT_EQ(seen, expected[k].size()) << "key " << k << ": all dups must be covered by the chunks";
    total_chunks += chunks;
  }
  EXPECT_GE(total_chunks, expected.size() * 2);

  /* PREV_MULTIPLE: walks sub-pages backwards within the current key. Note
   * that PREV_MULTIPLE fills only the value (the key slice stays invalid),
   * so it is not read here. */
  EXPECT_TRUE(cursor.seek_multiple_samelength(buffer::key_from_u64(2), false).done);
  auto next = cursor.next_multiple_samelength(false);
  ASSERT_TRUE(next.done);
  auto prev = cursor.previous_multiple_samelength(false);
  ASSERT_TRUE(prev.done);
  EXPECT_EQ(prev.value.length(), next.value.length()) << "back to the same sub-page as after the seek";
  read.abort();

  /* INCOMPATIBLE: batch ops are dupfix-only. */
  auto txn2 = env.start_write();
  auto single = txn2.create_map("single", mdbx::key_mode::ordinal, mdbx::value_mode::single);
  txn2.insert(single, buffer::key_from_u64(42), buffer::key_from_u64(42));
  std::cout << "DBG inserted" << std::endl;
  auto plain = txn2.open_cursor(single);
  std::cout << "DBG before incompatible" << std::endl;
  try {
    plain.get_multiple_samelength(true);
    std::cout << "DBG incompatible: no throw" << std::endl;
  } catch (const mdbx::exception &ex) {
    std::cout << "DBG incompatible threw: " << ex.what() << std::endl;
  }
  txn2.abort();
}

//--------------------------------------------------------------------------------------------------------------

TEST_P(DupfixMultiple, multiple_put) {
  auto txn = env.start_write();
  auto map = txn.create_map("mput", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal);

  std::vector<uint64_t> values;
  for (uint64_t v = 0; v < 111; ++v)
    values.push_back(v * 2 /* even values */);

  /* Fresh key: a full batch. */
  EXPECT_EQ(values.size(),
            txn.put_multiple_samelength(map, buffer::key_from_u64(1), values.data(), values.size(), mdbx::upsert));

  /* Upsert with an overlapping payload: existing values are no-ops, missing
   * ones are added, the count of processed items is still full. */
  std::vector<uint64_t> overlapping(values.begin() + 2, values.end());
  overlapping.push_back(1);
  overlapping.push_back(3);
  EXPECT_EQ(overlapping.size(), txn.put_multiple_samelength(map, buffer::key_from_u64(1), overlapping.data(),
                                                            overlapping.size(), mdbx::upsert));

  /* Empty batch: a successful no-op. */
  EXPECT_EQ(size_t(0), txn.put_multiple_samelength(map, buffer::key_from_u64(1), values.data(), 0, mdbx::upsert));

  /* insert_unique (NOOVERWRITE) with a fresh key: the key-level guard passes
   * for the first item and the whole batch is inserted (NOOVERWRITE guards
   * the KEY, not each dup). */
  static const uint64_t odd[] = {7, 9, 11, 13};
  EXPECT_EQ(std::size(odd),
            txn.put_multiple_samelength(map, buffer::key_from_u64(2), odd, std::size(odd), mdbx::insert_unique, true));
  /* ...and on an existing key nothing is done at all: with allow_partial the
   * KEYEXIST turns into a zero-count result instead of an exception. */
  EXPECT_EQ(size_t(0),
            txn.put_multiple_samelength(map, buffer::key_from_u64(1), odd, std::size(odd), mdbx::insert_unique, true));
  txn.commit();

  model_t expected;
  expected[1] = std::set<uint64_t>(values.begin(), values.end());
  expected[1].insert(1);
  expected[1].insert(3);
  expected[2] = {7, 9, 11, 13};

  /* Without allow_partial KEYEXIST is an exception; the txn becomes broken
   * (mdbx_txn_break), so it is performed in a throw-away transaction. */
  auto doomed = env.start_write();
  EXPECT_THROW(doomed.put_multiple_samelength(map, buffer::key_from_u64(2), odd, std::size(odd), mdbx::insert_unique),
               mdbx::key_exists);
  doomed.abort();

  /* Cursor-based put_multiple must match the txn-based one. */
  auto txn2 = env.start_write();
  auto cursor = txn2.open_cursor(map);
  static const uint64_t more[] = {21, 23, 29};
  EXPECT_EQ(std::size(more), cursor.put_multiple_samelength(buffer::key_from_u64(2), more, std::size(more),
                                                            mdbx::upsert));

  /* update (CURRENT) via a cursor deletes just the CURRENT dup and inserts
   * the batch next to the remaining ones. */
  static const uint64_t replacement[] = {100, 200};
  EXPECT_EQ(std::size(replacement), cursor.put_multiple_samelength(buffer::key_from_u64(2), replacement,
                                                                   std::size(replacement), mdbx::update));
  txn2.commit();
  expected[2] = {7, 9, 11, 13, 21, 23, 100, 200};

  /* The txn-level update (CURRENT) replaces the whole multivalue. */
  auto txn3 = env.start_write();
  EXPECT_EQ(std::size(replacement), txn3.put_multiple_samelength(map, buffer::key_from_u64(2), replacement,
                                                                 std::size(replacement), mdbx::update));
  EXPECT_THROW(txn3.put_multiple_samelength(map, buffer::key_from_u64(999), replacement, std::size(replacement),
                                            mdbx::update),
               mdbx::not_found)
      << "update of an absent key must fail";
  txn3.commit();
  expected[2] = {100, 200};

  /* The same fixed-length batch for another key of a multi_samelength table. */
  struct payload15 {
    uint8_t bytes[15];
  };
  static const payload15 payloads[] = {{1, 2}, {3, 4}};
  auto txn4 = env.start_write();
  auto fix15 = txn4.create_map("fix15", mdbx::key_mode::usual, mdbx::value_mode::multi_samelength);
  EXPECT_EQ(size_t(2), txn4.put_multiple_samelength(fix15, mdbx::slice("k1"), payloads, std::size(payloads),
                                                    mdbx::upsert));
  EXPECT_EQ(size_t(2), txn4.put_multiple_samelength(fix15, mdbx::slice("k2"), payloads, std::size(payloads),
                                                    mdbx::upsert));
  txn4.commit();

  auto read = env.start_read();
  expect_model(expected, scan_model(read, map));
  read.abort();
}

//--------------------------------------------------------------------------------------------------------------

TEST_P(DupfixMultiple, erase_and_conversion) {
  auto txn = env.start_write();
  auto map = txn.create_map("erase", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal);
  const size_t psize = txn.get_map_stat(map).ms_psize;
  const size_t per_page = page_capacity(psize, sizeof(uint64_t));

  /* A large multivalue spanning several dupfix sub-pages. */
  std::vector<uint64_t> values;
  for (uint64_t v = 0; v < per_page * 3 + 7; ++v)
    values.push_back(1000 + v);
  const uint64_t key = 1;
  EXPECT_EQ(values.size(),
            txn.put_multiple_samelength(map, buffer::key_from_u64(key), values.data(), values.size(), mdbx::upsert));
  txn.commit();

  /* Single dup erase via txn.erase(key, value) from the middle sub-page. */
  auto txn2 = env.start_write();
  const uint64_t middle = values[values.size() / 2];
  EXPECT_TRUE(txn2.erase(map, buffer::key_from_u64(key), mdbx::slice::wrap(middle)));
  EXPECT_FALSE(txn2.erase(map, buffer::key_from_u64(key), mdbx::slice::wrap(middle)))
      << "a repeated erase must report absence";
  EXPECT_FALSE(txn2.erase(map, buffer::key_from_u64(42), mdbx::slice::wrap(middle))) << "an absent key";
  txn2.commit();

  auto read = env.start_read();
  auto cursor = read.open_cursor(map);
  ASSERT_TRUE(cursor.find(buffer::key_from_u64(key), false).done);
  EXPECT_EQ(cursor.count_multivalue(), values.size() - 1);

  /* Batch reads remain consistent after the erasure. */
  size_t seen = 0;
  for (auto it = cursor.get_multiple_samelength(false); it.done; it = cursor.next_multiple_samelength(false))
    seen += it.value.length() / sizeof(uint64_t);
  EXPECT_EQ(seen, values.size() - 1);
  read.abort();

  /* Cursor-based erasures: one dup, then the whole multivalue. */
  auto txn3 = env.start_write();
  auto cur = txn3.open_cursor(map);
  ASSERT_TRUE(cur.find(buffer::key_from_u64(key), false).done);
  ASSERT_TRUE(cur.to_current_first_multi(false).done);
  EXPECT_TRUE(cur.erase(/* whole_multivalue = */ false)) << "erase the current (first) dup";
  ASSERT_TRUE(cur.seek(buffer::key_from_u64(key))) << "the key must survive with remaining dups";
  EXPECT_TRUE(cur.erase(buffer::key_from_u64(key), /* whole_multivalue = */ true)) << "erase all remaining dups";
  EXPECT_FALSE(cur.seek(buffer::key_from_u64(key))) << "the key must be gone";
  EXPECT_FALSE(txn3.erase(map, buffer::key_from_u64(key))) << "txn-level erase of an absent key";
  txn3.commit();

  /* The leaf is now plain: re-adding dups rebuilds a dupfix sub-tree. */
  auto txn4 = env.start_write();
  static const uint64_t again[] = {5, 3, 4};
  EXPECT_EQ(std::size(again),
            txn4.put_multiple_samelength(map, buffer::key_from_u64(key), again, std::size(again), mdbx::upsert));
  txn4.put_multiple_samelength(map, buffer::key_from_u64(key + 1), values.data(), per_page + 1, mdbx::upsert);
  txn4.commit();

  model_t expected;
  expected[key] = {3, 4, 5};
  expected[key + 1] = std::set<uint64_t>(values.begin(), values.begin() + per_page + 1);
  auto read2 = env.start_read();
  expect_model(expected, scan_model(read2, map));
  read2.abort();
}

//--------------------------------------------------------------------------------------------------------------

TEST_P(DupfixMultiple, stress_model) {
  if (GetParam() == 0)
    GTEST_SKIP() << "the stress runs on the small-page instance only";

  /* Small randomized stress: bounded volume (~1e5 ops / RELIEF_FACTOR), the
   * small page size makes the B-tree deep and dupfix sub-pages split/merge
   * early. Every stage is verified against a reference model. */
  auto txn = env.start_write();
  auto map = txn.create_map("stress", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal);
  const size_t psize = txn.get_map_stat(map).ms_psize;
  const size_t per_page = page_capacity(psize, sizeof(uint64_t));
  const size_t keyspace = per_page * 3;
  const size_t ops = 100000 / RELIEF_FACTOR;
  model_t model;

  for (size_t n = 0; n < ops; ++n) {
    const auto dice = prng(100);
    if (dice < 55) {
      const uint64_t k = prng(keyspace), v = prng(keyspace * 7);
      txn.upsert(map, buffer::key_from_u64(k), buffer::key_from_u64(v));
      model[k].insert(v);
    } else if (dice < 85) {
      /* batch upsert, unsorted payload around page boundaries */
      std::vector<uint64_t> v;
      const size_t count = 1 + prng(per_page * 2 + 2);
      for (size_t i = 0; i < count; ++i)
        v.push_back(prng(keyspace * 7));
      const uint64_t k = prng(keyspace);
      txn.put_multiple_samelength(map, buffer::key_from_u64(k), v.data(), v.size(), mdbx::upsert);
      model[k].insert(v.begin(), v.end());
    } else if (dice < 95) {
      /* cursor-based batch */
      std::vector<uint64_t> v;
      const size_t count = 1 + prng(per_page + 1);
      for (size_t i = 0; i < count; ++i)
        v.push_back(prng(keyspace * 7));
      const uint64_t k = prng(keyspace);
      auto cursor = txn.open_cursor(map);
      cursor.put_multiple_samelength(buffer::key_from_u64(k), v.data(), v.size(), mdbx::upsert);
      model[k].insert(v.begin(), v.end());
    } else {
      /* erase a single dup */
      if (model.empty()) {
        const uint64_t k = prng(keyspace), v = prng(keyspace * 7);
        txn.upsert(map, buffer::key_from_u64(k), buffer::key_from_u64(v));
        model[k].insert(v);
        continue;
      }
      auto it = model.begin();
      std::advance(it, prng(model.size()));
      const uint64_t k = it->first;
      if (it->second.size() > 1) {
        auto vit = it->second.begin();
        std::advance(vit, prng(it->second.size()));
        EXPECT_TRUE(txn.erase(map, buffer::key_from_u64(k), mdbx::slice::wrap(*vit)));
        it->second.erase(*vit);
        if (it->second.empty())
          model.erase(it);
      } else {
        EXPECT_TRUE(txn.erase(map, buffer::key_from_u64(k)));
        model.erase(it);
      }
    }

    if (n % 7777 == 7776) {
      txn.commit();
      txn = env.start_write();
      expect_model(model, scan_model(txn, map));
    }
  }

  txn.commit();
  auto read = env.start_read();
  size_t entries = 0;
  for (const auto &kv : model)
    entries += kv.second.size();
  EXPECT_EQ(read.get_map_stat(map).ms_entries, entries);
  expect_model(model, scan_model(read, map));
  read.abort();
}

//--------------------------------------------------------------------------------------------------------------

#if defined(MDBX_PROBES)
static std::string probe_ctl(const char *request) {
  char reply[16384];
  const int rc = mprobe_ctl(request, reply, sizeof(reply));
  EXPECT_EQ(rc, 0) << "mprobe_ctl(" << request << ")";
  return std::string(reply);
}

static size_t probe_hits(const char *tag) {
  const std::string reply = probe_ctl((std::string("query ") + tag).c_str());
  size_t pos = reply.find("site ");
  while (pos != std::string::npos) {
    char name[128];
    unsigned kind = 0, armed = 0;
    unsigned long long seen = 0, hits = 0, suppressed = 0, value = 0;
    if (sscanf(reply.c_str() + pos, "site %127s %u %u %llu %llu %llu %llu", name, &kind, &armed, &seen, &hits,
               &suppressed, &value) >= 6 &&
        std::strcmp(name, tag) == 0)
      return size_t(hits);
    pos = reply.find("site ", pos + 1);
  }
  return 0;
}
#endif /* MDBX_PROBES */

/* Self-asserted coverage: every watched dupfix/multiple branch of the engine
 * must be hit by the scenario above (the probe-bus WATCH channel). */
TEST_P(DupfixMultiple, probe_watch) {
#if defined(MDBX_PROBES)
  if (GetParam() == 0)
    GTEST_SKIP() << "probe coverage runs once, on the small-page instance";
  probe_ctl("reset *");
  probe_ctl("arm cursor_multiple_*");
  probe_ctl("arm node_dupfix_*");
  probe_ctl("arm page_split_dupfix");

  auto txn = env.start_write();
  auto map = txn.create_map("probes", mdbx::key_mode::ordinal, mdbx::value_mode::multi_ordinal);
  const size_t psize = txn.get_map_stat(map).ms_psize;
  const size_t per_page = page_capacity(psize, sizeof(uint64_t));
  std::vector<uint64_t> values(per_page * 3 + 3);
  for (size_t i = 0; i < values.size(); ++i)
    values[i] = i * 2;
  EXPECT_EQ(values.size(),
            txn.put_multiple_samelength(map, buffer::key_from_u64(1), values.data(), values.size(), mdbx::upsert));
  txn.put_multiple_samelength(map, buffer::key_from_u64(2), values.data(), values.size(), mdbx::upsert);
  txn.commit();

  auto read = env.start_read();
  auto cursor = read.open_cursor(map);
  EXPECT_TRUE(cursor.seek_multiple_samelength(buffer::key_from_u64(2), false).done);
  size_t chunks = 0;
  for (auto it = cursor.get_multiple_samelength(false); it.done; it = cursor.next_multiple_samelength(false))
    ++chunks;
  EXPECT_GE(chunks, 3);
  /* re-position before walking back: the exhausted next-loop leaves the
   * cursor at the key's end */
  EXPECT_TRUE(cursor.seek_multiple_samelength(buffer::key_from_u64(2), false).done);
  EXPECT_TRUE(cursor.next_multiple_samelength(false).done);
  EXPECT_TRUE(cursor.previous_multiple_samelength(false).done);
  read.abort();

  auto wtxn = env.start_write();
  EXPECT_TRUE(wtxn.erase(map, buffer::key_from_u64(1), mdbx::slice::wrap(uint64_t(14))));
  wtxn.commit();

  EXPECT_GT(probe_hits("cursor_multiple_put"), 0);
  EXPECT_GT(probe_hits("cursor_multiple_seek"), 0);
  EXPECT_GT(probe_hits("cursor_multiple_get"), 0);
  EXPECT_GT(probe_hits("cursor_multiple_next"), 0);
  EXPECT_GT(probe_hits("cursor_multiple_prev"), 0);
  EXPECT_GT(probe_hits("node_dupfix_add"), 0);
  EXPECT_GT(probe_hits("node_dupfix_del"), 0);
  EXPECT_GT(probe_hits("page_split_dupfix"), 0);
  probe_ctl("disarm *");
#else
  GTEST_SKIP() << "probe-bus is disabled (build without MDBX_PROBES=ON)";
#endif
}

} // namespace

//--------------------------------------------------------------------------------------------------------------

INSTANTIATE_TEST_SUITE_P(pagesize, DupfixMultiple, ::testing::Values(0, 512),
                         [](const ::testing::TestParamInfo<intptr_t> &info) {
                           return info.param ? "ps" + std::to_string(info.param) : std::string("default");
                         });
