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

class testcase_try : public testcase {
public:
  testcase_try(const actor_config &config, const mdbx_pid_t pid) : testcase(config, pid) {}
  bool run() override;
};
REGISTER_TESTCASE(try);

bool testcase_try::run() {
  db_open();
  assert(!txn_guard);

  MDBX_txn *txn = nullptr;
  MDBX_txn *txn2 = nullptr;
  int rc = mdbx_txn_begin(db_guard.get(), nullptr, MDBX_TXN_READWRITE, &txn);
  if (unlikely(rc != MDBX_SUCCESS))
    failure_perror("mdbx_txn_begin(MDBX_TXN_TRY)", rc);
  else {
    rc = mdbx_txn_begin(db_guard.get(), nullptr, MDBX_TXN_TRY, &txn2);
    if (unlikely(rc != MDBX_BUSY))
      failure_perror("mdbx_txn_begin(MDBX_TXN_TRY)", rc);
  }

  txn_guard.reset(txn);
  return true;
}
