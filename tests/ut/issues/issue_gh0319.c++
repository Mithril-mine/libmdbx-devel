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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#ifndef _WIN32
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/319 "API contract deviations (15 items)"

namespace {

const char *const kDb = "issue319.mdbx";
const int kMainDbi = 1;

void remove_db() {
  std::remove(kDb);
  std::remove((std::string(kDb) + MDBX_LOCK_SUFFIX).c_str());
}

struct db_remover {
  const char *path;
  explicit db_remover(const char *p) : path(p) { remove_db(); }
  ~db_remover() { remove_db(); }
};

MDBX_env *open_env(const char *path, MDBX_env_flags_t extra_flags) {
  MDBX_env *env = nullptr;
  EXPECT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  EXPECT_NE(env, nullptr);
  if (env) {
    EXPECT_EQ(mdbx_env_set_maxdbs(env, 4), MDBX_SUCCESS);
/* a 1 GiB upper bound is beyond the win32 limits */
#if MDBX_WORDBITS >= 64
    const size_t geometry_upper = 1024u << 20;
#else
    const size_t geometry_upper = 256u << 20;
#endif
    EXPECT_EQ(mdbx_env_set_geometry(env, -1, 1 << 20, geometry_upper, 1 << 20, -1, 4096), MDBX_SUCCESS);
    if (mdbx_env_open(env, path, MDBX_NOSUBDIR | extra_flags, 0644) != MDBX_SUCCESS) {
      mdbx_env_close(env);
      return nullptr;
    }
  }
  return env;
}

MDBX_val val_of(const char *s) {
  MDBX_val v = {(void *)s, strlen(s)};
  return v;
}

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *k, const char *v) {
  MDBX_val key = val_of(k), data = val_of(v);
  ASSERT_EQ(mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT), MDBX_SUCCESS);
}

} // namespace

//-----------------------------------------------------------------------------

/* Item 1: mdbx_cursor_renew(NULL, mc) must not dereference the null txn. */
TEST(issue_gh0319, pp1_renew_with_null_txn) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_cursor *cursor = mdbx_cursor_create(nullptr);
  ASSERT_NE(cursor, nullptr);
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_bind(txn, cursor, kMainDbi), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_abort(txn), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_renew(nullptr, cursor), MDBX_EINVAL);
  mdbx_cursor_close(cursor);
  mdbx_env_close(env);
}

/* Item 2: mdbx_env_info_ex() on an environment that is not open must zero
   the fields it does not fill. */
TEST(issue_gh0319, pp2_info_of_unopened_env) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  MDBX_envinfo info;
  memset(&info, -1, sizeof(info)); /* fill with 0xFF bytes */
  EXPECT_EQ(mdbx_env_info_ex(env, nullptr, &info, sizeof(info)), MDBX_SUCCESS);
  EXPECT_EQ(info.mi_recent_txnid, 0u);
  EXPECT_EQ(info.mi_mode, 0u);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}

/* Item 3: MDBX_NEXT_MULTIPLE/PREV_MULTIPLE require MDBX_DUPFIXED. */
TEST(issue_gh0319, pp3_multiple_requires_dupfixed) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  for (int i = 0; i < 5; ++i) {
    char v[16];
    snprintf(v, sizeof(v), "v%d", i);
    must_put(txn, dbi, "k", v);
  }
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("k"), data = val_of("v0");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_MULTIPLE), MDBX_INCOMPATIBLE);
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_PREV_MULTIPLE), MDBX_INCOMPATIBLE);
  /* the cursor must stay usable for the documented single-value ops */
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_DUP), MDBX_SUCCESS);
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 4: mdbx_estimate_range() must honor begin_data/end_data, and
   mdbx_estimate_move() must treat MDBX_RESULT_TRUE as success. */
TEST(issue_gh0319, pp4_estimate_honors_data_bounds) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  char buf[32];
  for (int i = 0; i < 1000; ++i) {
    snprintf(buf, sizeof(buf), "%06dxxxxxxxxxxxxx", i);
    must_put(txn, dbi, "k", buf);
  }
  must_put(txn, dbi, "m", "1");

  ptrdiff_t distance = -7;
  MDBX_val begin_key = val_of("k"), end_key = val_of("k");
  MDBX_val begin_data = val_of("000000"), end_data = val_of("000900");
  EXPECT_EQ(mdbx_estimate_range(txn, dbi, &begin_key, &begin_data, &end_key, &end_data, &distance), MDBX_SUCCESS);
  EXPECT_GT(distance, 0) << "the data bounds were ignored";
  EXPECT_LT(distance, 1000);

  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("a"), data = val_of("1");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST), MDBX_SUCCESS);
  distance = -7;
  key = val_of("l");
  data = val_of("");
  EXPECT_EQ(mdbx_estimate_move(cursor, &key, &data, MDBX_SET_LOWERBOUND, &distance), MDBX_SUCCESS);
  EXPECT_EQ(distance, 1) << "MDBX_RESULT_TRUE of the move was treated as a failure";
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 5: mdbx_cursor_scan() with MDBX_NEXT_DUP over keys with a single value
   each must return MDBX_RESULT_FALSE, not MDBX_ENODATA. */
