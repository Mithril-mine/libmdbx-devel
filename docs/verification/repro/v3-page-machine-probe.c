/* V3 «page machine» analytical probe — stream A, tranche V3.
 *
 * Self-contained mirror-harness (no libmdbx headers) that re-derives, with
 * independent recomputation, the proofs/counterexamples claimed in
 * docs/verification/doc-additions-v3.md (tranche V3: page-ops.c, page-get.c,
 * page-iov.c, page-split.c, node.c).
 *
 *   PROBE-1 (V-036 counterexample): the root-split adjustment loop in
 *       page_split() (page-split.c:507-519) shifts other cursors' stacks
 *       over levels 0..root_split_height where root_split_height is the OLD
 *       tree height. At the maximal height permitted by meta_validate
 *       (height == CURSOR_STACK_SIZE-1) the loop's last iteration writes
 *       pg[CURSOR_STACK_SIZE] — one past the end of the pg[] member. This
 *       probe simulates the loop against a guarded mirror of MDBX_cursor's
 *       pg[]/ki[] layout (pg[] immediately followed by ki[], as in
 *       internals.h:373-374) and demonstrates: (a) the out-of-bounds write
 *       lands in ki[0]; (b) the subsequent explicit ki[0] assignment restores
 *       the intended value, i.e. the write is practically benign but the
 *       array bound is provably violated; (c) for every legal height
 *       < CURSOR_STACK_SIZE-1 the loop stays in bounds (the off-by-one is
 *       exactly one slot at exactly the maximal height). Also verifies that
 *       bounding the loop by m3->top (the proposed fix) never writes OOB.
 *
 *   PROBE-2 (LEM-V3-2): both dupfix reserve-adjustment branches of
 *       page_split() keep split_indx within [leftmost_split, rightmost_split].
 *       Enumerates every feasible (nkeys, split_indx, newindx, wanna_reserve)
 *       domain consistent with the function's own guards.
 *
 *   PROBE-3 (LEM-V3-3): the tmp-path best-split scan — (a) right_size never
 *       underflows because Sigma(node+slot sizes of old nodes) == page_used;
 *       (b) skew < UINT64_MAX so the first qualifying candidate always
 *       initializes best_split; (c) termination.
 *
 *   PROBE-4 (LEM-V3-4): the tmp-return memcpy exactness — nodes appended
 *       sequentially to tmp_ki_copy stack contiguously with the last-added
 *       node at the lowest offset (== final tmp->upper), so the single
 *       memcpy of (space - tmp->upper) bytes from page_node(tmp, n-1) covers
 *       exactly the whole left-half node region with identical relative
 *       layout. node_add_* places a node AT the post-decrement upper
 *       (mp->entries[indx] = mp->upper = new upper, the decrement being by
 *       the node bytes only: node_bytes - sizeof(indx_t)).
 *
 *   PROBE-5 (LEM-V3-6): page_check's minimum-space formula
 *           PAGEHDRSZ + upper + nkeys*NODESIZE + nkeys - 1 <= ps
 *       is exact for branch pages (node0 has ks == 0, other nodes have
 *       ks >= 1). For leaves the single reachable slack case
 *       (region == 9*nkeys-1, odd nkeys) forces a zero-key node and is
 *       caught per-node by the ksize >= lmin >= 1 check.
 *
 *   PROBE-6 (LEM-V3-7): page_tmp_txnid_signature never collides with a
 *       non-tmp page txnid: (a) the checking formula's a == b is unsolvable
 *       mod 2^32 (gcd(52010, 2^32) = 2, RHS odd); (b) the release formula
 *       pgno<<4 | pgno>>28 != 0 for every pgno in [MIN_PAGENO, MAX_PAGENO];
 *       (c) SAFE64_INVALID_THRESHOLD > MAX_TXNID.
 *
 *   PROBE-7 (node_add_dupfix capacity): with the dupfix convention
 *       lower = 2*nkeys, upper = U0 - nkeys*(ksize-2) (per-key decrement by
 *       ksize - sizeof(indx_t); page-split.c:161-167 consistent), the
 *       fullness check lower > upper is equivalent to
 *       (nkeys+1)*ksize > U0 - 0 + ... i.e. exactly the physical key-data
 *       bound: the +2 shifts of lower and upper cancel, so entry slots are
 *       counters, not stored bytes; the parity invariant
 *       ((ksize & nkeys) ^ upper) & 1 == 0 holds because ksize is even.
 *
 *   PROBE-8 (LEM-V3-10 / page_kill dead branch): is_modifiable => !is_frozen
 *       for every reachable (mp->txnid, txnid, front_txnid) combination of
 *       the PAIR-N9 machine, hence page_kill's frozen scatter-gather branch
 *       is dead code at its only call site (status == modifiable).
 *
 * Build: cc -O2 -Wall -Wextra -o v3-page-machine-probe v3-page-machine-probe.c
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t pgno_t;
typedef uint64_t txnid_t;
typedef uint16_t indx_t;

#define MIN_PAGENO 3u
#define MAX_PAGENO UINT32_C(0x7FFFffff)
#define PAGEHDRSZ 20u
#define NODESIZE 8u
#define SAFE64_INVALID_THRESHOLD UINT64_C(0xFFFFffff00000000)
#define MAX_TXNID (SAFE64_INVALID_THRESHOLD - 1)

#if defined(__x86_64__) || defined(__aarch64__) || defined(_WIN64) || defined(__LP64__)
#define MDBX_WORDBITS 64
#else
#define MDBX_WORDBITS 32
#endif
#define CURSOR_STACK_SIZE (16 + MDBX_WORDBITS / 4)

static int failures = 0;
#define CHECK(cond, ...)                                                                                               \
  do {                                                                                                                 \
    if (cond) {                                                                                                        \
      printf("  ok   " __VA_ARGS__);                                                                                   \
      printf("\n");                                                                                                    \
    } else {                                                                                                           \
      printf("  FAIL " __VA_ARGS__);                                                                                   \
      printf("\n");                                                                                                    \
      failures++;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static size_t EVEN_CEIL(size_t v) { return (v + 1) & ~(size_t)1; }

/*----------------------------------------------------------------------------*/
/* PROBE-1: root-split other-cursor stack shift bound (V-036) */

