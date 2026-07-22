#include "mdbx.h++"
#include <cstdio>
#include <vector>
int main() {
  std::vector<mdbx::cursor> empty;
  mdbx::cursor from, to;
  try {
    bool r = mdbx::cursor::distribute(from, to, empty);
    std::printf("returned %d\n", r);
  } catch (const std::exception &e) {
    std::printf("caught (acceptable): %s\n", e.what());
    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
