#include "mdbx.h++"
#include <cstdio>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

int main(int, char **) {
  const mdbx::path testdb = "testdb";
  mdbx::env::remove(testdb);
  mdbx::env::operate_parameters op(4);
  op.options.enable_validation = true;
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), op);
  unsigned raw = 0;
  mdbx_env_get_flags(env, &raw);
  std::printf("raw MDBX_VALIDATION set: %d\n", (raw & MDBX_VALIDATION) != 0);
  std::printf("get_options().enable_validation: %d\n", env.get_options().enable_validation);
  return ((raw & MDBX_VALIDATION) != 0 && env.get_options().enable_validation) ? EXIT_SUCCESS : EXIT_FAILURE;
}
