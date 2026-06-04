/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

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
