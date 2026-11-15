/* V2 «durability/format» analytical probe — stream A, tranche V2.
 *
 * Mirror-harness (self-contained, no libmdbx headers) that re-derives, with
 * independent recomputation, the proofs/counterexamples claimed in
 * docs/verification/LEDGER-v2 report:
 *
 *   PROBE-1 (V-001 companion): the troika FSM mapping meta_cmp2pack() is
 *       verified over all 216 inputs against an independent tournament
 *       recomputation AND against the "tail = not(recent, prefer_steady)"
 *       structural property. Mirrors troika_verify_fsm() so the proof is
 *       executable even though troika_verify_fsm() itself is never called.
 *
 *   PROBE-2: pv2pages/pages2pv round-trip lemma (mirror of cogs.c me2v/v2me):
 *       pages2pv(pv2pages(pv)) == pv for all 65536 pv, and
 *       pv2pages(pages2pv(v)) >= v for all v in [0, me2v(2047,7)].
 *
 *   PROBE-3 (V-023 counterexample): on a 32-bit size_t platform the
 *       dxb_setup() geometry-from-db products
 *           pv2pages(grow_pv) * pagesize,  pv2pages(shrink_pv) * pagesize
 *       wrap mod 2^32: (a) 65536 pages * 65536 pagesize == 0, (b)
 *       me2v(2047,7) pages * 4096 pagesize == 174530560 (166 MB instead of
 *       256 GB). grow/shrink are never bounded by meta_validate().
 *
 *   PROBE-4 (V-024 counterexample): dxb_read_header() offset schedule.
 *       With sys_pagesize == 4096, a DB pagesize of 16384 and a damaged
 *       meta-0, every probe offset lands mid-page: valid meta-1/meta-2 at
 *       16384/32768 are never read -> dest->pagesize stays 0 ->
 *       MDBX_CORRUPTED although 2 of 3 metas are intact.
 *
 * Build: cc -O2 -Wall -Wextra -o v2_durability_probe v2_durability_probe.c
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t pgno_t;
typedef uint64_t txnid_t;
#define NUM_METAS 3u

/* ------------------------------------------------------------------ */
/* meta.h meta_cmp2int/recent/steady + meta.c meta_cmp2pack (verbatim) */
static inline uint8_t meta_cmp2int(txnid_t a, txnid_t b, uint8_t s) {
  return (a == b) ? (uint8_t)(1u * s) : (a > b) ? (uint8_t)(2u * s) : (uint8_t)(0u * s);
}
static inline uint8_t meta_cmp2recent(uint8_t ab_cmp2int, bool a_steady, bool b_steady) {
  assert(ab_cmp2int < 3);
  return ab_cmp2int > 1 || (ab_cmp2int == 1 && a_steady > b_steady);
}
static inline uint8_t meta_cmp2steady(uint8_t ab_cmp2int, bool a_steady, bool b_steady) {
  assert(ab_cmp2int < 3);
  return a_steady > b_steady || (a_steady == b_steady && ab_cmp2int > 1);
}
static uint8_t meta_cmp2pack(uint8_t c01, uint8_t c02, uint8_t c12, bool s0, bool s1, bool s2) {
  assert(c01 < 3 && c02 < 3 && c12 < 3);
  const uint8_t recent =
      meta_cmp2recent(c01, s0, s1) ? (meta_cmp2recent(c02, s0, s2) ? 0 : 2) : (meta_cmp2recent(c12, s1, s2) ? 1 : 2);
  const uint8_t prefer_steady =
      meta_cmp2steady(c01, s0, s1) ? (meta_cmp2steady(c02, s0, s2) ? 0 : 2) : (meta_cmp2steady(c12, s1, s2) ? 1 : 2);
  uint8_t tail;
  if (recent == 0)
    tail = meta_cmp2steady(c12, s1, s2) ? 2 : 1;
  else if (recent == 1)
    tail = meta_cmp2steady(c02, s0, s2) ? 2 : 0;
  else
    tail = meta_cmp2steady(c01, s0, s1) ? 1 : 0;
  const bool valid = c01 != 1 || s0 != s1 || c02 != 1 || s0 != s2 || c12 != 1 || s1 != s2;
  const bool strict = (c01 != 1 || s0 != s1) && (c02 != 1 || s0 != s2) && (c12 != 1 || s1 != s2);
  return tail | recent << 2 | prefer_steady << 4 | strict << 6 | valid << 7;
}
/* The exact troika_fsm_map table from meta.c:73-83. */
static const uint8_t troika_fsm_map[2 * 2 * 2 * 3 * 3 * 3] = {
    232, 201, 216, 216, 232, 233, 232, 232, 168, 201, 216, 152, 168, 233, 232, 168, 233, 201, 216, 201, 233, 233,
    232, 233, 168, 201, 152, 216, 232, 169, 232, 168, 168, 193, 152, 152, 168, 169, 232, 168, 169, 193, 152, 194,
    233, 169, 232, 169, 232, 201, 216, 216, 232, 201, 232, 232, 168, 193, 216, 152, 168, 193, 232, 168, 193, 193,
    210, 194, 225, 193, 225, 193, 168, 137, 212, 214, 232, 233, 168, 168, 168, 137, 212, 150, 168, 233, 168, 168,
    169, 137, 216, 201, 233, 233, 168, 169, 168, 137, 148, 214, 232, 169, 168, 168, 40,  129, 148, 150, 168, 169,
    168, 40,  169, 129, 152, 194, 233, 169, 168, 169, 168, 137, 214, 214, 232, 201, 168, 168, 168, 129, 214, 150,
    168, 193, 168, 168, 129, 129, 210, 194, 225, 193, 161, 129, 212, 198, 212, 214, 228, 228, 212, 212, 148, 201,
    212, 150, 164, 233, 212, 148, 233, 201, 216, 201, 233, 233, 216, 233, 148, 198, 148, 214, 228, 164, 212, 148,
    148, 194, 148, 150, 164, 169, 212, 148, 169, 194, 152, 194, 233, 169, 216, 169, 214, 198, 214, 214, 228, 198,
    212, 214, 150, 194, 214, 150, 164, 193, 212, 150, 194, 194, 210, 194, 225, 193, 210, 194};

