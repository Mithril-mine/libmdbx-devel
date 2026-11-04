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

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/338
// The MDBX_envinfo::mi_mode field is documented as "Current environment mode,
// the same as mdbx_env_get_flags() returns", but it reported the mode recorded in
// the lock file by the first process that opened the database for writing: it was
// masked to the so-called mode flags, so changeable flags set at open were missing,
// and later mdbx_env_set_flags() calls were not reflected at all. An environment
// without a lock file (MDBX_EXCLUSIVE) reported the raw internal flags instead.

namespace {

const char *const kPath = "issue338.mdbx";
const MDBX_env_flags_t kDurable = static_cast<MDBX_env_flags_t>(0);

void remove_db() {
  std::remove(kPath);
  std::remove((std::string(kPath) + "-lck").c_str());
}

struct db_remover {
  db_remover() { remove_db(); }
  ~db_remover() { remove_db(); }
};

unsigned get_flags(MDBX_env *env) {
  unsigned flags = 0;
  EXPECT_EQ(mdbx_env_get_flags(env, &flags), MDBX_SUCCESS);
  return flags;
}

MDBX_envinfo get_info(MDBX_env *env) {
  MDBX_envinfo info;
  memset(&info, 0, sizeof(info));
  EXPECT_EQ(mdbx_env_info_ex(env, nullptr, &info, sizeof(info)), MDBX_SUCCESS);
  return info;
}

MDBX_env *open_env(MDBX_env_flags_t extra) {
  MDBX_env *env = nullptr;
  EXPECT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  EXPECT_NE(env, nullptr);
  if (env && mdbx_env_open(env, kPath, MDBX_NOSUBDIR | extra, 0640) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

#if !defined(_WIN32)
/* A child process that opens the database with the given flags, reports its
 * mdbx_env_open() result through the "ready" file, holds the environment
 * until the "go" file appears, and exits.
 *
 * The report is published by an atomic rename(), so the existence of the
 * "ready" file implies its content is complete: a plain create-then-write
 * made the file visible (and thus readable) while still empty, so under CI
 * scheduling the parent could get a short read and mistake it for a timeout.
 *
 * The holder releases the child on every exit path of a test, including
 * gtest ASSERT failures, otherwise an orphaned child would hold the
 * environment and its locks forever. */
struct holder_child {
  pid_t pid = -1;
  std::string ready, pending, go;
  int open_rc = MDBX_EIO;

  holder_child() = default;
  holder_child(holder_child &&o)
      : pid(o.pid), ready(std::move(o.ready)), pending(std::move(o.pending)), go(std::move(o.go)), open_rc(o.open_rc) {
    o.pid = -1;
  }
  holder_child(const holder_child &) = delete;
  holder_child &operator=(const holder_child &) = delete;

  void release() {
    if (pid != -1) {
      if (FILE *f = fopen(go.c_str(), "wb"))
        fclose(f);
      int status = 0;
      waitpid(pid, &status, 0);
      pid = -1;
    }
    std::remove(ready.c_str());
    std::remove(pending.c_str());
    std::remove(go.c_str());
  }

  ~holder_child() { release(); }
};

holder_child spawn_holder(const char *tag, MDBX_env_flags_t flags) {
  holder_child c;
  c.ready = std::string(kPath) + "." + tag + ".ready";
  c.pending = c.ready + ".pending";
  c.go = std::string(kPath) + "." + tag + ".go";
  std::remove(c.ready.c_str());
  std::remove(c.pending.c_str());
  std::remove(c.go.c_str());
  c.pid = fork();
  if (c.pid == 0) {
    MDBX_env *env = nullptr;
    int rc =
        mdbx_env_create(&env) == MDBX_SUCCESS ? mdbx_env_open(env, kPath, MDBX_NOSUBDIR | flags, 0640) : MDBX_EBADSIGN;
    if (FILE *f = fopen(c.pending.c_str(), "wb")) {
      if (fwrite(&rc, sizeof(rc), 1, f) != 1)
        rc = MDBX_EIO;
      if (fclose(f) != 0 || rename(c.pending.c_str(), c.ready.c_str()) != 0)
        rc = MDBX_EIO;
    } else
      rc = MDBX_EIO;
    if (rc == MDBX_SUCCESS) {
      while (access(c.go.c_str(), F_OK) != 0)
        usleep(1000);
      mdbx_env_close(env);
    }
    _exit(rc == MDBX_SUCCESS ? 0 : 4);
  }
  for (int i = 0; i < 10000 && access(c.ready.c_str(), F_OK) != 0; ++i)
    usleep(1000);
  if (FILE *f = fopen(c.ready.c_str(), "rb")) {
    if (fread(&c.open_rc, sizeof(c.open_rc), 1, f) != 1)
      c.open_rc = MDBX_EIO;
    fclose(f);
  }
  return c;
}
#endif /* !Windows */

} // namespace

/* mi_mode must equal mdbx_env_get_flags() right after the open. */
TEST(issue_gh0338, mi_mode_matches_get_flags_on_open) {
  db_remover guard;
  MDBX_env *env = open_env(MDBX_WRITEMAP);
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  mdbx_env_close(env);
}

/* Changeable flags set at open must be reported as well. */
TEST(issue_gh0338, mi_mode_includes_changeable_flags) {
  db_remover guard;
  MDBX_env *env = open_env(MDBX_NOMEMINIT);
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  mdbx_env_close(env);
}

/* Runtime flag changes via mdbx_env_set_flags() must be reflected. */
TEST(issue_gh0338, mi_mode_tracks_set_flags) {
  db_remover guard;
  MDBX_env *env = open_env(MDBX_WRITEMAP);
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_NOMETASYNC, true), MDBX_SUCCESS);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_NOMETASYNC, false), MDBX_SUCCESS);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_NOMEMINIT, true), MDBX_SUCCESS);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  mdbx_env_close(env);
}

