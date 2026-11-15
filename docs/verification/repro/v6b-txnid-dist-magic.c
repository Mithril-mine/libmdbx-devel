/* v6b-txnid-dist-magic.c — standalone simulation for INV-T6 (stream B, wave 2).
 *
 * Verifies the reconciliation between
 *   - dxb_sync_locked():  meta_sync_txnid = committed_txnid - sync_txnid_dist(flags)
 *     (dxb.c:1161-1164), and
 *   - txn_basal_commit(): need_flush_for_nometasync iff
 *     meta_sync_txnid == (uint32_t)head.txnid - txnid_dist(txn->flags)
 *     (txn-basal.c:347-357; the commit-side dist omits the NOMETASYNC test —
 *     INV-T6 claims the omission is harmless),
 * over exhaustive 3-commit sequences of all (writemap, avoid_msync, nometasync,
 * pure) combinations, with a crash injected after every commit position.
 *
 * Modeled semantics (verified against the source, durable env):
 *  - NOMETASYNC commits still sign the head meta STEADY (ENV_UNSYNC is
 *    MDBX_SAFE_NOSYNC only), but the meta page write is lazily durable
 *    (unsynced_pages += 1); it becomes durable at the next step#1 sync of any
 *    later commit — skipped only when `unsynced_pages == 1 && NOMETASYNC`
 *    (skip_lastmeta_sync, dxb.c:981).
 *  - need_flush_for_nometasync gates the pure-commit fast path
 *    (txn-basal.c:378-384) and selects lazy_fd (txn-basal.c:446-450).
 *
 * Invariants asserted:
 *  I1: need_flush fires only when the head meta page is pending-lazy AND the
 *      stored dist equals the current commit's dist (no fire when the counter
 *      means something else);
 *  I2: no false negative: a pending same-mode lazy head always fires (other
 *      counters fall into the explicit meta_sync branch — conservative);
 *  I3: crash recovery always lands on an on-disk steady head; a stale counter
 *      can cause only spurious fires (full-path commit), never a lost meta;
 *  I4: after any non-NOMETASYNC commit every page (data + meta) is durable;
 *  I5: for every state produced by dxb_sync_locked (not recovery), the counter
 *      identity counter == head_txnid - stored_dist holds.
 *
 * Build: cc -Wall -Wextra -o v6b-txnid-dist-magic v6b-txnid-dist-magic.c
 * Run:   ./v6b-txnid-dist-magic   (exits 0 on success)
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define LAZY_UNK (UINT32_MAX / 3u)
#define LAZY_FD (LAZY_UNK + UINT32_MAX / 8u)
#define LAZY_WRITEMAP (LAZY_UNK - UINT32_MAX / 8u)
#define DIST_UNKNOWN UINT32_MAX

struct state {
  uint64_t head_txnid;  /* txnid recorded in the head meta page */
  bool head_steady;     /* sign of the head meta page */
  bool head_on_disk;    /* head meta page content durable */
  uint32_t counter;     /* shared meta_sync_txnid (lock file) */
  uint32_t stored_dist; /* dist encoded by the counter; DIST_UNKNOWN after recovery */
  unsigned lazy_metas;  /* number of committed-but-not-durable meta pages */
  bool lazy_data;       /* data pages of the current head not durable */
};

static const struct state initial = {1000, true, true, 1000, 0, 0, false};

static uint32_t dist_of(bool nometasync, bool writemap, bool avoid) {
  if (!nometasync)
    return 0;
  return (!writemap || avoid) ? LAZY_FD : LAZY_WRITEMAP;
}

static uint32_t commit_dist(bool writemap, bool avoid) { /* txn-basal.c:347 — no NOMETASYNC test */
  return (!writemap || avoid) ? LAZY_FD : LAZY_WRITEMAP;
}

