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
#include <cstdio>
#include <cstring>

int main() {
  const char *fname = "test_package.mdbx";
  std::puts("test_package: starting");
  try {
    mdbx::env::remove(fname);
  } catch (const mdbx::error &) {
    /* the database is not required to exist beforehand */
  }

  mdbx::env_managed::create_parameters cp;
  mdbx::env_managed::operate_parameters op;
  op.max_maps = 2;

  {
    mdbx::env_managed env(fname, cp, op);
    {
      auto txn = env.start_write();
      auto map = txn.open_map(nullptr, mdbx::key_mode::usual, mdbx::value_mode::single);
      txn.upsert(map, mdbx::slice("key"), mdbx::slice("value"));
      txn.commit();
    }
    {
      auto rtxn = env.start_read();
      auto map = rtxn.open_map(nullptr, mdbx::key_mode::usual, mdbx::value_mode::single);
      mdbx::slice value = rtxn.get(map, mdbx::slice("key"));
      if (value.empty() || value.size() != 5 || memcmp(value.data(), "value", 5) != 0) {
        std::fprintf(stderr, "test_package: unexpected value\n");
        return 1;
      }
      rtxn.abort();
    }
  }
  mdbx::env::remove(fname);
  std::puts("test_package: OK");
  return 0;
}
