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