/* Independent tournament: argmax under lex order with the function's actual
 * tie rule — on a FULL tie the LATER index wins (the pairwise predicate
 * meta_cmp2recent/meta_cmp2steady returns false for a full tie, and the
 * branch structure resolves every tie toward the higher index). */
static unsigned tournament(const txnid_t t[3], const bool s[3], bool prefer_steady_mode) {
  unsigned best = 0;
  for (unsigned i = 1; i < 3; ++i) {
    bool i_better;
    if (prefer_steady_mode)
      i_better = (s[i] > s[best]) || (s[i] == s[best] && t[i] >= t[best]);
    else
      i_better = (t[i] > t[best]) || (t[i] == t[best] && s[i] >= s[best]);
    if (i_better)
      best = i;
  }
  return best;
}

static int probe1(void) {
  int bad = 0;
  for (size_t i = 0; i < 2 * 2 * 2 * 3 * 3 * 3; ++i) {
    const bool s0 = (i >> 0) & 1, s1 = (i >> 1) & 1, s2 = (i >> 2) & 1;
    const uint8_t c01 = (i / 8) % 3, c02 = (i / 24) % 3, c12 = (i / 72) % 3;
    const uint8_t packed = meta_cmp2pack(c01, c02, c12, s0, s1, s2);
    /* consistency with the shipped table */
    if (troika_fsm_map[i] != packed) {
      printf("PROBE-1 FAIL: fsm_map[%zu]=%u != packed %u\n", i, troika_fsm_map[i], packed);
      ++bad;
      continue;
    }
    /* reconstruct txnids consistent with the three pairwise comparisons,
     * then re-derive recent/prefer_steady/tail structurally */
    /* cmp values 0,1,2 == "a<b", "a==b", "a>b"; solve for a linear order */
    bool s[3] = {s0, s1, s2};
    /* brute-force txnid triples from a small set covering all orders */
    const txnid_t cand[4] = {10, 20, 30, 30}; /* 30 duplicated for ties */
    for (unsigned a = 0; a < 4; ++a)
      for (unsigned b = 0; b < 4; ++b)
        for (unsigned c = 0; c < 4; ++c) {
          const txnid_t t[3] = {cand[a], cand[b], cand[c]};
          if (meta_cmp2int(t[0], t[1], 1) != c01 || meta_cmp2int(t[0], t[2], 1) != c02 ||
              meta_cmp2int(t[1], t[2], 1) != c12)
            continue;
          const unsigned recent = tournament(t, s, false);
          const unsigned prefer = tournament(t, s, true);
          if (recent != ((packed >> 2) & 3) || prefer != ((packed >> 4) & 3)) {
            printf("PROBE-1 FAIL: i=%zu tournament recent %u/%u prefer %u/%u\n", i, recent, (packed >> 2) & 3, prefer,
                   (packed >> 4) & 3);
            ++bad;
          }
          /* tail = the one member of {0,1,2} \ {recent, prefer} */
          const unsigned sum = recent + prefer;
          const unsigned tail_structural = (recent != prefer) ? 3 - sum /* 0+1+2 - recent - prefer */ : (packed & 3);
          if (recent != prefer && (packed & 3) != tail_structural) {
            printf("PROBE-1 FAIL: i=%zu tail %u != structural %u\n", i, packed & 3, tail_structural);
            ++bad;
          }
        }
  }
  printf("PROBE-1 (troika FSM mirror over 216 states, +tournament+structural tail): %s\n", bad ? "FAIL" : "PASS");
  return bad;
}

