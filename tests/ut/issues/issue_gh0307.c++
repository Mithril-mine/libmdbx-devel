/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but
/// not limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/307
// mdbx_dbi_rename() published the new name process-wide immediately and nothing restored it when the
// transaction (or a nested child) aborted. An aborted rename followed by a parent commit left both
// names in the main table sharing the same pages (silent committed corruption), and a top-level
// aborted rename left the handle unusable (MDBX_BAD_DBI) with the rename name persisting.

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../test-env.h"

namespace {

constexpr int kRows = 2000;

class env_ptr {
  MDBX_env *env = nullptr;

public:
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
  MDBX_env *release() {
    MDBX_env *e = env;
    env = nullptr;
    return e;
  }
  void close() {
    if (env) {
      mdbx_env_close(env);
      env = nullptr;
    }
  }
};

class txn_ptr {
  MDBX_txn *txn = nullptr;

public:
  ~txn_ptr() {
    if (txn)
      mdbx_txn_abort(txn);
  }
  MDBX_txn **operator&() { return &txn; }
  MDBX_txn *get() const { return txn; }
  MDBX_txn *release() {
    MDBX_txn *t = txn;
    txn = nullptr;
    return t;
  }
};

struct files_remover {
  const std::vector<std::string> paths;
  files_remover(std::initializer_list<std::string> list) : paths(list) {
    for (const auto &path : paths)
      for (const char *suffix : {"", "-lck"})
        remove((path + suffix).c_str());
  }
  ~files_remover() {
    for (const auto &path : paths)
      for (const char *suffix : {"", "-lck"})
        remove((path + suffix).c_str());
  }
};

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const char *value) {
  MDBX_val k = {const_cast<void *>(static_cast<const void *>(key)), strlen(key)};
  MDBX_val v = {const_cast<void *>(static_cast<const void *>(value)), strlen(value)};
  ASSERT_EQ(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
}

void fill_and_commit(MDBX_env *env, MDBX_dbi &dbi) {
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "t", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  char key[32];
  for (int i = 0; i < kRows; ++i) {
    snprintf(key, sizeof(key), "k%08d", i);
    must_put(txn.get(), dbi, key, "valuevaluevaluevaluevaluevaluevaluevaluevaluevalue");
  }
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
}

/* Opens the database and checks the "t"/"u" naming after an aborted rename:
 * the old name must resolve to the table, the new one must not exist. */
void verify_names(const std::string &db) {
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi d2 = 0;
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "t", MDBX_DB_ACCEDE, &d2), MDBX_SUCCESS);
  MDBX_val k = {const_cast<void *>(static_cast<const void *>("k00000001")), 9}, v;
  ASSERT_EQ(mdbx_get(txn.get(), d2, &k, &v), MDBX_SUCCESS);
  EXPECT_EQ(v.iov_len, strlen("valuevaluevaluevaluevaluevaluevaluevaluevaluevalue"));
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "u", MDBX_DB_ACCEDE, &d2), MDBX_NOTFOUND)
      << "the aborted rename must not persist the new name";
}

TEST(issue_gh0307, top_level_rename_abort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307-a.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  fill_and_commit(env.get(), dbi);

  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_rename(txn.get(), dbi, "u"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(txn.release()), MDBX_SUCCESS);

  /* the handle of the renamed table must stay usable via the old name */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "t", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  MDBX_val k = {const_cast<void *>(static_cast<const void *>("k00000001")), 9}, v;
  ASSERT_EQ(mdbx_get(txn.get(), dbi, &k, &v), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  env.close(); /* the second env cannot share the NOSUBDIR lock */
  verify_names(db);
}

TEST(issue_gh0307, nested_rename_abort_parent_commit) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307-b.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  fill_and_commit(env.get(), dbi);

  /* the table is made dirty in the parent, the child renames it and is
   * aborted, the parent commits: the committed state must keep "t" intact */
  txn_ptr parent;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &parent), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(parent.get(), "t", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  must_put(parent.get(), dbi, "k00000000", "v2");
  txn_ptr child;
  ASSERT_EQ(mdbx_txn_begin(env.get(), parent.get(), MDBX_TXN_READWRITE, &child), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_rename(child.get(), dbi, "u"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(child.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(parent.release()), MDBX_SUCCESS);
  env.close(); /* the second env cannot share the NOSUBDIR lock */
  verify_names(db);
}

TEST(issue_gh0307, rename_commit_is_unchanged) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307-c.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  fill_and_commit(env.get(), dbi);

  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_rename(txn.get(), dbi, "u"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);

  txn_ptr rd;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &rd), MDBX_SUCCESS);
  MDBX_dbi d2 = 0;
  ASSERT_EQ(mdbx_dbi_open(rd.get(), "u", MDBX_DB_ACCEDE, &d2), MDBX_SUCCESS);
  MDBX_val k = {const_cast<void *>(static_cast<const void *>("k00000001")), 9}, v;
  ASSERT_EQ(mdbx_get(rd.get(), d2, &k, &v), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(rd.get(), "t", MDBX_DB_ACCEDE, &d2), MDBX_NOTFOUND);
}