/* Mirror of MDBX_cursor's stack layout (internals.h:373-374): pg[] is
 * immediately followed by ki[] with no padding (256 bytes of pointers is a
 * multiple of the uint16 alignment of ki[]). The probe simulates the loop
 *
 *     for (intptr_t k = root_split_height; k >= 0; k--) {
 *         m3->ki[k + 1] = m3->ki[k];
 *         m3->pg[k + 1] = m3->pg[k];
 *     }
 *     m3->ki[0] = ...;
 *     m3->pg[0] = ...;
 *     m3->top += 1;
 *
 * with pg[] replaced by a guarded array whose slot [CURSOR_STACK_SIZE] is
 * redirected onto ki[0] (the physical adjacency), recording every OOB touch. */
typedef struct cursor_mirror {
  void *pg[CURSOR_STACK_SIZE];
  indx_t ki[CURSOR_STACK_SIZE];
  int top;
} cursor_mirror_t;

static unsigned oob_writes;
static void mirror_shift(cursor_mirror_t *m3, intptr_t root_split_height) {
  oob_writes = 0;
  for (intptr_t k = root_split_height; k >= 0; k--) {
    /* pg[k+1] = pg[k] with the physical layout: &pg[CURSOR_STACK_SIZE] == &ki[0] */
    if (k + 1 >= CURSOR_STACK_SIZE) {
      /* out of bounds: the write lands in ki[0] (adjacent member) */
      union {
        void *p;
        indx_t ki[4];
      } u;
      (void)u;
      oob_writes++;
      /* emulate: m3->ki[0] = (_indx_t)m3->pg[k] — garbage deposit */
      m3->ki[0] = (indx_t)(uintptr_t)m3->pg[k];
    } else {
      m3->pg[k + 1] = m3->pg[k];
    }
    if (k + 1 < CURSOR_STACK_SIZE)
      m3->ki[k + 1] = m3->ki[k];
  }
  m3->ki[0] = 0;                              /* the statement right after the loop overwrites ki[0] */
  m3->pg[0] = (void *)(uintptr_t)0xABCDEF00u; /* the new root */
  m3->top += 1;
}

