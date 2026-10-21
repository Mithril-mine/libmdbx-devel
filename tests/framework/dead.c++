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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "test.h++"

class testcase_deadread : public testcase {
public:
  testcase_deadread(const actor_config &config, const mdbx_pid_t pid) : testcase(config, pid) {}
  bool run() override;
};
REGISTER_TESTCASE(deadread);

bool testcase_deadread::run() {
  db_open();
  txn_begin(true);
  cursor_guard.reset();
  txn_guard.reset();
  db_guard.reset();
  return true;
}

//-----------------------------------------------------------------------------

class testcase_deadwrite : public testcase {
public:
  testcase_deadwrite(const actor_config &config, const mdbx_pid_t pid) : testcase(config, pid) {}
  bool run() override;
};

REGISTER_TESTCASE(deadwrite);

bool testcase_deadwrite::run() {
  db_open();
  txn_begin(false);
  cursor_guard.reset();
  txn_guard.reset();
  db_guard.reset();
  return true;
}
