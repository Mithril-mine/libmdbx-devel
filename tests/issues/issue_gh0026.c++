#include "mdbx.h++"
#include <cstdio>
#include <string>

int main() {
  std::u16string s(u"abcd"); // 4 chars = 8 bytes
  bool ok = true;

  mdbx::default_buffer bf_str_ctor(s), bf_str_asg;
  bf_str_asg.assign(s);
  mdbx::slice sl_str_ctor(s), sl_str_asg;
  sl_str_asg.assign(s);

  std::printf("\n%s.length=%zu", "u16string", s.length());
  std::printf(" %s.length=%zu", "bf_str_ctor", bf_str_ctor.length());
  std::printf(" %s.length=%zu", "bf_str_asg", bf_str_asg.length());
  std::printf(" %s.length=%zu", "bf_str_ctor", sl_str_ctor.length());
  std::printf(" %s.length=%zu", "bf_str_asg", sl_str_asg.length());

  ok = bf_str_ctor.length() / s.length() > 1 && ok;
  ok = bf_str_asg.length() / s.length() > 1 && ok;
  ok = sl_str_ctor.length() / s.length() > 1 && ok;
  ok = sl_str_asg.length() / s.length() > 1 && ok;

#if defined(__cpp_lib_string_view) && __cpp_lib_string_view >= 201606L
  std::u16string_view sv(s);
  mdbx::slice sl_sv_ctor(sv), sl_sv_asg;
  sl_sv_asg.assign(sv);
  mdbx::default_buffer bf_sv_ctor(sv), bf_sv_asg;
  bf_sv_asg.assign(sv);

  std::printf("\n%s.length=%zu", "u16string_view", sv.length());
  std::printf(" %s.length=%zu", "bf_sv_ctor", bf_sv_ctor.length());
  std::printf(" %s.length=%zu", "bf_sv_asg", bf_sv_asg.length());
  std::printf(" %s.length=%zu", "bf_sv_ctor", sl_sv_ctor.length());
  std::printf(" %s.length=%zu", "bf_sv_asg", sl_sv_asg.length());

  ok = bf_sv_ctor.length() / s.length() > 1 && ok;
  ok = bf_sv_asg.length() / s.length() > 1 && ok;
  ok = sl_sv_ctor.length() / s.length() > 1 && ok;
  ok = sl_sv_asg.length() / s.length() > 1 && ok;

#endif /* __cpp_lib_string_view*/

  std::printf("\nresult: %s\n", ok ? "SUCCESS" : "FAILURE");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
