#!/usr/bin/env python3
"""watchdog.py — wall-clock control for the CI runs.

The reusable-workflow caller jobs cannot carry `timeout-minutes` (the
GitHub workflow schema rejects it for `uses:` jobs), and a job's own
`timeout-minutes` counts from when the job actually starts on a runner:
queue time and pathological cascades are invisible to it. The historic
5-6-hour CI blockages were exactly this class (the 360-minute default
job timeout of the pre-hardening era). This watchdog closes the gap: on
a schedule it lists the in-progress runs of the CI workflows and
cancels those whose wall-clock age exceeds the budget.

Usage:
    python3 tests/ci/watchdog.py [--budget-hours 6] [--stuck-hours 24]
                                 [--dry-run] [--repo OWNER/REPO]
                                 [--workflows ci-linux.yml,...]

Environment:
    GITHUB_TOKEN, GH_TOKEN   API token (required to actually cancel)
    GITHUB_REPOSITORY        default repo, as OWNER/REPO
"""

import argparse
import datetime as dt
import json
import os
import sys
import urllib.error
import urllib.request

# Every CI workflow whose runs may chain for hours (platform cells,
# dispatch matrices, scans, module tests). Docs/probes are short but
# included for uniformity: a stuck run of any CI workflow is a hang.
DEFAULT_WORKFLOWS = (
    "ci-dispatch.yml",
    "ci-linux.yml",
    "ci-windows.yml",
    "ci-macos.yml",
    "ci-android.yml",
    "ci-codegraph.yml",
    "ci-mcp-shared-graph-memory.yml",
    "ci-special-builds.yml",
    "ci-docs.yml",
    "ci-probes.yml",
)

API = "https://api.github.com"


def api(method: str, path: str, token: str, payload: dict = None):
    req = urllib.request.Request(
        f"{API}{path}",
        data=json.dumps(payload).encode() if payload is not None else None,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
        method=method,
    )
    with urllib.request.urlopen(req) as resp:
        body = resp.read()
    return json.loads(body) if body else {}


def parse_iso(stamp: str) -> dt.datetime:
    return dt.datetime.fromisoformat(stamp.replace("Z", "+00:00"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget-hours", type=float, default=6.0,
                    help="cancel in-progress runs older than this (default 6)")
    ap.add_argument("--stuck-hours", type=float, default=24.0,
                    help="cancel queued-only runs older than this (default 24)")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""),
                    help="OWNER/REPO (default: GITHUB_REPOSITORY)")
    ap.add_argument("--workflows", default=",".join(DEFAULT_WORKFLOWS),
                    help="comma-separated workflow file names to watch")
    ap.add_argument("--dry-run", action="store_true",
                    help="report without cancelling")
    args = ap.parse_args()

    if not args.repo:
        print("watchdog: --repo or GITHUB_REPOSITORY is required", file=sys.stderr)
        return 4
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or ""
    if not token and not args.dry_run:
        print("watchdog: GITHUB_TOKEN/GH_TOKEN is required without --dry-run",
              file=sys.stderr)
        return 4
    headers = ({"Authorization": f"Bearer {token}"}
               if token else {"Accept": "application/vnd.github+json"})

    now = dt.datetime.now(dt.timezone.utc)
    workflows = {w.strip() for w in args.workflows.split(",") if w.strip()}
    over_budget, stuck, cancelled, failed = [], [], 0, 0

    # server-side status filter keeps the sweep two light queries instead of
    # paging through the whole run history
    for status in ("in_progress", "queued"):
        page = 1
        while True:
            req = urllib.request.Request(
                f"{API}/repos/{args.repo}/actions/runs"
                f"?status={status}&per_page=100&page={page}",
                headers=headers)
            try:
                with urllib.request.urlopen(req) as resp:
                    batch = json.loads(resp.read())
            except urllib.error.HTTPError as e:
                print(f"watchdog: list {status} page {page} failed: "
                      f"HTTP {e.code}", file=sys.stderr)
                return 1
            runs = batch.get("workflow_runs", [])
            if not runs:
                break
            for run in runs:
                path = run.get("path", "")
                if path not in workflows:
                    continue
                started = run.get("run_started_at")
                if started:
                    age_h = (now - parse_iso(started)).total_seconds() / 3600.0
                    if age_h > args.budget_hours:
                        over_budget.append((run["id"], path, age_h))
                else:
                    created = run.get("created_at")
                    age_h = (now - parse_iso(created)).total_seconds() / 3600.0
                    if age_h > args.stuck_hours:
                        stuck.append((run["id"], path, age_h))
            if len(runs) < 100:
                break
            page += 1

    for run_id, path, age_h in over_budget + stuck:
        kind = "over budget" if (run_id, path, age_h) in over_budget else "stuck queued"
        print(f"watchdog: {kind}: run {run_id} ({path}) is {age_h:.1f}h old")
        if args.dry_run:
            print(f"watchdog: dry-run, would cancel run {run_id}")
            continue
        try:
            api("POST", f"/repos/{args.repo}/actions/runs/{run_id}/cancel",
                token)
            cancelled += 1
            print(f"watchdog: cancelled run {run_id}")
        except urllib.error.HTTPError as e:
            if e.code == 409:  # already completed between list and cancel
                print(f"watchdog: run {run_id} already completed, skipping")
                continue
            failed += 1
            print(f"watchdog: cancel of run {run_id} failed: HTTP {e.code}",
                  file=sys.stderr)

    print(f"watchdog: {len(over_budget)} over-budget, {len(stuck)} stuck "
          f"queued, {cancelled} cancelled, {failed} failures"
          f"{', dry-run' if args.dry_run else ''}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