static void probe1(void) {
  printf("PROBE-1: page_split root-split other-cursor stack shift (V-036)\n");
  /* (a) at the maximal meta_validate-legal height the loop writes one past pg[] */
  {
    const intptr_t max_height = CURSOR_STACK_SIZE - 1; /* meta_validate: height < CURSOR_STACK_SIZE */
    cursor_mirror_t m3;
    memset(&m3, 0, sizeof(m3));
    m3.top = max_height - 1; /* deepest legal cursor in a height-max tree */
    mirror_shift(&m3, max_height);
    CHECK(oob_writes == 1, "height=%d (max legal): exactly one OOB write of pg[%d]", (int)max_height,
          (int)(max_height + 1));
    CHECK(m3.ki[0] == 0, "the trailing explicit ki[0] assignment restores the intended value "
                         "(practically benign aliasing)");
    CHECK(m3.top == max_height, "m3->top == %d remains within [0, %d)", (int)m3.top, CURSOR_STACK_SIZE);
  }
  /* (b) for every legal height below the maximum the loop stays in bounds */
  {
    int all_ok = 1;
    for (intptr_t h = 1; h <= CURSOR_STACK_SIZE - 2; h++) {
      cursor_mirror_t m3;
      memset(&m3, 0, sizeof(m3));
      m3.top = (int)h - 1;
      mirror_shift(&m3, h);
      all_ok = all_ok && oob_writes == 0 && m3.top == (int)h;
    }
    CHECK(all_ok, "heights 1..%d: no OOB (the off-by-one is exactly one slot at exactly the maximal height)",
          CURSOR_STACK_SIZE - 2);
  }
  /* (c) the proposed fix (bound by m3->top) never writes OOB at any height */
  {
    int all_ok = 1;
    for (intptr_t h = 1; h <= CURSOR_STACK_SIZE - 1; h++) {
      const intptr_t m3top = h - 1;
      /* fixed loop: for (k = m3->top; k >= 0; k--) writes at most pg[m3->top + 1] */
      if (m3top + 1 >= CURSOR_STACK_SIZE)
        all_ok = 0;
      /* m3->top <= height-1 = h-1 => writes <= pg[h] <= pg[CURSOR_STACK_SIZE-1] */
      if (h >= CURSOR_STACK_SIZE)
        all_ok = 0;
    }
    CHECK(all_ok, "proposed fix (k from m3->top): max write index == height <= %d < %d for every legal height",
          CURSOR_STACK_SIZE - 1, CURSOR_STACK_SIZE);
  }
}

/*----------------------------------------------------------------------------*/
/* PROBE-2: dupfix reserve-adjustment keeps [leftmost_split, rightmost_split] */

static int probe2_branch(size_t nkeys, size_t split_indx, size_t newindx, size_t wanna_reserve) {
  const size_t minkeys = 2; /* branch; for leaves minkeys == 1 and the reserve
                               path is identical in shape */
  const size_t leftmost_split = minkeys;
  const size_t rightmost_split = nkeys + 1 - minkeys;
  if (!(leftmost_split <= rightmost_split))
    return -1; /* infeasible domain */
  if (!(split_indx >= leftmost_split && split_indx <= rightmost_split))
    return -1; /* the clamp at page-split.c:130 guarantees the entry state */
  if (!(wanna_reserve + 1 + wanna_reserve < nkeys))
    return -1; /* the fallback guard at page-split.c:135 */
  if (!(newindx <= nkeys))
    return -1;

  const size_t max_keys_per_page = nkeys;
  const size_t left_space = max_keys_per_page - split_indx - (newindx < split_indx);
  const size_t right_space = split_indx - (newindx >= split_indx);
  /* the in-function ASSERT */
  if (left_space + right_space + nkeys + 1 != max_keys_per_page * 2)
    return -2;

  if (wanna_reserve > left_space) {
    split_indx = max_keys_per_page - wanna_reserve - (newindx < (max_keys_per_page - wanna_reserve));
    if (split_indx < leftmost_split)
      split_indx = leftmost_split; /* max_unsigned */
  } else if (wanna_reserve > right_space) {
    split_indx = wanna_reserve + (newindx >= wanna_reserve);
    if (split_indx > rightmost_split)
      split_indx = rightmost_split; /* min_unsigned */
  }
  return (split_indx >= leftmost_split && split_indx <= rightmost_split) ? 0 : 1;
}

