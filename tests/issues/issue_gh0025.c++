#include "mdbx.h++"
#include <cstdio>

int main() {
  const char payload[] = "0123456789";
  mdbx::default_buffer buf(mdbx::slice(payload), /*make_reference=*/true);
  assert(buf.is_reference());
  buf.append(nullptr, 0);
  assert(buf.is_reference());
  buf.append("X", 1); // reserve_tailroom -> silo_.resize -> reshape<false> with EXTERNAL content
  assert(buf.is_freestanding());
  std::printf("result: %.*s (len=%zu)\n", int(buf.length()), buf.char_ptr(), buf.length());
  return 0;
}
