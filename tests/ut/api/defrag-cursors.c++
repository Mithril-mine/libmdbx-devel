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

#include <string>
#include <thread>

static std::string to_std_string(const mdbx::slice &s) {
  return s.as_string<char, std::char_traits<char>, std::allocator<char>>();
}

TEST(ut_defrag_cursors, txn_lock_unlock) {
  const mdbx::path testdb = "test-defrag-cursors";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));

  EXPECT_TRUE(env.txn_lock(false));
  env.txn_unlock();
  EXPECT_TRUE(env.txn_lock(true));
  env.txn_unlock();

  {
    auto writer = env.start_write();
    EXPECT_FALSE(env.txn_lock(true)) << "write transaction already holds the lock";
    writer.abort();
  }
  EXPECT_TRUE(env.txn_lock(true));
  env.txn_unlock();

  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_defrag_cursors, txn_refresh) {
  const mdbx::path testdb = "test-defrag-cursors";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));

  auto map = [&] {
    auto writer = env.start_write();
    auto m = writer.create_map("table");
    writer.upsert(m, mdbx::slice("key"), mdbx::slice("old"));
    writer.commit();
    return m;
  }();

  auto reader = env.start_read();
  EXPECT_EQ(to_std_string(reader.get(map, mdbx::slice("key"))), "old");
  EXPECT_TRUE(reader.refresh()) << "already at the most recent snapshot";

  ::std::thread writer([&] {
    auto txn = env.start_write();
    txn.upsert(map, mdbx::slice("key"), mdbx::slice("new"));
    txn.commit();
  });
  writer.join();

  EXPECT_EQ(to_std_string(reader.get(map, mdbx::slice("key"))), "old") << "snapshot is frozen before refresh";
  EXPECT_FALSE(reader.refresh());
  EXPECT_EQ(to_std_string(reader.get(map, mdbx::slice("key"))), "new");

  reader.abort();
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_defrag_cursors, cursor_reset) {
  const mdbx::path testdb = "test-defrag-cursors";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto writer = env.start_write();
    auto map = writer.create_map("table");
    writer.upsert(map, mdbx::slice("key"), mdbx::slice("value"));
    writer.commit();
  }
  {
    auto reader = env.start_read();
    auto map = reader.open_map("table");
    auto cursor = reader.open_cursor(map);
    EXPECT_TRUE(cursor.to_first());
    cursor.reset();
    EXPECT_THROW(cursor.current(), mdbx::exception);
    EXPECT_TRUE(cursor.to_first());
    EXPECT_EQ(to_std_string(cursor.current().value), "value");
    reader.abort();
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_defrag_cursors, cursor_bunch_delete) {
  const mdbx::path testdb = "test-defrag-cursors";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto writer = env.start_write();
    auto map = writer.create_map("table", mdbx::key_mode::usual, mdbx::value_mode::multi);
    for (int i = 0; i < 16; ++i) {
      const std::string key = "key-" + std::to_string(i);
      for (int j = 0; j < 3; ++j) {
        const std::string value = "value-" + std::to_string(j);
        writer.upsert(map, mdbx::slice(key), mdbx::slice(value));
      }
    }
    writer.commit();
  }
  {
    auto writer = env.start_write();
    auto map = writer.open_map("table", mdbx::key_mode::usual, mdbx::value_mode::multi);
    auto cursor = writer.open_cursor(map);

    EXPECT_TRUE(cursor.to_first());
    EXPECT_EQ(cursor.count_multivalue(), 3u);
    EXPECT_EQ(cursor.erase_bunch(mdbx::cursor::delete_current_multival_all), 3u);

    EXPECT_TRUE(cursor.to_first());
    EXPECT_EQ(cursor.erase_bunch(mdbx::cursor::delete_current_value), 1u);
    EXPECT_EQ(cursor.count_multivalue(), 2u);

    EXPECT_TRUE(cursor.to_last());
    EXPECT_EQ(cursor.erase_bunch(mdbx::cursor::delete_before_including), 2u + 13u * 3u + 3u);
    EXPECT_TRUE(cursor.eof());

    writer.commit();
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_defrag_cursors, cursor_delete_range) {
  const mdbx::path testdb = "test-defrag-cursors";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto writer = env.start_write();
    auto map = writer.create_map("table");
    for (int i = 0; i < 100; ++i) {
      const std::string key = "key-" + std::to_string(i);
      writer.upsert(map, mdbx::slice(key), mdbx::slice("value"));
    }
    writer.commit();
  }
  {
    auto writer = env.start_write();
    auto map = writer.open_map("table");
    auto begin = writer.open_cursor(map);
    auto end = writer.open_cursor(map);
    EXPECT_TRUE(begin.to_first());
    EXPECT_TRUE(end.to_last());
    EXPECT_EQ(begin.erase_range(end, /*end_including=*/true), 100u);
    EXPECT_FALSE(begin.to_first(false).done);
    writer.commit();
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_defrag_cursors, get_datacmp) {
  EXPECT_NE(mdbx::get_datacmp(MDBX_db_flags_t(0)), nullptr);
  EXPECT_NE(mdbx::get_datacmp(MDBX_db_flags_t(MDBX_DUPSORT)), nullptr);
  EXPECT_EQ(mdbx::get_datacmp(MDBX_db_flags_t(0)), mdbx::get_datacmp(MDBX_db_flags_t(0)));
}

TEST(ut_defrag_cursors, defrag) {
  const mdbx::path testdb = "test-defrag-cursors";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto writer = env.start_write();
    auto map = writer.create_map("table");
    const std::string chunk(8192, 'x');
    for (int i = 0; i < 1000; ++i) {
      const std::string key = "key-" + std::to_string(i);
      writer.upsert(map, mdbx::slice(key), mdbx::slice(chunk));
    }
    writer.commit();
  }
  {
    auto writer = env.start_write();
    auto map = writer.open_map("table");
    for (int i = 0; i < 500; ++i) {
      const std::string key = "key-" + std::to_string(i);
      writer.erase(map, mdbx::slice(key));
    }
    writer.commit();
  }

  unsigned notifications = 0;
  auto visitor = [&notifications](const mdbx::env::defrag_result &progress) {
    ++notifications;
    return progress.pages_moved < 1000 ? mdbx::env::defrag_control::proceed : mdbx::env::defrag_control::discontinue;
  };
  const mdbx::env::defrag_result result = env.defrag(visitor);

  EXPECT_GT(notifications, 0u);
  EXPECT_GT(result.pages_moved, 0u);
  EXPECT_GT(result.pages_whole, 0u);

  env.close();
  mdbx::env::remove(testdb);
}