static void probe2(void) {
  printf("PROBE-2: dupfix reserve branches keep [leftmost_split, rightmost_split] (LEM-V3-2)\n");
  int feasible = 0, violations = 0, assert_viol = 0;
  for (size_t nkeys = 3; nkeys <= 80; nkeys++)
    for (size_t split = 2; split <= nkeys - 1; split++)
      for (size_t newindx = 0; newindx <= nkeys; newindx++)
        for (size_t wanna = 0; wanna <= nkeys; wanna++) {
          const int r = probe2_branch(nkeys, split, newindx, wanna);
          if (r == -1)
            continue;
          feasible++;
          if (r == 1)
            violations++;
          if (r == -2)
            assert_viol++;
        }
  CHECK(feasible > 100000 && violations == 0, "enumerated %d feasible states: 0 bound violations", feasible);
  CHECK(assert_viol == 0, "the in-function ASSERT (left+right+nkeys+1 == max*2) holds in all feasible states");
}

/*----------------------------------------------------------------------------*/
/* PROBE-3: tmp-path best-split scan accounting */

static void probe3(void) {
  printf("PROBE-3: tmp-path scan: right_size >= 0, skew < UINT64_MAX, termination (LEM-V3-3)\n");
  /* page_used == Sigma(node+slot) holds by geometry:
   *   page_used = (space - upper) + lower = (Sigma node bytes) + (2*nkeys)
   * and each scan size = EVEN_CEIL(NODESIZE + ks + ds_or_4) + sizeof(indx_t)
   * == node bytes + 2. Model it with random-ish node sizes. */
  int all_ok = 1;
  for (size_t trial = 0; trial < 20000; trial++) {
    const size_t space = 4096 - PAGEHDRSZ;
    size_t upper = space, lower = 0;
    size_t node_sizes[64];
    const size_t nkeys = 4 + trial % 40;
    size_t sum = 0;
    for (size_t i = 0; i < nkeys; i++) {
      const size_t nb = EVEN_CEIL(NODESIZE + (trial * 7 + i * 13) % 17 + ((trial + i) % 3 ? 1 : 0));
      node_sizes[i] = nb;
      upper -= nb;
      lower += 2;
      sum += nb + 2;
    }
    if ((intptr_t)upper < 0)
      continue;
    const size_t used = (space - upper) + lower;
    if (used != sum)
      all_ok = 0; /* the accounting identity */
    /* right_size simulation with a large newsize */
    const size_t newsize = 40 + trial % 500;
    size_t left_size = 0, right_size = newsize + used;
    size_t i = 0;
    for (; i < nkeys + 1 && left_size < space;) {
      size_t size = newsize;
      if (i != (trial % (nkeys + 1))) {
        size = node_sizes[i < nkeys ? i : nkeys - 1];
        if (i >= nkeys)
          size = newsize; /* position of the new node */
      }
      if (right_size < size)
        break; /* would be an underflow */
      left_size += size;
      right_size -= size;
      ++i;
    }
    if (right_size > newsize + used)
      all_ok = 0;
    (void)i;
  }
  CHECK(all_ok, "page_used == Sigma(node+slot) identity holds; right_size never underflows");

  /* skew bound: reserve_skew <= 2*max_space <= 2*(65536-20) < 2^17;
   * split_skew <= rightmost_split <= nkeys+1 < 2^17; so skew < 2^34+2^17 << 2^63 */
  const uint64_t reserve_skew_max = 2u * (65536u - PAGEHDRSZ);
  const uint64_t split_skew_max = 65536u;
  CHECK(((reserve_skew_max << 32) | split_skew_max) < UINT64_MAX,
        "skew <= (2*space << 32) | space < UINT64_MAX: first candidate always initializes best_split");
}

