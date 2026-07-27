/* nested child spills, then is aborted -> spilled.list leaks. */
#include "mdbx.h++"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
  const mdbx::path testdb = (argc > 1) ? argv[1] : "issue10";
  mdbx::env::remove(testdb);
  mdbx::env_managed::create_parameters cp;
  cp.geometry = mdbx::env::geometry(mdbx::env::geometry::minimal_value, mdbx::env::geometry::default_value,
                                    mdbx::env::geometry::default_value, mdbx::env::geometry::default_value,
                                    mdbx::env::geometry::default_value, mdbx::env::geometry::minimal_value);
  mdbx::env_managed::operate_parameters op;
  op.mode = mdbx::env_managed::nested_transactions;
  op.max_maps = 42;
  mdbx::env_managed env(testdb, cp, op);

  env.set_extra_option(mdbx::env::extra_runtime_option::dp_limit, 128) /* must be >= CURSOR_STACK_SIZE*4 */;
  auto t = env.start_write();
  auto d = t.create_map("dbi");

  char v[4096], k[32];
  memset(v, 7, 4096);
  auto c = t.start_nested();
  for (size_t i = 0; i < 2000; i++) { /* child spills */
    mdbx::slice K(k, snprintf(k, 32, "k%08zd", i));
    mdbx::slice V(v, 1200u + (i % 1500u));
    c.put(d, K, V, mdbx::upsert);
  }
  c.abort();
  return 0;
}