static int scan_predicate_never_matches(void *, MDBX_val *, MDBX_val *, void *) noexcept { return MDBX_RESULT_FALSE; }

TEST(issue_gh0319, pp5_scan_next_dup_single_values) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "a", "1");
  must_put(txn, dbi, "b", "1");
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_scan(cursor, scan_predicate_never_matches, nullptr, MDBX_FIRST, MDBX_NEXT_DUP, nullptr),
            MDBX_RESULT_FALSE);
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 6: MDBX_opt_txn_dp_initial set before open must not be truncated. */
TEST(issue_gh0319, pp6_dp_initial_not_truncated) {
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_option(env, MDBX_opt_txn_dp_initial, 4096), MDBX_SUCCESS);
  uint64_t got = 0;
  ASSERT_EQ(mdbx_env_get_option(env, MDBX_opt_txn_dp_initial, &got), MDBX_SUCCESS);
  EXPECT_EQ(got, 4096u);
  EXPECT_EQ(mdbx_env_close(env), MDBX_SUCCESS);
}

#ifndef _WIN32
/* Item 7: mvcc_bind_slot() must not return MDBX_READERS_FULL after it has
   just freed dead slots. The reader table's effective capacity is rounded
   up by the lock file setup (src/lck.c), so enough children are spawned to
   occupy every slot; the parent verifies MDBX_READERS_FULL while the
   children are alive and holds its environment open across the kills.
   POSIX-only (fork + kill). */

/* Waits a single byte within the timeout, for the fork() handshake below. */
static bool wait_byte(int fd, char *out) {
  struct pollfd pfd = {(decltype(pollfd::fd))fd, POLLIN, 0};
  if (poll(&pfd, 1, 10000) != 1)
    return false;
  return read(fd, out, 1) == 1;
}

TEST(issue_gh0319, pp7_readers_full_after_dead_cleanup) {
  db_remover guard(kDb);
  MDBX_env *parent = nullptr;
  ASSERT_EQ(mdbx_env_create(&parent), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxreaders(parent, 2), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(parent, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  mdbx_env_close(parent);

  /* The lock file rounds its size up to the system page, so the effective
   * slot capacity may be well above maxreaders; 512 children are enough to
   * occupy the whole table at any plausible rounding. */
  constexpr int kChildren = 512;
  int ready[2], done[2];
  ASSERT_NE(pipe(ready), -1);
  ASSERT_NE(pipe(done), -1);
  fflush(nullptr);
  pid_t pids[kChildren];
  int started = 0;
  for (int i = 0; i < kChildren; ++i) {
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      MDBX_env *child = nullptr;
      char c = 0;
      const bool opened = mdbx_env_create(&child) == MDBX_SUCCESS && child != nullptr &&
                          mdbx_env_set_maxreaders(child, 2) == MDBX_SUCCESS &&
                          mdbx_env_open(child, kDb, MDBX_NOSUBDIR, 0644) == MDBX_SUCCESS;
      MDBX_txn *txn = nullptr;
      if (opened && mdbx_txn_begin(child, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn) == MDBX_SUCCESS) {
        c = 1;
        if (write(ready[1], &c, 1) != 1)
          _exit(3);
        /* the parent kills us while we hold the reader slot; do not exit */
        if (read(done[0], &c, 1) < 0)
          _exit(3);
        mdbx_txn_abort(txn);
      } else {
        c = 0;
        if (write(ready[1], &c, 1) != 1)
          _exit(3);
      }
      if (child)
        mdbx_env_close(child);
      _exit(opened ? 0 : 2);
    }
    pids[i] = pid;
    char c = 0;
    if (wait_byte(ready[0], &c) && c == 1)
      ++started;
  }

  /* the parent's environment must be opened while the children are alive
     and hold the reader slots */
  MDBX_env *env = nullptr;
  ASSERT_GT(started, 4) << "not enough children started their read transactions";
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxreaders(env, 2), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);
  MDBX_txn *probe = nullptr;
  EXPECT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &probe), MDBX_READERS_FULL)
      << "precondition: every reader slot must be occupied by the alive children";
  if (probe) {
    mdbx_txn_abort(probe);
    probe = nullptr;
  }

  /* kill all the readers while the completion pipe stays unsignaled, then
     reap them: their slots become dead-but-taken */
  for (int i = 0; i < kChildren; ++i)
    kill(pids[i], SIGKILL);
  ASSERT_EQ(close(done[1]), 0);
  for (int i = 0; i < kChildren; ++i) {
    int wstatus = 0;
    EXPECT_EQ(waitpid(pids[i], &wstatus, 0), pids[i]);
  }
  ASSERT_EQ(close(done[0]), 0);

  /* before the fix: the first txn_begin() returned MDBX_READERS_FULL after
     mvcc_cleanup_dead() had just freed the dead slots */
  MDBX_txn *txn = nullptr;
  EXPECT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  if (txn)
    mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db();
}
#endif /* !_WIN32 */

