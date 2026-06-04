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

#include <mdbx.h>
#include <stdio.h>
#include <stdlib.h>

#define check_is(success, rc)                                                                                          \
  do {                                                                                                                 \
    int __rc = (rc);                                                                                                   \
    if (__rc != success) {                                                                                             \
      fprintf(stderr, "mdbx failed at %s:%d: %s\n", __FILE__, __LINE__, mdbx_strerror(__rc));                          \
      exit(EXIT_FAILURE);                                                                                              \
    }                                                                                                                  \
  } while (0)

#define check(rc) check_is(MDBX_SUCCESS, rc)

int main() {
  const char *db_filename = "./test-dbi-nested-txn";
  int err = mdbx_env_delete(db_filename, MDBX_ENV_JUST_DELETE);
  if (err)
    check_is(MDBX_RESULT_TRUE, err);

  MDBX_env *env;
  check(mdbx_env_create(&env));
  mdbx_env_set_maxdbs(env, 64);
  check(mdbx_env_open(env, db_filename, MDBX_NOSUBDIR, 0666));

  MDBX_txn *txn;
  check(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
  MDBX_txn *txn_nested;
  check(mdbx_txn_begin(env, txn, MDBX_TXN_READWRITE, &txn_nested));
  MDBX_dbi dbi;
  check(mdbx_dbi_open(txn_nested, "test", MDBX_CREATE, &dbi));
  check(mdbx_txn_abort(txn_nested));
  check_is(MDBX_NOTFOUND, mdbx_dbi_open(txn, "test", 0, &dbi));
  check(mdbx_txn_abort(txn));

  check(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn));
  check_is(MDBX_NOTFOUND, mdbx_dbi_open(txn, "test", 0, &dbi));
  check(mdbx_txn_abort(txn));

  check(mdbx_env_close(env));
  return EXIT_SUCCESS;
}
