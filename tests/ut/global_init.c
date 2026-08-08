#include "mdbx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;

  MDBX_env *env = NULL;
  int err = mdbx_env_create(&env);
  if (err != MDBX_SUCCESS) {
    fprintf(stderr, "%s(), err %u: %s\n", "mdbx_env_create", err, mdbx_strerror(err));
    return EXIT_FAILURE;
  }

  err = mdbx_env_close_ex(env, false);
  if (err != MDBX_SUCCESS) {
    fprintf(stderr, "%s(), err %u: %s\n", "mdbx_env_close_ex", err, mdbx_strerror(err));
    return EXIT_FAILURE;
  }

  puts("Ok");
  return EXIT_SUCCESS;
}