/* Checks the "present"/"absent" naming from a fresh read-only transaction:
 * the expected-present name must resolve to the table with its first row
 * readable, the expected-absent one must not open. */
void verify_two_names(const std::string &db, const char *present, const char *absent) {
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi d = 0;
  ASSERT_EQ(mdbx_dbi_open(txn.get(), present, MDBX_DB_ACCEDE, &d), MDBX_SUCCESS);
  MDBX_val k = {const_cast<void *>(static_cast<const void *>("k00000001")), 9}, v;
  ASSERT_EQ(mdbx_get(txn.get(), d, &k, &v), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), absent, MDBX_DB_ACCEDE, &d), MDBX_NOTFOUND)
      << "the absent name must not resolve after the aborted renames";
}

TEST(issue_gh0307, top_level_double_rename_abort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307-d.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  fill_and_commit(env.get(), dbi);

  /* two renames of the same slot in one transaction, then abort: the undo
   * chain must unwind in reverse chronological order, leaving the original
   * name with its data and neither interim name behind */
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_rename(txn.get(), dbi, "u"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_rename(txn.get(), dbi, "v"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(txn.release()), MDBX_SUCCESS);
  env.close();
  verify_two_names(db, "t", "u");
  verify_two_names(db, "t", "v");
}

TEST(issue_gh0307, nested_rename_commit_parent_abort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307-e.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  fill_and_commit(env.get(), dbi);

  /* the nested transaction renames and commits, transferring the undo to
   * the parent; the parent then aborts — the whole timeline rolls back,
   * so the old name must survive */
  txn_ptr parent;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &parent), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(parent.get(), "t", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  txn_ptr child;
  ASSERT_EQ(mdbx_txn_begin(env.get(), parent.get(), MDBX_TXN_READWRITE, &child), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_rename(child.get(), dbi, "u"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(child.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(parent.release()), MDBX_SUCCESS);
  env.close();
  verify_two_names(db, "t", "u");
}

TEST(issue_gh0307, create_rename_abort_recreate) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307-f.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  /* a table created and renamed in one aborted transaction: the slot is
   * slain on abort, the rename undo must not break the later re-create of
   * the original name */
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "s", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  must_put(txn.get(), dbi, "row", "1");
  ASSERT_EQ(mdbx_dbi_rename(txn.get(), dbi, "s2"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(txn.release()), MDBX_SUCCESS);

  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "s", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  must_put(txn.get(), dbi, "row", "2");
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  env.close(); /* the second env cannot share the NOSUBDIR lock */

  env_ptr env2;
  ASSERT_EQ(mdbx_env_create(&env2), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env2.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env2.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_begin(env2.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "s", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  MDBX_val k = {const_cast<void *>(static_cast<const void *>("row")), 3}, v;
  ASSERT_EQ(mdbx_get(txn.get(), dbi, &k, &v), MDBX_SUCCESS);
  EXPECT_EQ(v.iov_len, strlen("2"));
  must_put(txn.get(), dbi, "row", "3");
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "s2", MDBX_DB_ACCEDE, &dbi), MDBX_NOTFOUND);
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
}

/* Общий каркас: таблица "u" с данными; вложенная транзакция удаляет её
 * (SLAIN-слот), nested_join() хвостом вызывает dbi_update(nested, true) —
 * раннее env-закрытие слота; затем родитель коммитится (pp7) или
 * откатывается (pp8). Пинит, что раннее закрытие SLAIN-слота не теряет и
 * не портит ни данные, ни открываемость таблицы. */
static void drop_in_nested_scenario(const std::string &db, bool parent_commit) {
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  txn_ptr txn;
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "u", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  char key[32], val[64];
  for (int i = 0; i < 500; ++i) {
    snprintf(key, sizeof(key), "k%08d", i);
    snprintf(val, sizeof(val), "value-%08d", i);
    MDBX_val k = {key, strlen(key)}, v = {val, strlen(val)};
    ASSERT_EQ(mdbx_put(txn.get(), dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);

  /* родитель + вложенная, удаляющая таблицу */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "u", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  txn_ptr nested;
  ASSERT_EQ(mdbx_txn_begin(env.get(), txn.get(), MDBX_TXN_READWRITE, &nested), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_drop(nested.get(), dbi, true), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(nested.release()), MDBX_SUCCESS);

  if (parent_commit)
    ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  else
    ASSERT_EQ(mdbx_txn_abort(txn.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env.release()), MDBX_SUCCESS);

  /* reopen: состояние таблицы соответствует исходу родителя */
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  const int rc = mdbx_dbi_open(txn.get(), "u", MDBX_DB_ACCEDE, &dbi);
  if (parent_commit) {
    EXPECT_EQ(rc, MDBX_NOTFOUND) << "the dropped table must stay dropped";
  } else {
    ASSERT_EQ(rc, MDBX_SUCCESS) << "the parent's abort must restore the table";
    size_t count = 0;
    MDBX_cursor *cursor = nullptr;
    ASSERT_EQ(mdbx_cursor_open(txn.get(), dbi, &cursor), MDBX_SUCCESS);
    MDBX_val k, v;
    while (mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT) == MDBX_SUCCESS)
      ++count;
    mdbx_cursor_close(cursor);
    EXPECT_EQ(count, 500u) << "the rollback must keep every record";
  }
  mdbx_txn_abort(txn.release());
}

TEST(issue_gh0307, pp7_nested_drop_commit_then_parent_commit) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307d.mdbx";
  const files_remover cleanup{db};
  drop_in_nested_scenario(db, true);
}

TEST(issue_gh0307, pp8_nested_drop_commit_then_parent_abort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307e.mdbx";
  const files_remover cleanup{db};
  drop_in_nested_scenario(db, false);
}

/* pp9: rename и drop в одной вложенной транзакции — один вызов
 * dbi_update(nested, true) в nested_join() разряжает и SLAIN-слот, и
 * undo-цепочку (передачей родителю); затем abort родителя откатывает
 * и переименование (undo применяется), и удаление (tree rollback). */
TEST(issue_gh0307, pp9_nested_drop_and_rename_commit_parent_abort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0307f.mdbx";
  const files_remover cleanup{db};
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  txn_ptr txn;
  MDBX_dbi u = 0, t2 = 0;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "u", MDBX_CREATE, &u), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "t2", MDBX_CREATE, &t2), MDBX_SUCCESS);
  char key[32], val[64];
  for (int i = 0; i < 500; ++i) {
    snprintf(key, sizeof(key), "k%08d", i);
    snprintf(val, sizeof(val), "value-%08d", i);
    MDBX_val k = {key, strlen(key)}, v = {val, strlen(val)};
    ASSERT_EQ(mdbx_put(txn.get(), u, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_put(txn.get(), t2, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);

  /* одна вложенная транзакция: drop "u" + rename "t2" -> "t3" */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "u", MDBX_DB_ACCEDE, &u), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "t2", MDBX_DB_ACCEDE, &t2), MDBX_SUCCESS);
  txn_ptr nested;
  ASSERT_EQ(mdbx_txn_begin(env.get(), txn.get(), MDBX_TXN_READWRITE, &nested), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_drop(nested.get(), u, true), MDBX_SUCCESS);
  const MDBX_val new_name = {const_cast<void *>(static_cast<const void *>("t3")), 2};
  ASSERT_EQ(mdbx_dbi_rename2(nested.get(), t2, &new_name), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_commit(nested.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(txn.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env.release()), MDBX_SUCCESS);

  /* reopen: abort родителя откатил всё — "u" и "t2" на месте, "t3" нет */
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "u", MDBX_DB_ACCEDE, &u), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "t2", MDBX_DB_ACCEDE, &t2), MDBX_SUCCESS);
  size_t count_u = 0, count_t2 = 0;
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn.get(), u, &cursor), MDBX_SUCCESS);
  MDBX_val k, v;
  while (mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT) == MDBX_SUCCESS)
    ++count_u;
  mdbx_cursor_close(cursor);
  ASSERT_EQ(mdbx_cursor_open(txn.get(), t2, &cursor), MDBX_SUCCESS);
  while (mdbx_cursor_get(cursor, &k, &v, MDBX_NEXT) == MDBX_SUCCESS)
    ++count_t2;
  mdbx_cursor_close(cursor);
  EXPECT_EQ(count_u, 500u);
  EXPECT_EQ(count_t2, 500u);
  EXPECT_EQ(mdbx_dbi_open(txn.get(), "t3", MDBX_DB_ACCEDE, &u), MDBX_NOTFOUND);
  mdbx_txn_abort(txn.release());
}

} // namespace
