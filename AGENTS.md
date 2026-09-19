
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing

## Code style
 - use LLVM codestyle

## Project index & agent practices
 - Start from the project index: `skynet/README.md` (repo map, build, architecture, workflows,
   testing infrastructure, amalgamation).
 - Platform specifics (SourceCraft CI/MCP/PR flow): `skynet/sourcecraft/README.md`.
 - Follow the integrated agent methodology in `skynet/superpowers/README.md` (adapted from
   obra/superpowers, MIT): brainstorming, writing-plans, executing-plans, TDD,
   systematic-debugging, code-review skills.
 - Verification levels: 1 = specific test, 1-x = same test under ASAN/UBSAN/MEMCHECK,
   2 = full `ctest`, 3 = `make check`, 4 = + test-ubsan/test-asan, 5 = + test-memcheck,
   6 = `stochastic.sh` (bounded), 7 = human-controlled extended (battery-tmux.sh).
   Choose the level by change area and risk; see
   `skynet/superpowers/verification-before-completion/SKILL.md`.
 - Keep build-file changes out of the amalgamated `dist/` (dist-cutoff markers), see
   `skynet/build.md` section on amalgamation.
 - Maintain the codebase knowledge graph (memory-MCP) per `skynet/memory-codex.md`:
   verify observations against code before acting (Gemba), refresh entities after
   significant work, record invariants/naming traps with provenance.
