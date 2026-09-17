# SourceCraft Platform Specifics

> Part of the [Skynet project index](../README.md).
> Everything specific to the **SourceCraft** hosting/CI platform for this repository
> (`dqdkfa/libmdbx-devel`): CI configuration, MCP tooling, issue/PR/review automation, runs and
> logs. General (platform-agnostic) docs live one level up in `skynet/`.

---

## 1. Repository on SourceCraft

- Org: `dqdkfa`; repo slug: `libmdbx-devel` (development source; amalgamated distribution repo:
  `libmdbx`).
- Canonical origin since 2022 (GitHub is only a mirror and is blacklisted as origin — see
  project history in the root README).
- Web UI: <https://sourcecraft.dev/dqdkfa/libmdbx>; API endpoint used by MCP:
  `https://api.sourcecraft.tech/mcp`.

## 2. SourceCraft CI (`.sourcecraft/ci.yaml`)

The primary CI for this repo (GitHub Actions is a secondary mirror). Config file at repo root:
`.sourcecraft/ci.yaml`.

### Triggers

- push → workflows: `ci-linux-debug-gcc`, `ci-linux-debug-clang`, `ci-linux-release-spilling`
  (pull_request trigger commented out);
- schedule: daily `42 3 * * *` (03:42 UTC) — "Ежедневный запуск CI Linux".

### Global env

```yaml
MDBX_BUILD_OPTIONS: -DMDBX_CHECKING=2 -DMDBX_DEBUG=0 -DMDBX_FORCE_ASSERTIONS=1 -DxMDBX_DEBUG_SPILLING=1 -DMDBX_DEBUG_SPILLING=1
```

### Workflows

| Workflow | Task | Compiler | CMake config | `CI_MAKE_TARGET` |
| --- | --- | --- | --- | --- |
| `ci-linux-debug-gcc` | `ci-linux-debug-gcc-task` | `CC=gcc CXX=g++` | `-DCMAKE_BUILD_TYPE=Devel -DMDBX_BUILD_CXX:BOOL=OFF` | `smoke` |
| `ci-linux-debug-clang` | `ci-linux-debug-clang-task` | `CC=clang CXX=clang++` | `-DCMAKE_BUILD_TYPE=MinSizeRel -DMDBX_BUILD_CXX:BOOL=ON` | `test` |
| `ci-linux-release-spilling` | `ci-linux-release-spilling-task` | `CC=clang CXX=clang++` | `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | `check` |

Cube settings: image `dh-mirror.gitverse.ru/jakoch/cpp-devbox:forky-latest`,
`max_cube_duration: 30m`. Script: `test*/ci/ci.sh "-DCFG1|-DCFG2"` — the `|`-separated configs
are passed to `tests/ci/ci.sh`, which performs several configure+build rounds per workflow
(hence `test*/ci/ci.sh` glob matching the `test/` symlink layout on the runner).

### Monitoring runs & logs via MCP

- `ListRuns` / `GetRun` — list/inspect CI runs in the repo.
- `GetWorkflow` — workflow within a run.
- `GetCubeLogs` (cube = job step; params: `run_slug`, `workflow_slug`, `task_slug`,
  `cube_slug`, `page`) — paged logs of a step.
- `GetCubeArtifacts` — download artifacts.

Use these when a PR's CI fails: pull `GetCubeLogs` pages in order and find the failing target.

## 3. MCP configuration (`.codeassistant/mcp.json`)

The agent toolset is configured in `.codeassistant/mcp.json` → server `sourcecraft`
(streamable HTTP, `https://api.sourcecraft.tech/mcp`). Enabled tools cover:

| Domain | Tools |
| --- | --- |
| Issues | `ListRepositoryIssues`, `ListIssuesAssignedToAuthenticatedUser`, `CreateIssue`, `GetIssue`, `UpdateIssue`, `DeleteIssue`, labels (`CreateLabel`, `DeleteLabel`, `GetLabels`, `RemoveLabels`), linked PRs (`AddLinkedPRs`, `GetLinkedPRs`, `RemoveLinkedPRs`), comments (`Create/Get/Update/DeleteIssueComment`) |
| Pull Requests | `ListRepositoryPullRequests`, `ListMyPullRequests`, `CreatePullRequest`, `GetPullRequest`, `UpdatePullRequest`, `DiscardPullRequest`, `PublishPullRequest`, comments (`List/Create/Get/Update/DeletePullRequestComment`), `ListPullRequestFiles`, `SetDecision`, `GetMergeChecks`, `MergePullRequest` |
| CI | `RunWorkflows`, `ListRuns`, `GetRun`, `GetWorkflow`, `GetCubeLogs`, `GetCubeArtifacts` |

## 4. Issue & PR workflow on SourceCraft

### Issues

- Create with `CreateIssue` (title/description/labels/priority `trivial..blocker`/status
  `open|inProgress|...`/deadline/assignee). Issue slugs are stable identifiers.
- Labels are repo-scoped (`CreateLabel`, slug auto-generated from name unless provided).
- Link PRs: `AddLinkedPRs` (ids or slugs) — the PR then shows on the issue page.

### Pull requests

1. `CreatePullRequest(source_branch, target_branch, title, description, publish=false)` —
   starts as **draft** (publish=false). Drafts do not trigger merge checks/CI completion.
2. Iterate: add commits to the source branch; comments are anchored to file+position within an
   **iteration** (`CreatePullRequestComment` with `anchor.path/position.from/to`, `side`,
   `outdated`; `iteration` selects the diff iteration). Draft comments (publish=false) can be
   reviewed before posting.
3. Ready: `PublishPullRequest` → status `open` → CI runs on the three workflows.
4. Review decision: `SetDecision` (`approve`, `trust`, `block`, `abstain`); `block` requires
   resolution before merge.
5. Merge checks: `GetMergeChecks` → review status, CI status, merge-conflict detection.
6. Merge: `MergePullRequest` with options `squash`, `rebase`, `delete_branch`, `force`.
   (Merge is asynchronous; poll `GetMergeChecks`/`GetPullRequest`.)

Conventions observed in this repo: `publish: false` for drafts, `silent: true` to suppress
notifications for bulk/automated operations.

## 5. Platform-specific gotchas

- CI cube duration is capped at 30 min — keep `smoke`/`test`/`check` within budget; long
  stochastic scenarios must not be wired into push CI (use daily cron or manual `RunWorkflows`).
- The `test*/ci/ci.sh` glob in the workflow script implies the runner has `test/` ↔ `tests/`
  layout; when adding CI steps, keep the same invocation style.
- Daily cron CI is the safety net for stochastic regressions after merges — watch
  `GetCubeLogs` on cron runs after engine changes.
- Only the three Linux workflows are enabled for push; cross-platform coverage lives in the
  GitHub Actions mirror (informational).