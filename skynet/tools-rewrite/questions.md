# Questions for the owner (TASK-44 / B46 escalation)

Decisions needed before/while Phase B. Each item lists options + recommendation.

## Q1. Output compatibility: strict 1:1 or allow improvements?
- **Context**: mdbx_dump/mdbx_load formats must stay byte-compatible for
  BDB/LMDB interop (`spec-dump.md`, `spec-load.md`). But human-readable outputs
  (stat blocks, chk verdict text, error messages) and exit codes could be
  improved while rewriting.
- **Options**:
  a) Strict 1:1 for everything (safest; tests/dump-load.sh + scripts keep working unchanged).
  b) 1:1 only for machine-consumed artifacts (dump/load stream, exit codes);
     allow cosmetic text improvements elsewhere.
  c) Free rein.
- **Recommendation**: (b) — keep dump/load bytes and exit codes frozen;
  allow stderr/stdout prose cleanup. Scripts and CI depend on (a) for formats.

## Q2. Fate of wingetopt
- **Options**: (a) drop entirely (new parser is cross-platform); (b) keep for
  C API consumers outside the rewrite.
- **Recommendation**: (a) — remove from src/tools and GNUmakefile amalgamation
  (`mdbx-wingetopt.h`, `dist-tool-rule` rewrite). It exists only for these tools.

## Q3. API layer for the rewritten tools
- **Options**: (a) `mdbx.h++` required; (b) bare `extern "C" mdbx.h` allowed;
  (c) either, per tool.
- **Recommendation**: (a) `mdbx.h++` — RAII reduces boilerplate massively; the
  test framework already proves it in-repo. If the owner wants a pure-C
  migration, pick (b).

## Q4. Internal headers (`essentials.h`) vs public API only
- **Current**: tools include `src/essentials.h` → compile against libmdbx
  internals (not just public `mdbx.h`).
- **Options**: (a) move to public API only (recommended — cleaner ABI story,
  enables building tools against an installed libmdbx); (b) keep internals.
- **Recommendation**: (a). Must verify no tool uses an internal symbol that has
  no public equivalent (grep found none essential — all calls are public mdbx_*).

## Q5. Missing man page for mdbx_defrag
- defrag has NO man page (`MANPAGES` = 6, no mdbx_defrag.1).
- **Options**: (a) add `man1/mdbx_defrag.1` in Phase B (recommended — the tool is
  documented in --help only); (b) leave as-is.

## Q6. mdbx_copy: unify the argument parser
- copy.c uses a bespoke short-only parser (no getopt, no long options, no
  bundling) while the other six use getopt. The rewrite unifies parsing.
- **Options**: (a) unified LLVM-style parser everywhere, preserving every
  current flag (recommended); (b) add long options for copy as a bonus.
- **Recommendation**: (a), no new long options unless requested.

## Q7. T3/nightly coverage after TASK-42 (flagging, not tools-specific)
- With `MDBX_ENABLE_LONG_TESTS` now gating the 5-minute T3 families and no
  GitHub CI workflow enabling it, T3 becomes local-only. Confirm the owner
  accepts this or wants a nightly/dispatch job (related: REV:cmake N3 note on
  TASK-42).

## Q8. Scope of Phase B (proposal for coordinator/owner)
- Suggested order: `common.hpp` + parser first; then `drop`/`stat` (small) →
  `copy` → `chk` → `dump`/`load` (formats) → `defrag` (callbacks). Each tool in
  its own commit, validated by the existing smoke/dump-load/ctest paths.