/* Item 8: the internal node flags N_TREE (2) and N_DUP (4) must be rejected
   by mdbx_cursor_put()/mdbx_cursor_del(). */
TEST(issue_gh0319, pp8_internal_node_flags_rejected) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "x", "y");
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, kMainDbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("t"), data;
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET_KEY), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)2), MDBX_EINVAL);
  key = val_of("a");
  data = val_of("b");
  EXPECT_EQ(mdbx_cursor_put(cursor, &key, &data, (MDBX_put_flags_t)4), MDBX_EINVAL);
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 11: mdbx_replace() must reject MDBX_ALLDUPS and other meaningless
   flags combinations, while the multi-value mode (MDBX_CURRENT |
   MDBX_NOOVERWRITE with the value filled in old_data) keeps working. */
TEST(issue_gh0319, pp11_replace_flags_validation) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dup, plain;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "dup", MDBX_CREATE | MDBX_DUPSORT, &dup), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "plain", MDBX_CREATE, &plain), MDBX_SUCCESS);
  must_put(txn, dup, "k", "1");
  must_put(txn, dup, "k", "2");
  must_put(txn, dup, "k", "3");
  must_put(txn, dup, "s", "a");
  must_put(txn, plain, "p", "v");

  char buffer[16];
  MDBX_val key = val_of("k"), data = val_of("9");
  MDBX_val old;
  old.iov_base = buffer;
  old.iov_len = sizeof(buffer);

  /* MDBX_ALLDUPS is rejected: with several values, a single one and alone. */
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_ALLDUPS)), MDBX_EINVAL);
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, MDBX_ALLDUPS), MDBX_EINVAL);
  key = val_of("s");
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_ALLDUPS)), MDBX_EINVAL);

  /* The internal node flags (see item 8) and other unknown bits. */
  key = val_of("k");
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)2), MDBX_EINVAL);
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)4), MDBX_EINVAL);

  /* No appending while updating, no extras for deletion. */
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_APPEND)), MDBX_EINVAL);
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_APPENDDUP)), MDBX_EINVAL);
  EXPECT_EQ(mdbx_replace(txn, dup, &key, nullptr, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_NODUPDATA)),
            MDBX_EINVAL);
  /* Extraction requires MDBX_CURRENT, as documented. */
  key = val_of("p");
  EXPECT_EQ(mdbx_replace(txn, plain, &key, nullptr, &old, (MDBX_put_flags_t)0), MDBX_EINVAL);

  /* The same buffer cannot serve both for the old and the new data. */
  MDBX_val aliased_new;
  aliased_new.iov_base = buffer;
  aliased_new.iov_len = sizeof(buffer);
  MDBX_val aliased_old = aliased_new;
  EXPECT_EQ(mdbx_replace(txn, plain, &key, &aliased_new, &aliased_old, MDBX_CURRENT), MDBX_EINVAL);

  /* The multi-value mode: update/delete a particular value selected by old_data. */
  key = val_of("k");
  old = val_of("2");
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_NOOVERWRITE)),
            MDBX_SUCCESS);
  old = val_of("3");
  EXPECT_EQ(mdbx_replace(txn, dup, &key, nullptr, &old, (MDBX_put_flags_t)(MDBX_CURRENT | MDBX_NOOVERWRITE)),
            MDBX_SUCCESS);
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dup, &cursor), MDBX_SUCCESS);
  MDBX_val probe = val_of("9");
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &probe, MDBX_GET_BOTH), MDBX_SUCCESS);
  probe = val_of("1");
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &probe, MDBX_GET_BOTH), MDBX_SUCCESS);
  probe = val_of("3");
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &probe, MDBX_GET_BOTH), MDBX_NOTFOUND);
  mdbx_cursor_close(cursor);

  /* The plain update of multi-values is still refused, extraction works. */
  old.iov_base = buffer;
  old.iov_len = sizeof(buffer);
  EXPECT_EQ(mdbx_replace(txn, dup, &key, &data, &old, MDBX_CURRENT), MDBX_EMULTIVAL);
  key = val_of("p");
  EXPECT_EQ(mdbx_replace(txn, plain, &key, nullptr, &old, MDBX_CURRENT), MDBX_SUCCESS);
  EXPECT_EQ(old.iov_len, 1u);
  EXPECT_EQ(memcmp(old.iov_base, "v", 1), 0);
  must_put(txn, plain, "p", "v"); /* the extraction above deleted it */
  data = val_of("w");
  EXPECT_EQ(mdbx_replace(txn, plain, &key, &data, &old, MDBX_CURRENT), MDBX_SUCCESS);

  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 12: MDBX_GET_MULTIPLE returns a whole page of the current key's
   duplicates from its beginning, regardless of the inner cursor position
   (in accord with LMDB), and prepares the cursor for MDBX_NEXT_MULTIPLE. */
