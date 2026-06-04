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

#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <unordered_set>
#include <vector>

mdbx::path db_filename = "test-revins";

int doit() {
  mdbx::env::remove(db_filename);

  mdbx::env::operate_parameters operateParameters(100, 10);
  mdbx::env_managed::create_parameters createParameters;
  createParameters.geometry.pagesize = mdbx::env::limits::pagesize_min();

  mdbx::env_managed env(db_filename, createParameters, operateParameters);
  auto txn = env.start_write();
  auto map1 = txn.create_map("map1", mdbx::key_mode::usual, mdbx::value_mode::single);
  auto map2 = txn.create_map("map2", mdbx::key_mode::usual, mdbx::value_mode::single);
  auto map3 = txn.create_map("map3", mdbx::key_mode::usual, mdbx::value_mode::multi_ordinal);
  auto map4 = txn.create_map("map4", mdbx::key_mode::usual, mdbx::value_mode::multi_ordinal);
  txn.commit();

  env.set_extra_option(mdbx::env::extra_runtime_option::split_reserve, 4242);

  bool ok = true;
  txn = env.start_write();
  unsigned prev_height = 0;
  for (uint64_t order = INT16_MAX; order > 0; --order) {
    char buf[32];
    size_t kl = snprintf(buf, sizeof(buf), "%04x", unsigned(order));
    const auto kv = mdbx::default_buffer_pair(mdbx::slice(buf, kl), "");
    std::cout << "+" << kv << "\n";
    txn.insert(map1, kv);
    const auto stat = txn.get_map_stat(map1);
    if (prev_height != stat.ms_depth) {
      printf("tree-height %u -> %u, %zu items\n\n", prev_height, stat.ms_depth, (size_t)stat.ms_entries);
      prev_height = stat.ms_depth;
      fflush(nullptr);
    }
    if (stat.ms_depth > 3)
      break;
  }
  txn.commit();

  txn = env.start_write();
  prev_height = 0;
  for (uint64_t order = 0; order < INT16_MAX; ++order) {
    char buf[32];
    size_t kl = snprintf(buf, sizeof(buf), "%04x", unsigned(order));
    const auto kv = mdbx::default_buffer_pair(mdbx::slice(buf, kl), "");
    std::cout << "+" << kv << "\n";
    txn.insert(map2, kv);
    const auto stat = txn.get_map_stat(map2);
    if (prev_height != stat.ms_depth) {
      printf("tree-height %u -> %u, %zu items\n\n", prev_height, stat.ms_depth, (size_t)stat.ms_entries);
      prev_height = stat.ms_depth;
      fflush(nullptr);
    }
    if (stat.ms_depth > 3)
      break;
  }
  txn.commit();

  txn = env.start_write();
  prev_height = 0;
  for (uint64_t order = 0; order < INT16_MAX; ++order) {
    const auto kv = mdbx::default_buffer_pair("key", mdbx::slice::wrap(order));
    std::cout << "+" << kv << "\n";
    txn.upsert(map3, kv);
    const auto stat = txn.get_map_stat(map3);
    if (prev_height != stat.ms_depth) {
      printf("tree-height %u -> %u, %zu items\n\n", prev_height, stat.ms_depth, (size_t)stat.ms_entries);
      prev_height = stat.ms_depth;
      fflush(nullptr);
    }
    if (stat.ms_depth > 3)
      break;
  }
  txn.commit();

  txn = env.start_write();
  prev_height = 0;
  for (uint64_t order = INT16_MAX; order > 0; --order) {
    const auto kv = mdbx::default_buffer_pair("key", mdbx::slice::wrap(order));
    std::cout << "+" << kv << "\n";
    txn.upsert(map4, kv);
    const auto stat = txn.get_map_stat(map4);
    if (prev_height != stat.ms_depth) {
      printf("tree-height %u -> %u, %zu items\n\n", prev_height, stat.ms_depth, (size_t)stat.ms_entries);
      prev_height = stat.ms_depth;
      fflush(nullptr);
    }
    if (stat.ms_depth > 3)
      break;
  }
  txn.commit();

  if (ok) {
    std::cout << "OK\n";
    return EXIT_SUCCESS;
  } else {
    std::cerr << "FAIL\n";
    return EXIT_FAILURE;
  }
}
static char log_buffer[1024];

static void logger_nofmt(MDBX_log_level_t loglevel, const char *function, int line, const char *msg,
                         unsigned length) noexcept {
  (void)length;
  (void)loglevel;
  fprintf(stdout, "%s:%u %s", function, line, msg);
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  mdbx_setup_debug_nofmt(MDBX_LOG_NOTICE, MDBX_DBG_ASSERT, logger_nofmt, log_buffer, sizeof(log_buffer));
  try {
    return doit();
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