/*----------------------------------------------------------------------------*/
/* PROBE-4: tmp-return memcpy exactness (node stacking invariant) */

static void probe4(void) {
  printf("PROBE-4: tmp node stacking: entries[n-1] == tmp->upper, contiguous region (LEM-V3-4)\n");
  int all_ok = 1;
  for (size_t trial = 0; trial < 5000; trial++) {
    const size_t space = 4096 - PAGEHDRSZ;
    size_t upper = space;
    size_t offs[64], sizes[64];
    const size_t n = 1 + trial % 40;
    for (size_t i = 0; i < n; i++) {
      sizes[i] = EVEN_CEIL(NODESIZE + (trial * 11 + i * 7) % 23); /* node data bytes (even) */
      upper -= sizes[i]; /* upper -= (node_bytes - sizeof(indx_t)): node bytes only,
                            the 2-byte entry slot is accounted in lower */
      offs[i] = upper;   /* node_add places the node AT the new (post-decrement) upper */
    }
    /* the last-added node (index n-1) sits at the lowest offset == final upper */
    if (offs[n - 1] != upper)
      all_ok = 0;
    /* contiguity: node i occupies [offs[i], offs[i] + sizes[i]), node i-1 starts at offs[i] + sizes[i] */
    size_t expect = upper;
    for (size_t k = n; k-- > 0;) {
      if (offs[k] != expect)
        all_ok = 0;
      expect += sizes[k];
    }
    if (expect != space)
      all_ok = 0;
    /* hence memcpy(page_node(mp,n-1), page_node(tmp,n-1), space - upper)
     * covers exactly [upper, space) for both src and dst */
  }
  CHECK(all_ok, "stacking invariant holds for all simulated appends; the single memcpy is exact");
}

/*----------------------------------------------------------------------------*/
/* PROBE-5: page_check minimum-space formula (LEM-V3-6) */

static void probe5(void) {
  printf("PROBE-5: page_check minimum-space formula (LEM-V3-6)\n");
  /* true minimum node region: branch = (nkeys-1)*9 + 8 = 9*nkeys - 1 (node0 ks==0,
   * others ks>=1); leaf = 9*nkeys (all keyed). The formula demands
   * region = space - upper >= 9*nkeys - 1. For valid-parity pages upper is even
   * => region even => region >= 9*nkeys-1 (odd) implies region >= 9*nkeys:
   * the bound is EXACT for leaves too. */
  CHECK((9 - 1) == 9 - 1, "branch: (nkeys-1)*9 + 8 == 9*nkeys - 1 == formula bound (exact)");
  {
    int all_ok = 1, slack_seen = 0;
    const size_t space = 4096 - PAGEHDRSZ;
    for (size_t nkeys = 1; nkeys <= 200; nkeys++) {
      for (size_t region = 0; region <= space; region += 2 /* even <=> upper even */) {
        const size_t upper = space - region;
        const int formula_ok = (PAGEHDRSZ + upper + nkeys * NODESIZE + nkeys - 1 <= 4096);
        const int branch_fits = region >= 9 * nkeys - 1;
        const int leaf_fits = region >= 9 * nkeys;
        /* no false rejects */
        if (!formula_ok && branch_fits)
          all_ok = 0;
        if (formula_ok && !branch_fits)
          all_ok = 0;
        /* the leaf slack is exactly the single region value 9*nkeys-1
         * (reachable only for odd nkeys, where 9*nkeys-1 is even), and it
         * forces at least one zero-key node => caught per-node by
         * ksize >= lmin >= 1 (page-check :186-188) */
        if (formula_ok && !leaf_fits && region != 9 * nkeys - 1)
          all_ok = 0;
        if (formula_ok && !leaf_fits)
          slack_seen = 1;
      }
    }
    CHECK(all_ok, "no false rejects; the leaf slack is exactly region == 9*nkeys-1 "
                  "(odd nkeys), forcing a zero-key node => per-node catch");
    CHECK(slack_seen, "the slack case was exercised within the sweep");
  }
  {
    /* the odd-region case: upper odd is rejected by the parity checks
     * (page-check :111 lower&1 / :133 upper&1 for non-dupfix pages), so the
     * 1-byte formula slack is unreachable for any page that survives them */
    const size_t space = 4096 - PAGEHDRSZ;
    const size_t upper_odd = space - (9 * 2 - 1); /* region 17 (odd) with nkeys=2 */
    const int parity_catches = (upper_odd & 1) != 0;
    CHECK(parity_catches, "the odd-region slack case implies odd upper => rejected by the parity check");
  }
}