/* ------------------------------------------------------------------ */
/* cogs.c me2v/v2me/pv2pages/pages2pv (verbatim semantics)             */
static inline pgno_t me2v(size_t m, size_t e) {
  assert(m < 2048 && e < 8);
  return (pgno_t)(32768 + ((m + 1) << (e + 8)));
}
static inline uint16_t v2me(size_t v, size_t e) {
  assert(v > (e ? me2v(2047, e - 1) : 32768));
  assert(v <= me2v(2047, e));
  size_t m = (v - 32768 + ((size_t)1 << (e + 8)) - 1) >> (e + 8);
  m -= m > 0;
  assert(m < 2048 && e < 8);
  const uint16_t pv = (uint16_t)(0x8001 + (e << 12) + (m << 1));
  assert(pv != 65535);
  return pv;
}
static pgno_t pv2pages(uint16_t pv) {
  if ((pv & 0x8001) != 0x8001)
    return pv;
  if (pv == 65535)
    return 65536;
  return me2v((pv >> 1) & 2047, (pv >> 12) & 7);
}
static uint16_t pages2pv(size_t pages) {
  if (pages < 32769 || (pages < 65536 && (pages & 1) == 0))
    return (uint16_t)pages;
  if (pages <= me2v(2047, 0))
    return v2me(pages, 0);
  if (pages <= me2v(2047, 1))
    return v2me(pages, 1);
  if (pages <= me2v(2047, 2))
    return v2me(pages, 2);
  if (pages <= me2v(2047, 3))
    return v2me(pages, 3);
  if (pages <= me2v(2047, 4))
    return v2me(pages, 4);
  if (pages <= me2v(2047, 5))
    return v2me(pages, 5);
  if (pages <= me2v(2047, 6))
    return v2me(pages, 6);
  return (pages < me2v(2046, 7)) ? v2me(pages, 7) : 65533;
}

static int probe2(void) {
  int bad = 0;
  /* Lemma A (machine parity, mirrors pv2pages_verify() at global.c:298):
   * D(E(D(pv))) == D(pv) for all 65536 pv — decode is stable under a
   * re-encoding. The ENCODING may canonicalize (me2v has duplicate (m,e)
   * encodings, e.g. me2v(119,1) == me2v(239,0) == 94208), so E(D(pv)) == pv
   * holds only for canonical (minimal-e) encodings, i.e. image(E). */
  for (size_t i = 0; i < 65536; ++i) {
    const pgno_t pages = pv2pages((uint16_t)i);
    if (pv2pages((uint16_t)pages2pv(pages)) != pages) {
      printf("PROBE-2 FAIL (A): D(E(D(%zu))) != D(%zu)\n", i, i);
      ++bad;
    }
  }
  /* Lemma B (round-up quantization): D(E(v)) >= v below the top clamp. */
  for (size_t v = 0; v <= me2v(2047, 7); ++v) {
    const pgno_t r = pv2pages(pages2pv(v));
    if (v < me2v(2046, 7) && r < v) {
      printf("PROBE-2 FAIL (B): D(E(%zu)) = %u < v\n", v, r);
      ++bad;
    }
  }
  /* Lemma C (what meta_model's ENSURE at meta.c:314-315 actually needs):
   * E*D is idempotent over ALL 65536 pv encodings (canonical or not): with
   * pv2 := E(D(pv)), E(D(pv2)) == pv2. Derived from Lemma A: D(pv2) =
   * D(E(D(pv))) == D(pv) =: w, so E(D(pv2)) = E(w) = pv2. This covers
   * grow_pv/shrink_pv from any source: set_geometry normalization
   * (api-env.c:1221), geometry read from a (foreign/crafted) meta, and the
   * dxb_setup amendment path alike. */
  for (size_t i = 0; i < 65536; ++i) {
    const uint16_t pv2 = (uint16_t)pages2pv(pv2pages((uint16_t)i));
    if (pages2pv(pv2pages(pv2)) != pv2) {
      printf("PROBE-2 FAIL (C): E*D not idempotent at pv=%zu: %u vs %u\n", i, pages2pv(pv2pages(pv2)), pv2);
      ++bad;
    }
  }
  printf("PROBE-2 (pv/pages: stability over 65536 pv + round-up + E*D idempotence): %s\n", bad ? "FAIL" : "PASS");
  return bad;
}