TEST(issue_gh0319, pp12_get_multiple_positioning) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "fixed", MDBX_CREATE | MDBX_DUPSORT | MDBX_DUPFIXED, &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "k", "00000001");
  must_put(txn, dbi, "k", "00000002");
  must_put(txn, dbi, "k", "00000003");
  must_put(txn, dbi, "k", "00000004");
  must_put(txn, dbi, "k", "00000005");
  must_put(txn, dbi, "m", "msingle!");

  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("k"), data = val_of("00000003");

  /* position at the 3rd duplicate of "k", then take the batch */
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_GET_BOTH), MDBX_SUCCESS);
  EXPECT_EQ(memcmp(data.iov_base, "00000003", 8), 0);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_GET_MULTIPLE), MDBX_SUCCESS);
  EXPECT_EQ(data.iov_len, 5u * 8u);
  EXPECT_EQ(memcmp(data.iov_base, "00000001", 8), 0);
  /* the batch has consumed the whole key */
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_MULTIPLE), MDBX_NOTFOUND);

  /* SEEK_AND_GET_MULTIPLE positions at the key and returns its first page */
  key = val_of("k");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SEEK_AND_GET_MULTIPLE), MDBX_SUCCESS);
  EXPECT_EQ(data.iov_len, 5u * 8u);
  EXPECT_EQ(memcmp(data.iov_base, "00000001", 8), 0);

  /* a single-value key yields the single value */
  key = val_of("m");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS);
  data.iov_base = nullptr;
  data.iov_len = 0;
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_GET_MULTIPLE), MDBX_SUCCESS);
  EXPECT_EQ(data.iov_len, 8u);
  EXPECT_EQ(memcmp(data.iov_base, "msingle!", 8), 0);

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 13: mdbx_cursor_get_batch() end-of-data codes as documented:
   MDBX_NOTFOUND when there are no pairs after the position or the table is
   empty, MDBX_ENODATA when the cursor has no position to continue from,
   MDBX_RESULT_TRUE for the last chunk. */
TEST(issue_gh0319, pp13_get_batch_end_of_data) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val pairs[8], key, data;
  size_t count = 7;

  /* a fresh cursor has no position to continue from */
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 8, MDBX_NEXT), MDBX_ENODATA);
  EXPECT_EQ(count, 0u);
  /* FIRST on an empty table */
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 8, MDBX_FIRST), MDBX_NOTFOUND);
  EXPECT_EQ(count, 0u);

  must_put(txn, dbi, "a", "1");
  must_put(txn, dbi, "b", "2");
  must_put(txn, dbi, "c", "3");

  /* a limited chunk with more pairs left */
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 4, MDBX_FIRST), MDBX_SUCCESS);
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(memcmp(pairs[0].iov_base, "a", 1), 0);
  EXPECT_EQ(memcmp(pairs[2].iov_base, "b", 1), 0);

  /* the last chunk reaching the end of data */
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 8, MDBX_NEXT), MDBX_RESULT_TRUE);
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(memcmp(pairs[0].iov_base, "c", 1), 0);

  /* nothing after the position */
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 8, MDBX_NEXT), MDBX_NOTFOUND);
  EXPECT_EQ(count, 0u);

  /* right after MDBX_LAST, the issue's case */
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 8, MDBX_NEXT), MDBX_NOTFOUND);
  EXPECT_EQ(count, 0u);

  /* a reset cursor has no position to continue from */
  ASSERT_EQ(mdbx_cursor_reset(cursor), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_get_batch(cursor, &count, pairs, 8, MDBX_NEXT), MDBX_ENODATA);
  EXPECT_EQ(count, 0u);

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 14: mdbx_cursor_on_first()/on_last() (and the _dup variants) report
   MDBX_RESULT_TRUE for a not positioned cursor, i.e. the result tells whether
   moving in the corresponding direction makes sense. */