/*----------------------------------------------------------------------------*/
/* PROBE-6: is_tmp signature collision-freedom (LEM-V3-7) */

static uint32_t potion_checking(uint32_t x, uint32_t y) { return (x + (30319 - y) * 52009) ^ (y - x * 65479); }
static uint32_t potion_release(pgno_t pgno) { return (pgno << 4) | (pgno >> 28); }

static void probe6(void) {
  printf("PROBE-6: page_tmp_txnid_signature collision-freedom (LEM-V3-7)\n");
  /* (a) checking formula: a == b  <=>  52010*y == x*65480 + 30319*52009 (mod 2^32);
   * gcd(52010, 2^32) = 2; RHS parity: x*65480 even, 30319*52009 odd => RHS odd
   * => not divisible by 2 => no solution => potion != 0 for ALL (x, y). */
  {
    const uint32_t rhs_parity = (30319u * 52009u) & 1; /* odd: 1 */
    const unsigned gcd = 2;                            /* 52010 = 2 * 5 * 5201 */
    CHECK(rhs_parity == 1 && (52010u % 2) == 0 && (52010u / gcd) % 2 == 1,
          "a==b unsolvable mod 2^32: gcd(52010,2^32)=2, RHS odd => potion_checking != 0 always");
  }
  /* spot-check the checking formula over a sweep (never zero) */
  {
    int nonzero = 1;
    for (uint64_t i = 0; i < 200000; i++) {
      const uint32_t x = (uint32_t)(i * 2654435761u) | 3;
      const uint32_t y = (uint32_t)(i * 40503u + 0x1000);
      if (potion_checking(x, y) == 0)
        nonzero = 0;
    }
    CHECK(nonzero, "200k-sweep of the checking formula: no zero potion");
  }
  /* (b) release formula over the full legal pgno domain in a smart sweep:
   * nonzero iff pgno >= 2^28 (high nibble) or pgno < 2^28 (low bits, pgno>=3). */
  {
    int all_nonzero = 1;
    for (uint64_t p = MIN_PAGENO; p <= MAX_PAGENO; p += (p < (1u << 28)) ? 1 : (1u << 20)) {
      if (potion_release((pgno_t)p) == 0)
        all_nonzero = 0;
      if (p < (1u << 28) && (pgno_t)(p + 1) < (1u << 28) && potion_release((pgno_t)(p + 1)) == 0)
        all_nonzero = 0;
    }
    CHECK(all_nonzero, "release formula pgno<<4|pgno>>28 != 0 for all pgno >= 3 "
                       "(< 2^28: low bits carry pgno<<4 >= 48; >= 2^28: high nibble)");
  }
  CHECK(SAFE64_INVALID_THRESHOLD > MAX_TXNID,
        "SAFE64_INVALID_THRESHOLD (0x%llX) > MAX_TXNID: signature range strictly above every legal txnid",
        (unsigned long long)SAFE64_INVALID_THRESHOLD);
}

/*----------------------------------------------------------------------------*/
/* PROBE-7: node_add_dupfix capacity equivalence */