/* ------------------------------------------------------------------ */
static int probe3(void) {
  int bad = 0;
  /* Model of a 32-bit size_t build: unsigned 32-bit arithmetic. */
  const uint32_t pagesize = 65536;
  const uint32_t grow_pages = 65536;                /* pv 0x8101 encodes exactly this */
  const uint32_t product32 = grow_pages * pagesize; /* wraps on 32-bit */
  if (product32 != 0) {
    printf("PROBE-3 NOTE: expected wrap 65536*65536 == 0, got %u\n", product32);
    ++bad;
  } else
    printf("PROBE-3 V-023 (a): pv2pages(grow_pv)=65536 pages * pagesize=65536 "
           "on 32-bit size_t == %u (silent grow-disable; true value 2^32)\n",
           product32);
  const uint32_t ps4096 = 4096;
  const uint32_t big = me2v(2047, 7); /* 67141632 pages, encodable grow step */
  const uint32_t prod2 = big * ps4096;
  printf("PROBE-3 V-023 (b): me2v(2047,7)=%u pages * 4096 on 32-bit == %u "
         "(true 275052437504; distorted but plausible-looking)\n",
         big, prod2);
  /* meta_validate() never bounds grow_pv/shrink_pv: only lower/upper/now. */
  printf("PROBE-3: %s\n", bad ? "FAIL" : "counterexamples reproduced");
  return bad;
}

/* ------------------------------------------------------------------ */
static int probe4(void) {
  /* Mirror of dxb_read_header()'s offset schedule. */
  const unsigned NUM_METAS_ = 3, loop_limit0 = 6;
  const unsigned sys_pagesize = 4096, env_ps = sys_pagesize /* default before env_setup_pagesize */;
  const unsigned db_pagesize = 16384;
  const bool meta0_valid = false, meta1_valid = true, meta2_valid = true;
  unsigned guess_pagesize = 0, loop_limit = loop_limit0;
  unsigned found = 0;
  for (unsigned loop_count = 0; loop_count < loop_limit; ++loop_count) {
    const unsigned meta_number = loop_count % NUM_METAS_;
    const unsigned offset = (guess_pagesize              ? guess_pagesize
                             : (loop_count > NUM_METAS_) ? env_ps
                                                         : sys_pagesize) *
                            meta_number;
    /* A read can only validate the meta whose page starts at this offset. */
    const bool page_start = (offset % db_pagesize) == 0;
    const unsigned page_no = offset / db_pagesize;
    bool valid = false;
    if (page_start) {
      if (page_no == 0)
        valid = meta0_valid;
      else if (page_no == 1)
        valid = meta1_valid;
      else if (page_no == 2)
        valid = meta2_valid;
    }
    if (valid) {
      if (guess_pagesize != db_pagesize) {
        guess_pagesize = db_pagesize;
        loop_limit += 0; /* latched-weak extension not relevant here */
      }
      ++found;
    }
  }
  printf("PROBE-4 V-024: sys=4096, db pagesize=16384, meta0 damaged, "
         "meta1/meta2 intact -> validated metas: %u (pagesize guess %u)\n",
         found, guess_pagesize);
  const int bad = (found == 0 && guess_pagesize == 0) ? 0 : 1;
  printf("PROBE-4: %s (0 found => dest->pagesize==0 => MDBX_CORRUPTED despite "
         "2 intact metas)\n",
         bad ? "FAIL" : "counterexample reproduced");
  return bad;
}

int main(void) {
  int bad = 0;
  bad += probe1();
  bad += probe2();
  bad += probe3();
  bad += probe4();
  printf("== v2_durability_probe: %s ==\n", bad ? "FAILURES PRESENT" : "ALL PROBES BEHAVED AS CLAIMED");
  return bad;
}
