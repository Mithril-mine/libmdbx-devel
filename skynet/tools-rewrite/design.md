# Design: src/tools → C++ (Phase A component selection)

Epic B46, Phase A. This document proposes the C++ component choices for
rewriting the 7 CLI tools (`chk copy defrag drop dump load stat`). Docs-only —
nothing here is binding until the owner answers `questions.md`.

## 1. Argument parser

Options considered:

1. **Custom LLVM-style parser** (like `cl::opt`, simplified): full control,
   uniform `-x`, `-xyz`, `--long=val`, `--` handling, no new dependency.
   Cost: ~200-300 lines of tested code; risk of subtle parsing differences vs
   getopt (positional interleaving, `--` termination).
2. **getopt_long (POSIX)** — current behavior source; but on Windows requires
   wingetopt, and getopt's global state / permute behavior is awkward in C++.
3. **Vendored third-party** (e.g. a header-only arg parser) — extra dependency
   and license review; not justified for 7 small tools.

**Recommendation: custom LLVM-style parser in `common.hpp`**, with a strict
compatibility shim so that every current invocation keeps working:
- All existing short flags (`-V -v -q -c -d -f -p -n -u -U -a -l -s -t -T -w -i
  -r -e -0/-1/-2 -b -L -G -N`) and their arg-taking forms.
- `-s table`, `-f file`, `-b n`, `-L mb`, `-d %`, `-G geom`, `-t sec`, `-f %`,
  `-r %`, `-s mb` all take values (value may be glued or spaced, as today).
- copy.c's bespoke loop collapses into the same parser (its flags map 1:1).
- Exit/usage behavior identical (usage → stderr → exit code 1 / EXIT_INTERRUPTED
  for chk).

## 2. Replacement for wingetopt

- Do NOT port wingetopt; the C++ tools use the new parser on all platforms
  (no POSIX getopt dependency at all).
- Remove wingetopt.{c,h} from `src/tools/` and the GNUmakefile amalgamation
  rules (`mdbx-wingetopt.h`, `dist-tool-rule` include rewrite). Keep `TOOLS`
  names unchanged.

## 3. API layer

Options:

1. **Bare C API (`extern "C" mdbx.h`)** — minimal churn, matches current code,
  but loses RAII (env/txn/cursor guards) and the C++ conveniences (error
  handling, `keyval`).
2. **`mdbx.h++` (C++ wrapper)** — RAII environments/transactions/cursors,
  exceptions or error objects, cleaner callbacks. Already used by the test
  framework (`tests/framework/*.c++`), so it is battle-tested in-repo.

**Recommendation: `mdbx.h++`**, with an error strategy matching each tool's
exit-code contract (see below). Rationale: the tools are thin CLI layers over
the API; the wrapper eliminates the biggest share of current boilerplate
(open→txn→cursor→teardown + errno handling), and the test framework proves the
pattern works on all supported platforms.

Exit-code mapping must be explicit: a small `common.hpp` helper maps
`mdbx::error`/return codes to the tool-specific exit codes (chk has its own
6-code scheme; the rest are 0/1).

## 4. Common module structure (`common.hpp`)

Proposed shared pieces (all in one header, ~200-300 lines):

- `struct tool_options` + the LLVM-style parser.
- `class tool { virtual int run() = 0; }` — each `main()` is a thin shell:
  parse → instantiate tool → `return tool.run();`.
- `logger` abstraction (level-filtered to stderr, same prefixes as today).
- `err2exit()` helpers per tool.
- Buffered output writer for dump/load byte-exactness.
- `user_break` (SIGINT/SIGPIPE handler) shared.
- Version banner printer (`-V` block is identical across tools).

## 5. Per-tool design notes

- **chk**: wrap `mdbx_env_chk()` context callbacks in lambdas; keep the
  sync-to-disk auto-repair and turn-to-meta logic; keep exit-code enum.
- **copy**: replace bespoke arg loop; keep binary-to-stdout path exact
  (`mdbx_env_copy2fd`).
- **defrag**: wrap `MDBX_defrag_func` + progress callback; add missing man page
  (document CLI in `man1/mdbx_defrag.1` — currently absent).
- **drop**: trivial.
- **dump**: state machine already table-like; factor header writing into a
  struct; preserve byte-exact output.
- **load**: table-driven header parser; keep strict validation semantics and
  the e55df29d exit-status behavior.
- **stat**: formatting helpers; preserve column alignment.

## 6. Build integration

- New sources `src/tools/*.c++` + `src/tools/common.hpp`; drop `.c` files.
- GNUmakefile `TOOLS` unchanged; rule becomes C++ (`$(CXX)`) and links
  `mdbx.c++` (or libmdbx++) instead of `mdbx-static.o`; remove
  `essentials.h`/`wingetopt` includes.
- CMake: build tools from C++ sources; keep tool target names
  (`mdbx_chk` etc.) for CI/scripts.
- dist/amalgamation: re-verify the `dist-tool-rule` sed rewrites after the
  rewrite (includes change from `essentials.h`/`wingetopt.h` to public API).

## Open items (moved to questions.md)

- 1:1 output compatibility vs. allowed improvements (exit codes, stderr text).
- Fate of wingetopt (recommend removal — owner decision).
- Mandatory `mdbx.h++` vs C-API allowed.
- Whether `mdbx_defrag.1` man page should be added.
- Whether the tools should keep compiling against internals
  (`essentials.h`) or move strictly to the public API.