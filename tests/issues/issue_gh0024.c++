#include "mdbx.h++"
#include <cstdio>
#include <utility>

int main() {
  const char payload[] = "external-payload";
  mdbx::default_buffer ref(mdbx::slice(payload), /*make_reference=*/true);
  mdbx::default_buffer dst;
  dst = std::move(ref); // assign(buffer&&) -> src.data() non-const -> assert(is_freestanding())
  assert(dst.is_reference());
  std::printf("moved: %zu bytes\n", dst.length());
  return 0;
}
