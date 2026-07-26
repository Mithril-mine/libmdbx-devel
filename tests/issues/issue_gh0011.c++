/* transaction-lifecycle stress: multi-level nested write txns with
 * mixed commit/abort at each level, plus RO reset/renew/park/unpark cycles.
 * Non-writemap so nested txns are allowed. */
#include "mdbx.h++"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

static MDBX_dbi dbi;
static unsigned r;
#define NEXT() (r = r * 1103515245u + 12345u)

static void wr_ops(mdbx::txn t, int n, char *vbuf) {
  char kbuf[32];
  for (int i = 0; i < n; ++i) {
    unsigned k = NEXT() % 4000;
    int klen = snprintf(kbuf, sizeof kbuf, "k%08u", k);
    mdbx::slice key(kbuf, klen);
    if ((NEXT() % 4) == 0) {
      t.erase(dbi, key);
    } else {
      size_t vlen = 8 + (NEXT() % 3000);
      mdbx::slice val(vbuf, vlen);
      t.upsert(dbi, key, val);
    }
  }
}

/* recursive nested writer: depth levels, random commit/abort at each */
static void nest(mdbx::txn parent, int depth, char *vbuf) {
  auto child = parent.start_nested();
  wr_ops(child, 40, vbuf);
  if (depth > 0 && (NEXT() & 1))
    nest(child, depth - 1, vbuf);
  if ((NEXT() % 3) == 0)
    child.abort();
  else
    child.commit();
}

int main(int argc, char **argv) {
  const mdbx::path testdb = (argc > 1) ? argv[1] : "issue11";
  mdbx::env::remove(testdb);
  unsigned long seed = (argc > 2) ? strtoul(argv[2], 0, 0) : 1;
  r = (unsigned)seed * 2654435761u + 1;

  bool ok = true;
  try {
    mdbx::env_managed::create_parameters cp;
    cp.geometry = mdbx::env::geometry(mdbx::env::geometry::minimal_value, mdbx::env::geometry::default_value,
                                      mdbx::env::geometry::default_value, mdbx::env::geometry::default_value,
                                      mdbx::env::geometry::default_value, mdbx::env::geometry::minimal_value);
    mdbx::env_managed::operate_parameters op;
    op.mode = mdbx::env_managed::nested_transactions;
    op.options.no_sticky_threads = true;
    op.reclaiming.lifo = true;
    op.max_maps = 42;
    mdbx::env_managed env(testdb, cp, op);
    env.set_extra_option(mdbx::env::extra_runtime_option::dp_limit, 128 * 3) /* must be >= CURSOR_STACK_SIZE*4 */;

    auto t = env.start_write();
    dbi = t.create_map("d");
    t.commit();

    char vbuf[4096];
    memset(vbuf, 0x3C, sizeof(vbuf));

    /* one long-lived reader we keep reset/renew/park/unpark-ing */
    auto ro = env.start_read();

    const int ROUNDS = 500;
    for (int round = 0; round < ROUNDS; ++round) {
      /* top-level writer, sometimes nested, sometimes aborted */
      t = env.start_write();
      wr_ops(t, 60, vbuf);
      if ((NEXT() & 1))
        nest(t, 2, vbuf); /* up to 3 levels deep */
      if ((NEXT() % 8) == 0)
        t.abort();
      else
        t.commit();

      /* exercise RO lifecycle */
      unsigned a = NEXT() % 5;
      if (a == 0) {
        ro.reset_reading();
        ro.renew_reading();
      } else if (a == 1) {
        ro.park_reading(false);
        ro.unpark_reading(false);
      } else if (a == 2) {
        ro.park_reading(true);
        mdbx::slice k("k00000001");
        [[maybe_unused]] auto v = ro.get(dbi, k, mdbx::slice::invalid());
      } else if (a == 3) {
        ro.renew_reading();
      }
      /* else leave reader as-is to hold an old snapshot (creates GC lag) */
    }

    ro.abort();
    env.close();
  } catch (const std::exception &e) {
    fprintf(stderr, "Exception %s\n", e.what());
    ok = false;
  }

  fprintf(stderr, "%s seed=%lu\n", ok ? "Succeded" : "Failed", seed);

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
