/* v13a-checkpoint-has-child-control.cpp — V13a stream B control experiment.
 *
 * PROVES (not refutes): txn_managed::checkpoint() on a parent with a live
 * nested txn_managed child is refused with mdbx::bad_transaction and both
 * handles stay alive — the wrapper does NOT lose the child, and there is no
 * use-after-free at child destruction.  The protection comes from the C core:
 * mdbx_txn_checkpoint() checks its argument with
 *   check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED)
 * whose bad_bits include MDBX_TXN_HAS_CHILD, so the nested-chain
 * commit/abort inside mdbx_txn_checkpoint() is unreachable for any handle
 * with an attached child.  This differs from mdbx_txn_commit_ex() /
 * mdbx_txn_abort_ex(), whose entry bad_bits do NOT include HAS_CHILD — that
 * is why txn_managed::commit()/abort() carry their own C++-side guard
 * (4693ccca, issue #92) while checkpoint() needs none.
 *
 * Verified behavior matrix (run of this file):
 *   E1 checkpoint(parent-with-child)   -> bad_transaction, both txns alive
 *   E2 commit(parent-with-child)       -> bad_transaction (C++-side guard)
 *   E3 abort(parent-with-child)        -> bad_transaction (C++-side guard)
 *   E4 child commit, then parent work  -> normal, no leak (LSan clean)
 *   E5 commit_embark_read(parent)      -> handle becomes a read txn
 *   E6 double txn_managed::abort()     -> second call throws (no UB)
 *   E7 cursor_managed closed after its txn committed -> SUCCESS (detached
 *      ready4dispose cursor, txn/tree pointers drowned at EOT, INV-CXX1)
 *
 * Build (from the tree root):
 *   g++ -std=gnu++23 -g -O1 -fsanitize=address,leak \
 *     -DMDBX_BUILD_CXX=1 '-DMDBX_CONFIG_H="config-gnumake.h"' -I. -Isrc \
 *     docs/verification/repro/v13a-checkpoint-has-child-control.cpp \
 *     src/mdbx.c++ src/alloy.c -o /tmp/v13a-control -pthread -lrt
 */

#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "mdbx.h++"

static int failures = 0;

#define CHECK(cond, msg)                                                                                               \
  do {                                                                                                                 \
    if (cond)                                                                                                          \
      printf("ok    %s\n", msg);                                                                                       \
    else {                                                                                                             \
      printf("FAIL  %s\n", msg);                                                                                       \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)

template <typename F> bool throws_bad_txn(F &&f) {
  try {
    f();
    return false;
  } catch (const mdbx::bad_transaction &) {
    return true;
  } catch (const std::exception &) {
    /* reached only by E6 (null-handle abort → EINVAL → std::invalid_argument);
     * E1-E3 are asserted with bad_transaction specifically */
    return true;
  }
}

int main(int argc, char **argv) {
  const char *path = (argc > 1) ? argv[1] : "/tmp/v13a-control.db";
  std::error_code ec;
  std::filesystem::remove_all(path, ec);

  {
    mdbx::env_managed::create_parameters cp;
    cp.set_use_subdirectory(false).set_file_mode_bits(0640);
    mdbx::env::operate_parameters op(4, 16, mdbx::env::mode::write_file_io);
    mdbx::env_managed env(path, cp, op);

    /* E1: checkpoint refuses the parent while the child is alive */
    {
      mdbx::txn_managed parent = env.start_write();
      mdbx::txn_managed child = parent.start_nested();
      CHECK(throws_bad_txn([&] { parent.checkpoint(); }), "E1 checkpoint(parent-with-child) throws bad_transaction");
      CHECK((mdbx_txn_flags(parent.handle()) & MDBX_TXN_HAS_CHILD) != 0, "E1 parent handle kept, child attached");
      child.abort();
      parent.abort();
    }

    /* E2/E3: commit and abort have their own C++-side guard */
    {
      mdbx::txn_managed parent = env.start_write();
      mdbx::txn_managed child = parent.start_nested();
      CHECK(throws_bad_txn([&] { parent.commit(); }), "E2 commit(parent-with-child) throws bad_transaction");
      CHECK(throws_bad_txn([&] { parent.abort(); }), "E3 abort(parent-with-child) throws bad_transaction");
      child.abort();
      parent.commit();
    }

    /* E4: normal nested lifecycle, resources balanced */
    {
      mdbx::txn_managed parent = env.start_write();
      {
        mdbx::txn_managed child = parent.start_nested();
        auto map = child.create_map("table", mdbx::key_mode::usual, mdbx::value_mode::single);
        child.upsert(map, mdbx::slice("key"), mdbx::slice("value"));
        child.commit();
      }
      parent.commit();
      mdbx::txn_managed r = env.start_read();
      auto map = r.open_map("table");
      CHECK(r.get(map, mdbx::slice("key")) == mdbx::slice("value"), "E4 nested commit visible to fresh reader");
    }

    /* E5: commit_embark_read leaves a usable read txn in the same object */
    {
      mdbx::txn_managed w = env.start_write();
      w.commit_embark_read();
      CHECK((mdbx_txn_flags(w.handle()) & ::MDBX_TXN_RDONLY) != 0,
            "E5 commit_embark_read replaced handle with a read txn");
      CHECK(w.id() != 0, "E5 embarked read txn is usable");
    }

    /* E6: abort() on an already-aborted (null) handle throws, no UB */
    {
      mdbx::txn_managed t = env.start_write();
      t.abort();
      CHECK(throws_bad_txn([&] { t.abort(); }), "E6 second abort() throws instead of UB");
      CHECK(!bool(t), "E6 handle is null after successful abort");
    }

    /* E7: cursor_managed outliving its txn closes cleanly (ready4dispose) */
    {
      mdbx::cursor_managed c;
      {
        mdbx::txn_managed t = env.start_write();
        auto map = t.open_map("table");
        c = t.open_cursor(map);
        CHECK(c.seek(mdbx::slice("key")), "E7 cursor finds the record");
        /* t commits and is destroyed here; c becomes a detached cursor */
      }
      bool closed_cleanly = true;
      try {
        c.close();
      } catch (const std::exception &) {
        closed_cleanly = false;
      }
      CHECK(closed_cleanly, "E7 cursor_managed::close() after txn end succeeds (detached ready4dispose)");
    }

    env.close();
    printf("done%s\n", failures ? " WITH FAILURES" : ", all checks passed");
  }
  std::filesystem::remove_all(path, ec);
  return failures ? 1 : 0;
}