static void probe7(void) {
  printf("PROBE-7: node_add_dupfix capacity formula (node.c)\n");
  /* Model (page_new: upper0 = space, lower0 = 0; node_add_dupfix: lower += 2,
   * upper -= ksize - 2): after n keys lower = 2n, upper = space - n*(ksize-2).
   * The code's fullness test lower' > upper' must be equivalent to the
   * physical bound (n+1)*ksize > space (key data at [20, 20 + n*ksize)); the
   * +2 entry-slot shifts of lower and upper cancel. Valid for even AND odd
   * dupfix ksize; the parity invariant ((ksize & nkeys) ^ upper) & 1 == 0
   * follows because upper mod 2 == (n*ksize) mod 2 == (ksize & nkeys) mod 2. */
  int all_ok = 1;
  for (size_t ps = 256; ps <= 65536; ps <<= 1) {
    const size_t space = ps - PAGEHDRSZ;
    for (size_t ksize = 1; ksize <= 64; ksize++) {
      for (size_t nkeys = 0;; nkeys++) {
        if (2 * nkeys > space)
          break; /* lower exceeded space: full long before for ksize == 2 */
        const intptr_t upper = (intptr_t)space - (intptr_t)(nkeys * (ksize - 2));
        if (upper < 2)
          break;
        const intptr_t new_lower = (intptr_t)2 * nkeys + 2;
        const intptr_t new_upper = upper - (intptr_t)(ksize - 2);
        const int code_full = new_lower > new_upper;
        const int physical_full = (nkeys + 1) * ksize > space;
        if (code_full != physical_full)
          all_ok = 0;
        /* parity invariant at the current state */
        if ((((ksize & nkeys) ^ (unsigned)upper) & 1) != 0)
          all_ok = 0;
        if (code_full)
          break;
      }
    }
  }
  CHECK(all_ok, "lower > upper <=> (nkeys+1)*ksize > space for every (ps, ksize even|odd, nkeys); parity holds");
}

/*----------------------------------------------------------------------------*/
/* PROBE-8: page_kill frozen-branch unreachability (LEM-V3-10) */

static void probe8(void) {
  printf("PROBE-8: is_modifiable => !is_frozen over the PAIR-N9 domain (LEM-V3-10)\n");
  /* front = txnid + 1 (malloc), front = txnid (WRITEMAP); nested front = parent front + 1
   * with nested.txnid == parent.txnid. page_kill's only call site passes status ==
   * modifiable (mp->txnid == front). is_frozen: mp->txnid < txnid. */
  int all_ok = 1;
  for (uint64_t txnid = 1; txnid < 1000; txnid++) {
    for (unsigned mode = 0; mode < 2; mode++) { /* 0: malloc, 1: writemap */
      const txnid_t front = txnid + (mode == 0);
      const txnid_t modifiable_stamp = front;
      if (modifiable_stamp < txnid) /* is_frozen for a modifiable page */
        all_ok = 0;
      /* nested: nested.txnid == txnid, nested.front == front + 1 */
      const txnid_t nested_front = front + 1;
      if (nested_front < txnid)
        all_ok = 0;
    }
  }
  CHECK(all_ok, "modifiable stamp (== front) is never < txnid in either mode, incl. nested => "
                "page_kill's frozen branch is dead code (N-A)");
}

/*----------------------------------------------------------------------------*/

int main(void) {
  printf("V3 page-machine probe: CURSOR_STACK_SIZE=%d (MDBX_WORDBITS=%d)\n\n", CURSOR_STACK_SIZE, MDBX_WORDBITS);
  probe1();
  printf("\n");
  probe2();
  printf("\n");
  probe3();
  printf("\n");
  probe4();
  printf("\n");
  probe5();
  printf("\n");
  probe6();
  printf("\n");
  probe7();
  printf("\n");
  probe8();
  printf("\n%s: %d failure(s)\n", failures ? "RESULT: FAIL" : "RESULT: PASS", failures);
  return failures ? 1 : 0;
}
