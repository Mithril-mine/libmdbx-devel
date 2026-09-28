
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing

## Code style
 - use LLVM codestyle

<!-- dist-cutoff-begin -->
## Git history & CI policy (dev-only, excluded from `make dist`)

Development remotes (`origin`, `github`, `upstream`) are developer-owned and
rewritable — users consume only amalgamated `make dist` artifacts, so history
rewrites and force-pushes here never affect them. Keep every commit in the
published history working **and CI-green** so `git bisect` never walks false
paths:

- when fixing, **rewrite/fold the fix into the commit that introduced the
  defect** instead of stacking fix commits on top;
- rewrites must **not cross git tags**; ask the owner before rewriting more than
  a few commits;
- before publishing, validate each rewritten point locally (build + tests), then
  force-push the branch.

Full policy: `skynet/git-history-policy.md`.
<!-- dist-cutoff-end -->