TEST(issue_gh0319, pp14_on_first_last_unpositioned) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dup, plain;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "dup", MDBX_CREATE | MDBX_DUPSORT, &dup), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "plain", MDBX_CREATE, &plain), MDBX_SUCCESS);

  /* a fresh cursor on an empty table is on the first and the last simultaneously */
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dup, &cursor), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_first_dup(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last_dup(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_eof(cursor), MDBX_RESULT_TRUE);
  /* after reset: the same */
  ASSERT_EQ(mdbx_cursor_reset(cursor), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_TRUE);

  must_put(txn, dup, "k", "1");
  must_put(txn, dup, "k", "2");
  must_put(txn, dup, "k", "3");
  must_put(txn, dup, "z", "9");
  must_put(txn, plain, "a", "1");
  must_put(txn, plain, "b", "2");
  must_put(txn, plain, "c", "3");

  MDBX_val key, data;
  /* positioned on the first pair: backward makes no sense, forward does */
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_FALSE);
  EXPECT_EQ(mdbx_cursor_on_first_dup(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last_dup(cursor), MDBX_RESULT_FALSE);

  /* a middle multi-value: moving makes sense in neither dup-direction,
     and on_first == MDBX_RESULT_FALSE implies a successful backward move */
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_DUP), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first_dup(cursor), MDBX_RESULT_FALSE);
  EXPECT_EQ(mdbx_cursor_on_last_dup(cursor), MDBX_RESULT_FALSE);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_PREV_DUP), MDBX_SUCCESS);
  EXPECT_EQ(memcmp(data.iov_base, "1", 1), 0);

  /* the last pair: forward makes no sense */
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_FALSE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_first_dup(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last_dup(cursor), MDBX_RESULT_TRUE);
  mdbx_cursor_close(cursor);

  /* a plain table: the same for the outer levels, while the _dup variants
     are always MDBX_RESULT_TRUE without MDBX_DUPSORT */
  ASSERT_EQ(mdbx_cursor_open(txn, plain, &cursor), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_FIRST), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_FALSE);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_FALSE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_FALSE);
  EXPECT_EQ(mdbx_cursor_on_first_dup(cursor), MDBX_RESULT_TRUE);
  EXPECT_EQ(mdbx_cursor_on_last_dup(cursor), MDBX_RESULT_TRUE);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_on_first(cursor), MDBX_RESULT_FALSE);
  EXPECT_EQ(mdbx_cursor_on_last(cursor), MDBX_RESULT_TRUE);

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* Item 15: mdbx_dbi_rename() to the same name is a no-op success in all
   build flavors, while renaming to another existing table's name still
   fails with MDBX_KEYEXIST. */
TEST(issue_gh0319, pp15_dbi_rename_same_name) {
  db_remover guard(kDb);
  MDBX_env *env = open_env(kDb, MDBX_env_flags_t(0));
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi t1, t2, check;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t1", MDBX_CREATE, &t1), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t2", MDBX_CREATE, &t2), MDBX_SUCCESS);
  must_put(txn, t1, "k", "v");

  /* renaming to the same name is a no-op success */
  EXPECT_EQ(mdbx_dbi_rename(txn, t1, "t1"), MDBX_SUCCESS);
  MDBX_val key = val_of("k"), data;
  ASSERT_EQ(mdbx_get(txn, t1, &key, &data), MDBX_SUCCESS);
  EXPECT_EQ(memcmp(data.iov_base, "v", 1), 0);

  /* renaming to another existing table's name still fails */
  EXPECT_EQ(mdbx_dbi_rename(txn, t1, "t2"), MDBX_KEYEXIST);

  /* a real rename, then the same-name no-op again */
  EXPECT_EQ(mdbx_dbi_rename(txn, t1, "t3"), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_get(txn, t1, &key, &data), MDBX_SUCCESS);
  EXPECT_EQ(memcmp(data.iov_base, "v", 1), 0);
  EXPECT_EQ(mdbx_dbi_open(txn, "t1", (MDBX_db_flags_t)0, &check), MDBX_NOTFOUND);
  EXPECT_EQ(mdbx_dbi_rename(txn, t1, "t3"), MDBX_SUCCESS);

  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}