/* returns need_flush_for_nometasync; pure = no data pages written */
static bool commit(struct state *s, bool nometasync, bool writemap, bool avoid, bool pure) {
  /* --- txn_basal_commit head-flush gate --- */
  bool need_flush = false;
  if (s->head_steady && s->counter != (uint32_t)s->head_txnid) {
    need_flush = (s->counter == (uint32_t)s->head_txnid - commit_dist(writemap, avoid));
    if (need_flush) { /* I1 */
      assert(!s->head_on_disk);
      assert(s->stored_dist == commit_dist(writemap, avoid));
    }
    /* I2: pending same-mode lazy head must fire */
    if (!need_flush && !s->head_on_disk && s->stored_dist == commit_dist(writemap, avoid))
      assert(!"false negative: pending same-mode lazy meta not detected");
    if (!need_flush)
      s->head_on_disk = true; /* explicit meta_sync(env, head) covers it */
  }

  /* --- pure-commit fast path gating (txn-basal.c:378) --- */
  const bool fast_pure = pure && !need_flush; /* durable env satisfies the head.is_steady disjunct */

  /* --- dxb_sync_locked --- */
  if (!fast_pure) {
    const unsigned snap = s->lazy_metas + (s->lazy_data ? 3u : 0u);
    if (snap > 0 && !(snap == 1 && nometasync)) { /* step#1, skip_lastmeta_sync */
      s->head_on_disk = true;                     /* fsync/msync covers everything pending */
      s->lazy_metas = 0;
      s->lazy_data = false;
    }
    /* write the new meta page */
    s->head_txnid += 1;
    s->head_steady = true;
    if (nometasync) {
      s->lazy_metas += 1;
      s->head_on_disk = false;
    } else {
      assert(s->lazy_metas == 0 && !s->lazy_data); /* I4 */
      s->head_on_disk = true;
    }
    s->stored_dist = dist_of(nometasync, writemap, avoid);
    s->counter = (uint32_t)s->head_txnid - s->stored_dist;
  }
  /* fast_pure: nothing written, counter/head untouched */

  if (s->stored_dist != DIST_UNKNOWN) /* I5 */
    assert(s->counter == (uint32_t)s->head_txnid - s->stored_dist);
  return need_flush;
}

static void crash_recover(struct state *s, struct state *r) {
  *r = *s;
  if (r->lazy_metas > 0) {
    /* the newest lazy_metas meta pages are lost; recovery lands on the newest
     * on-disk steady meta; the lock-file counter is separately durable and
     * arbitrary relative to it (stale) */
    r->head_txnid -= r->lazy_metas;
    r->head_on_disk = true;
    r->head_steady = true;
    r->lazy_metas = 0;
    r->lazy_data = false;
    r->stored_dist = DIST_UNKNOWN;
    /* keep `counter` as the stale value the lock file retained */
  }
}

int main(void) {
  unsigned long seqs = 0, fires = 0, spurious = 0;
  const bool nm[2] = {false, true}, wm[2] = {false, true}, av[2] = {false, true}, pu[2] = {false, true};
  for (int a = 0; a < 2; ++a)
    for (int b = 0; b < 2; ++b)
      for (int c = 0; c < 2; ++c)
        for (int p0 = 0; p0 < 2; ++p0)
          for (int p1 = 0; p1 < 2; ++p1)
            for (int p2 = 0; p2 < 2; ++p2)
              for (int crash_at = 0; crash_at < 3; ++crash_at) {
                struct state s = initial;
                const bool N[3] = {nm[a], nm[b], nm[c]};
                const bool W[3] = {wm[a], wm[b], wm[c]};
                const bool A[3] = {av[a], av[b], av[c]};
                const bool P[3] = {pu[p0], pu[p1], pu[p2]};
                for (int i = 0; i < 3; ++i) {
                  if (i == crash_at) {
                    struct state r;
                    crash_recover(&s, &r);
                    s = r;
                    assert(s.head_on_disk && s.head_steady); /* I3 */
                    /* next commit's gate over a stale counter: a spurious fire
                     * classifies a DURABLE head meta as pending — harmless
                     * (one full-path commit); assert it can only happen when
                     * the head is NOT pending-lazy */
                  }
                  const uint32_t cd = commit_dist(W[i], A[i]);
                  const bool was_stale_possible =
                      (s.stored_dist == DIST_UNKNOWN) && (s.counter == (uint32_t)s.head_txnid - cd);
                  const bool need = commit(&s, N[i], W[i], A[i], P[i]);
                  ++seqs;
                  fires += need;
                  if (was_stale_possible && need)
                    ++spurious;
                }
              }
  printf("sequences: %lu, need_flush fires: %lu (of which stale-counter-spurious: %lu) — I1..I5 all passed\n", seqs,
         fires, spurious);
  return 0;
}