/* A process working alone may switch the sync mode at runtime in both
 * directions; the change must be published to the lock file, so that a
 * later process with MDBX_WRITEMAP is refused at open exactly as if the
 * lazy mode was set at open (the original complaint of the issue). */
TEST(issue_gh0338, set_flags_sync_mode_switch_when_alone) {
  db_remover guard;
  MDBX_env *env = open_env(kDurable);
  ASSERT_NE(env, nullptr);
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, true), MDBX_SUCCESS);
  EXPECT_NE(get_flags(env) & MDBX_SAFE_NOSYNC, 0u);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));

#if !defined(_WIN32)
  /* while the environment is still open, another process with MDBX_WRITEMAP
   * must be refused: the lazy mode published by the runtime switch is what
   * the open-time check compares against (the original complaint) */
  holder_child refused = spawn_holder("writemap", MDBX_WRITEMAP);
  EXPECT_EQ(refused.open_rc, MDBX_INCOMPATIBLE);
  refused.release();
#endif

  mdbx_env_close(env);
  /* the lazy mode must be adopted by the next opener and switchable back */
  env = open_env(MDBX_SAFE_NOSYNC);
  ASSERT_NE(env, nullptr);
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, false), MDBX_SUCCESS);
  EXPECT_EQ(get_flags(env) & MDBX_SAFE_NOSYNC, 0u);
  mdbx_env_close(env);
  MDBX_env *again = open_env(kDurable);
  EXPECT_NE(again, nullptr);
  if (again)
    mdbx_env_close(again);
}

/* While another process is attached, a runtime change that would create the
 * mode mixing refused at mdbx_env_open() must be refused as well, in both
 * directions; MDBX_NOMETASYNC stays changeable since it takes no part in
 * the rigorous conditions. */
TEST(issue_gh0338, set_flags_refuses_switch_with_other_process) {
#if !defined(_WIN32)
  db_remover guard;
  MDBX_env *env = open_env(kDurable);
  ASSERT_NE(env, nullptr);
  const unsigned before = get_flags(env);

  holder_child holder = spawn_holder("holder", kDurable);
  ASSERT_EQ(holder.open_rc, MDBX_SUCCESS);
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, true), MDBX_INCOMPATIBLE);
  EXPECT_EQ(get_flags(env), before);
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_NOMETASYNC, true), MDBX_SUCCESS);
  holder.release();
  mdbx_env_close(env);

  /* the same in the opposite direction: a lazy environment with an attached
   * lazy process must refuse to become durable at runtime */
  env = open_env(MDBX_SAFE_NOSYNC);
  ASSERT_NE(env, nullptr);
  holder_child lazy_holder = spawn_holder("lazy-holder", MDBX_SAFE_NOSYNC);
  ASSERT_EQ(lazy_holder.open_rc, MDBX_SUCCESS);
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, false), MDBX_INCOMPATIBLE);
  lazy_holder.release();
  mdbx_env_close(env);
#else
  GTEST_SKIP() << "requires fork()";
#endif
}

/* MDBX_NOMETASYNC does not participate in the rigorous conditions and must
 * stay runtime-changeable, with the change published to the lock file. */
TEST(issue_gh0338, set_flags_allows_nometasync_toggle) {
  db_remover guard;
  MDBX_env *env = open_env(kDurable);
  ASSERT_NE(env, nullptr);
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_NOMETASYNC, true), MDBX_SUCCESS);
  EXPECT_NE(get_flags(env) & MDBX_NOMETASYNC, 0u);
  EXPECT_EQ(get_info(env).mi_mode, get_flags(env));
  ASSERT_EQ(mdbx_env_set_flags(env, MDBX_NOMETASYNC, false), MDBX_SUCCESS);
  EXPECT_EQ(get_flags(env) & MDBX_NOMETASYNC, 0u);
  mdbx_env_close(env);
  /* the recorded mode must stay usable for the next opener */
  MDBX_env *again = open_env(kDurable);
  EXPECT_NE(again, nullptr);
  if (again)
    mdbx_env_close(again);
}

/* The sync mode is freely selectable before mdbx_env_open(). */
TEST(issue_gh0338, set_flags_sync_mode_before_open) {
  db_remover guard;
  MDBX_env *env = nullptr;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, true), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, false), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, true), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env, kPath, MDBX_NOSUBDIR, 0640), MDBX_SUCCESS);
  EXPECT_NE(get_flags(env) & MDBX_SAFE_NOSYNC, 0u);
  /* working alone, the runtime switch back to the durable mode is allowed */
  EXPECT_EQ(mdbx_env_set_flags(env, MDBX_SAFE_NOSYNC, false), MDBX_SUCCESS);
  mdbx_env_close(env);
}
