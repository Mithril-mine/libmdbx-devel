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
#include <gtest/gtest.h>

#include "../test-env.h"

#include <string>

TEST(issue_gh0026, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  std::u16string s(u"abcd1234"); // 8 chars = 16 bytes
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
  EXPECT_TRUE(ok);
}
