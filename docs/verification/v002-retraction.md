# V-002 RETRACTION — exhaustive proof that `scan4seq_neon` is CORRECT

**Status: the wave-1 finding V-002 («NEON clz shift → wrong position → heap
underflow») is a FALSE POSITIVE and is retracted in full.** This document
supersedes the V-002 rows in LEDGER.md, issue #485, and the wave-1 report
`G7-gc-adversarial.md`; the harness that produced the finding had the defect.

## 1. The wave-1 harness bug (root cause of the false positive)

The adversarial agent re-implemented the NEON return in its differential
harness as element-pointer arithmetic:

```c
/* harness (WRONG): */ return range - (clz >> 2);        /* pgno_t* minus int = ELEMENTS */
```

while the actual code returns a **byte** displacement through `ptr_disp`
(`src/utils.h:25`: `(intptr_t)ptr + (intptr_t)disp` — raw byte address):

```c
/* gc-get.c:469 (REAL): */ return ptr_disp(range, -(clz_uintptr(mask) >> sizeof(size_t) / 4));
```

The x86 SSE2/AVX2/AVX512 variants return via direct element arithmetic
(`range + 28 - clz32(mask)` etc.), so the wave-1 harness matched them —
that is why the x86 differential "held" while the NEON arm "diverged".
The element/byte confusion was in the harness, not the code.

## 2. Why the code is correct — analytical proof

`diffcmp2mask_neon` (gc-get.c:438-447): `vceqq_u32` yields 0xFFFFFFFF per
matching lane; `vmovn_u32` narrows **by truncation** (ARM ARM), so on
aarch64 each matched lane contributes the field **0xFFFF at bits [16b,
16b+15]** (little-endian `vreinterpret_u64_u16`); on armhf the second
narrow yields **0xFF at bits [8b, 8b+7]**. Lane b corresponds to the
candidate element `range - 3 + b` (window loaded at `range - 3`).

- **aarch64:** highest set bit of a matched lane b = 16b+15 →
  `clz64 = 48 - 16b` → `clz >> (sizeof(size_t)/4) = clz >> 2 = 12 - 4b`
  **bytes** = `4·(3−b)` — exactly the byte distance from `range` to
  `range - 3 + b` ✓, for every b ∈ {0,1,2,3} (b=3 → 0 bytes → `range`).
- **armhf:** highest set bit = 8b+7 → `clz32 = 24 - 8b` →
  `clz >> (sizeof(size_t)/4) = clz >> 1 = 12 - 4b` **bytes** ✓ likewise.

`ptr_disp` adds bytes; the returned pointer is therefore always
element-aligned (`(12-4b) % 4 == 0`) and equals the fallback's answer.
The in-code comment («the width and format of mask … depend on the platform
bitness») is accurate: the field width (16 vs 8 bits) and the shift
(sizeof/4 = 2 vs 1) co-vary so that `clz>>shift` is the byte offset in
both cases. The masked-tail keep-mask `~(size_t)0 << (extra·sizeof·2)`
keeps all lanes ≥ extra (all their bits), so the same clz math applies on
the masked path too. The `found:` return is shared by the main loop and the
tail — one formula, correct in both.

The x86 counterparts, for contrast, need element math because their masks
are one-bit-per-lane (movemask): biases 28/24/16 = 31−3, 31−7, 31−15
encode «minus the window start». Different mask formats, different
formulas — both correct.

## 3. Exhaustive machine proof (real compiled NEON, not emulation)

Harness `docs/verification/repro/v002-prove.c`: the four functions are
**verbatim-extracted** from `src/gc-get.c` @ `bd4516ce` (extraction ranges
recorded in the file header); compiled with the actual cross toolchains and
executed under qemu user-mode emulation of the real target ISA:

| Run | Target | Path coverage | Cases | Divergences |
|---|---|---|---|---|
| 1 | aarch64 gcc -O2 -static → qemu-aarch64 | main loop + masked tail (ROV=0) | 917,506 | **0** |
| 2 | aarch64 → qemu-aarch64 | scalar tail forced (RUNNING_ON_VALGRIND≡1) | 917,506 | **0** |
| 3 | armhf gcc -O2 -mfpu=neon → qemu-arm | main loop + masked tail | 917,506 | **0** |
| 4 | armhf → qemu-arm | scalar tail | 917,506 | **0** |

Exhaustive space: **all 2¹⁶ = 65,536 strictly-descending lists over
{1..16}** with len ≥ 2 × **all num ∈ [2..len]** (seq ∈ [1..15] — the
complete non-wrap domain for legal data, since legal pgno ≤ 2³¹−1 and
pgno+seq < 2³² can never wrap) × 16 placements covering every
`(range+offset) & 0xfff` class (both tail-path variants). Three
independent references per case: `scan4seq_fallback` (verbatim),
`scan4range_checker` (verbatim), and a written-from-spec brute force —
all three agreed on all 917,506 cases before the NEON comparison.

Lane-field format proven empirically in-run (selftest): single match at
lane 1 → mask `0x0000ffff00000000` (aarch64) / `0x0000000000ff0000`
(armhf) — exactly the analytical field model of §2.

**Long-seq corroboration** (`repro/longseq-a64`): 1,359 checks with seq up
to ~1190 on a 1200-page list (beyond the 16-value universe) — zero
mismatches. The formula is seq-independent by construction (§2), the runs
corroborate.

Consequence classification over the whole space: 0 wrong-in-bounds,
0 OOB-underflow, 0 false-nonzero, 0 false-negative. (The wave-1 report's
counterexample `repnl=[10,6,5,1], num=2`: real code — tail, lane 2 match,
mask `0x0000ffff00000000`, clz=16, `-(clz>>2)` = −4 **bytes** = `pnl[3]`
= the '5' — correct; the wave-1 harness computed `pnl[4] − 4·elements` =
`pnl[0]` — the artifact.)

## 4. Why prior verification missed nothing — and what it means

- **The code has presumably survived aarch64 debug CI**: the
  `ASSERT(target == scan4range_checker(...))` in `gc_repnl_get_sequence`
  would fire on the (nonexistent) divergence — consistent with this
  retraction, inconsistent with the wave-1 claim.
- **Under ASAN/Valgrind** the masked tail is compiled out and the scalar
  tail runs — still correct; the main-loop NEON SIMD also runs under ASAN
  and is correct.
- The wave-1 harness lesson is now a standing protocol rule: **when
  re-implementing a function for differential testing, pointer
  displacements returned by the function must be interpreted in the same
  units as the original code** — the safest method is to call the extracted
  verbatim code (as this harness does) rather than a re-implementation.

## 5. Corrective actions performed

- LEDGER.md: V-002 moved from «REFUTED defects» to «false positives
  (retracted)»; defect counts, priority lists, and L8-family notes updated;
  the top defect is now **V-003**.
- Issue #485: body corrected (V-002 removed from defect list; priorities
  updated), retraction comment posted.
- shared-graph-memory: `bug:gc:neon-clz-shift` purged (was 7350);
  `fact:verification:neon-scan4seq-correct` + `fact:verification:ptr-disp-byte-units`
  recorded (the reusable lesson).
- Harness preserved: `docs/verification/repro/v002-prove.c` (+ this
  document) as the permanent differential test — it is the cheapest guard
  for this family of SIMD scans (run under qemu in CI).
