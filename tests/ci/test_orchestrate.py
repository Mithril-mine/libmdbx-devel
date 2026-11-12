# Unit tests for tests/ci/orchestrate.py (requirements traceability:
# docs/engineering/ci-orchestrator-requirements.md — every test names the
# ORCH-*/INV-* requirement it exercises). No network: the Actions API is a
# fake; git operations run against the real repository.
#
# Run: python3 -m pytest tests/ci/test_orchestrate.py -q
# or: python3 tests/ci/test_orchestrate.py

import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orchestrate  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent


def git(*a):
    return subprocess.run(["git", "-C", str(REPO), *a], capture_output=True, text=True)


def head_shas():
    """Current tip and its parent (fresh per call: the branch advances)."""
    return (git("rev-parse", "HEAD").stdout.strip(),
            git("rev-parse", "HEAD~1").stdout.strip())


@pytest.fixture(autouse=True)
def clean_tags():
    for t in git("tag", "-l", "ci-ok-*").stdout.split():
        git("tag", "-d", t)
    yield
    for t in git("tag", "-l", "ci-ok-*").stdout.split():
        git("tag", "-d", t)


# ---------------------------------------------------------------------------
# ORCH-8: nearest successful base anchor over ci-ok-* tags.
# ---------------------------------------------------------------------------

def test_nearest_ancestor_prefers_closest():
    """ORCH-8: the minimal rev-list distance wins among ci-ok-* ancestors."""
    git("tag", "ci-ok-far", "HEAD~3")
    git("tag", "ci-ok-near", "HEAD~1")
    sha, dist = orchestrate.nearest_ancestor_sha()
    assert sha is not None
    # HEAD~1 must be chosen over HEAD~3 (dist could be >1 with merge parents,
    # but must be strictly less than the far one).
    far_dist = int(git("rev-list", "--count", f"{git('rev-parse','HEAD~3').stdout.strip()}..HEAD").stdout)
    assert dist < far_dist
    assert git("rev-parse", "HEAD~1").stdout.strip() == sha


def test_nearest_ancestor_ignores_non_ancestors():
    """ORCH-8: non-ancestor tags are never selected."""
    branches = git("for-each-ref", "refs/remotes/github", "--format=%(refname)").stdout.split()
    other = None
    for br in branches:
        sha = git("rev-parse", br).stdout.strip()
        if sha and git("merge-base", "--is-ancestor", sha, "HEAD").returncode != 0:
            other = sha
            break
    if other is None:
        pytest.skip("no non-ancestor commit available")
    git("tag", "ci-ok-other", other)
    git("tag", "ci-ok-anc", "HEAD~1")
    sha, _ = orchestrate.nearest_ancestor_sha()
    assert sha == git("rev-parse", "HEAD~1").stdout.strip()


def test_classification_kind():
    """ORCH-16/17: the git primitive behind the superseded/rewritten-history
    classification."""
    assert git("merge-base", "--is-ancestor", "HEAD~1", "HEAD").returncode == 0
    assert git("merge-base", "--is-ancestor", "HEAD", "HEAD~1").returncode != 0


# ---------------------------------------------------------------------------
# Planning core: the PR-wins same-SHA matrix (ORCH-13/14), pure.
# ---------------------------------------------------------------------------

def runinfo(rid, sha, event="push", status="in_progress", conclusion=None):
    return orchestrate.RunInfo(id=str(rid), head_sha=sha, status=status,
                               conclusion=conclusion, event=event)


def test_same_sha_matrix_pr_wins():
    """ORCH-13: the full PR-wins matrix, pure (ORCH-14: cancelled is
    neutral)."""
    sha = "a" * 40
    cases = [
        # (this_event, twin_event, twin_status, twin_conclusion, verdict)
        ("pull_request", "push", "in_progress", None, orchestrate.SUPERSEDE),
        ("pull_request", "push", "queued", None, orchestrate.SUPERSEDE),
        ("pull_request", "push", "completed", "success", orchestrate.DEFER),
        ("pull_request", "push", "completed", "failure", orchestrate.NEUTRAL),
        ("pull_request", "push", "completed", "cancelled", orchestrate.NEUTRAL),
        ("push", "pull_request", "in_progress", None, orchestrate.DEFER),
        ("push", "pull_request", "completed", "success", orchestrate.DEFER),
        ("push", "pull_request", "completed", "failure", orchestrate.NEUTRAL),
        ("push", "pull_request", "completed", "cancelled", orchestrate.NEUTRAL),
        ("push", "push", "in_progress", None, orchestrate.DEFER),
        ("pull_request", "pull_request", "in_progress", None, orchestrate.DEFER),
    ]
    for this, tev, st, con, want in cases:
        twin = runinfo(701, sha, event=tev, status=st, conclusion=con)
        got = orchestrate.same_sha_verdict(this, twin)
        assert got == want, f"{this} vs {tev}/{st}/{con}: {got} != {want}"


def test_plan_cancel_targets_before_accounting():
    """ORCH-20: the decision carries cancellation *targets*; the confirmed
    accounting happens in the effects layer."""
    head = "c" * 40
    d = orchestrate.plan(event="push", head_sha=head, run_id="1",
                         runs=[runinfo(500, "d" * 40)], skip_self=False, base_sha=None,
                         is_ancestor=lambda s: True,
                         followup_touches_common=lambda s: True,
                         estimate_cells=lambda s: [], select_gate=lambda: "cell-a")
    assert d.cancel_ids == ["500"]
    assert d.cells == "cell-a"


def test_plan_preserve_invariant():
    """INV-4: preserved cells plus the spawned complement cover the new
    selection (issue #224, ORCH-18)."""
    head = "c" * 40
    d = orchestrate.plan(event="push", head_sha=head, run_id="1",
                         runs=[runinfo(500, "d" * 40)], skip_self=False, base_sha="e" * 40,
                         is_ancestor=lambda s: True,
                         followup_touches_common=lambda s: False,
                         estimate_cells=lambda s: ["cell-b", "cell-c"],
                         select_gate=lambda: "cell-a\ncell-b\ncell-c")
    assert d.preserved == ["500"]
    spawn = set(d.cells.splitlines())
    assert spawn == {"cell-a"}
    assert spawn | set(d.preserved_cells) >= {"cell-a", "cell-b", "cell-c"}


# ---------------------------------------------------------------------------
# Harness: fake Actions API; real git for classification/anchors/tags.
# ---------------------------------------------------------------------------

def staged_run(rid, sha, status="in_progress", conclusion=None, event="push"):
    return {"id": rid, "head_sha": sha, "status": status,
            "conclusion": conclusion, "event": event, "run_number": rid}


class FakeApi:
    """Routes orchestrate.api calls (ORCH-4/7): branch-tip lookups, run
    listings (the head branch and the per-branch anchor success queries),
    cancel POSTs. Listings honor pagination (page >= 2 is empty)."""

    def __init__(self, head_runs=None, anchor_by_branch=None, tip=None,
                 head_branch="feature/x", open_prs=False):
        self.head_runs = head_runs or []
        self.anchor_by_branch = anchor_by_branch or {}
        self.tip = tip
        self.head_branch = head_branch
        self.open_prs = open_prs
        self.cancelled = []
        self.listing_queries = []

    def __call__(self, repo, path, token, method="GET", payload=None):
        if path.startswith("branches/"):
            assert self.tip is not None, "unexpected branch-tip lookup"
            return {"commit": {"sha": self.tip}}
        if path.startswith("pulls?"):
            return [{"number": 1}] if self.open_prs else []
        assert path.startswith("actions/workflows/"), path
        if method == "POST" and "/cancel" in path:
            self.cancelled.append(int(path.rstrip("/").rsplit("/", 1)[1]))
            return {}
        assert method == "GET", method
        self.listing_queries.append(path)
        if "page=1" not in path:
            return {"workflow_runs": []}
        if "status=success" in path:
            branch = path.split("branch=")[-1].split("&")[0]
            return {"workflow_runs": list(self.anchor_by_branch.get(branch, []))}
        branch = path.split("branch=")[-1].split("&")[0]
        return {"workflow_runs": list(self.head_runs) if branch == self.head_branch else []}


def run_main(monkeypatch, capsys, fake, head_ref, head_sha, run_id="999",
             base_ref="origin/master", event="push", cells="cell-a\ncell-b",
             tip=None):
    """Run orchestrate.main() with the fake API, a recording cancel_run and a
    stubbed select-cells."""
    monkeypatch.setattr(orchestrate, "api", fake)

    monkeypatch.setattr(orchestrate, "list_runs",
                        lambda repo, token, workflow_id, branch=None, event=None,
                        status=None: _fake_list(fake, branch, event, status))

    def fake_cancel(repo, token, rid, dry_run):
        fake.cancelled.append(rid)
        return True

    monkeypatch.setattr(orchestrate, "cancel_run", fake_cancel)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: cells)
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", head_ref, "--head-sha", head_sha,
        "--base-ref", base_ref, "--event", event, "--run-id", run_id])
    assert rc == 0
    return json.loads(capsys.readouterr().out)


def _fake_list(fake, branch, event, status):
    """Pagination-aware routing over the fake: the head branch gets the
    staged runs (page 1), the anchor success queries get their per-branch
    staged runs, page >= 2 is empty."""
    fake.listing_queries.append((branch, event, status))
    if status == "success":
        return list(fake.anchor_by_branch.get(branch, []))
    if branch == fake.head_branch:
        return list(fake.head_runs)
    return []


def test_push_prefixed_head_ref_normalizes_and_supersedes(monkeypatch, capsys):
    """ORCH-5: push events pass refs/heads/<branch>; the API filter needs the
    bare name, so the orchestrator must normalize (else no supersede on
    push)."""
    head, old = head_shas()
    fake = FakeApi(head_runs=[staged_run(101, old)], tip=head)
    out = run_main(monkeypatch, capsys, fake, "refs/heads/feature/x", head)
    assert any(q[0] == "feature/x" for q in fake.listing_queries)
    assert not [q for q in fake.listing_queries if q[0] and q[0].startswith("refs/heads/")]
    assert out["cancel_ids"] == ["101"]
    assert out["skip"] is False


def test_empty_head_ref_disables_supersede_and_dedup(monkeypatch, capsys):
    """ORCH-7: repository_dispatch has no branch context: the orchestrator
    must not touch other branches' runs and must not treat them as
    duplicates."""
    head, old = head_shas()
    fake = FakeApi(head_runs=[staged_run(201, old), staged_run(202, head)])
    out = run_main(monkeypatch, capsys, fake, "", head)
    assert fake.cancelled == []
    assert out["cancel_ids"] == []
    assert out["skip"] is False


def test_bare_head_ref_supersedes_ancestor_run(monkeypatch, capsys):
    """ORCH-16: a bare branch name supersedes an ancestor run."""
    head, old = head_shas()
    fake = FakeApi(head_runs=[staged_run(101, old)], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["cancel_ids"] == ["101"]
    assert out["skip"] is False


def test_same_sha_duplicate_skips(monkeypatch, capsys):
    """ORCH-13: a live same-event same-SHA twin is authoritative."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(101, head)], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["skip"] is True
    assert out["cancel_ids"] == []
    assert out["cells"] == ""


def test_pr_run_supersedes_alive_push_duplicate(monkeypatch, capsys):
    """ORCH-13: the PR gate outranks the push one — a PR run facing a live
    same-SHA push run supersedes it and runs the gate itself, instead of both
    runs skipping each other into a zero-coverage deadlock (the post-merge
    Windows failure of gh0463)."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(701, head, event="push")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="pull_request")
    assert out["skip"] is False
    assert out["cancel_ids"] == ["701"]
    assert fake.cancelled == ["701"]
    assert out["cells"] == "cell-a\ncell-b"


def test_pr_run_skips_on_successful_push_duplicate(monkeypatch, capsys):
    """ORCH-13: coverage already provided by the successful push twin."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(702, head, event="push", status="completed",
                                         conclusion="success")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="pull_request")
    assert out["skip"] is True
    assert out["cancel_ids"] == []


def test_pr_run_provides_signal_after_failed_push_duplicate(monkeypatch, capsys):
    """ORCH-13: the failed push twin left the gate empty — the PR run
    provides the signal (INV-1: never green with an empty matrix)."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(703, head, event="push", status="completed",
                                         conclusion="failure")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="pull_request")
    assert out["skip"] is False
    assert out["cancel_ids"] == []
    assert out["cells"] == "cell-a\ncell-b"


def test_push_run_defers_to_alive_pr_run(monkeypatch, capsys):
    """ORCH-13: the push leg defers to a live pull_request twin."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(704, head, event="pull_request")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="push")
    assert out["skip"] is True
    assert fake.cancelled == []


def test_push_run_provides_signal_after_failed_pr_run(monkeypatch, capsys):
    """ORCH-13: the failed PR twin left the gate empty — the push run
    provides the signal."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(705, head, event="pull_request",
                                         status="completed", conclusion="failure")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="push")
    assert out["skip"] is False
    assert out["cells"] == "cell-a\ncell-b"


def test_push_run_defers_to_open_pr_green(monkeypatch, capsys):
    """ORCH-25: the push leg defers to an open pull request for its branch —
    a green self-skip instead of being cancelled red by the PR run."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[], tip=head, open_prs=True)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="push")
    assert out["skip"] is True
    assert fake.cancelled == []
    assert out["cells"] == ""


def test_push_run_runs_when_no_open_pr(monkeypatch, capsys):
    """ORCH-25: without an open PR the push gate is authoritative and runs."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[], tip=head, open_prs=False)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="push")
    assert out["skip"] is False
    assert out["cells"] == "cell-a\ncell-b"


def test_cancelled_same_sha_twin_is_neutral(monkeypatch, capsys):
    """ORCH-14: a cancelled same-SHA twin produced no result — this run
    proceeds (no skip, nothing to cancel)."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(706, head, event="push", status="completed",
                                         conclusion="cancelled")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, event="pull_request")
    assert out["skip"] is False
    assert fake.cancelled == []
    assert out["cells"] == "cell-a\ncell-b"


def test_own_run_is_never_a_cancel_candidate(monkeypatch, capsys):
    """ORCH-6: the run's own listing entry is exempt from dedup and is never
    a cancel candidate (the int/string id mismatch once broke the
    exemption)."""
    head, old = head_shas()
    fake = FakeApi(head_runs=[staged_run(501, old),
                              staged_run(999, head, status="in_progress")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="999")
    assert fake.cancelled == ["501"]
    assert out["cancel_ids"] == ["501"]
    assert out["skip"] is False


def test_plain_push_does_not_skip_on_own_entry(monkeypatch, capsys):
    """ORCH-6: a push with no other run on the branch must not deduplicate
    against its own listing entry (that once silently produced an empty
    gate)."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(999, head, status="in_progress")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="999")
    assert out["skip"] is False
    assert fake.cancelled == []
    assert out["cells"] == "cell-a\ncell-b"


def test_empty_run_id_disables_own_run_exemption(monkeypatch, capsys):
    """ORCH-6: an empty --run-id disables the own-run exemption (documented
    edge) — the run's own entry is then treated as a foreign same-SHA
    duplicate and the gate skips."""
    head, _ = head_shas()
    fake = FakeApi(head_runs=[staged_run(777, head, status="in_progress")], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="")
    assert out["skip"] is True


def test_rewritten_history_run_is_cancelled(monkeypatch, capsys):
    """ORCH-17: a run whose head is not an ancestor (force-pushed away) is
    cancelled as rewritten history."""
    head, _ = head_shas()
    other = git("rev-parse", "stable").stdout.strip()
    if not other or git("merge-base", "--is-ancestor", other, "HEAD").returncode == 0:
        pytest.skip("no non-ancestor commit available")
    fake = FakeApi(head_runs=[staged_run(303, other)], tip=head)
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["cancel_ids"] == ["303"]
    assert out["skip"] is False


def test_stale_queued_run_skips_instead_of_cancelling(monkeypatch, capsys):
    """ORCH-21: a queued run waking after the branch tip moved skips itself
    and never cancels the current lineage's runs; the stale output shape is
    cells="" with no preserved runs."""
    head, _ = head_shas()
    other = git("rev-parse", "stable").stdout.strip()
    if not other or git("merge-base", "--is-ancestor", other, "HEAD").returncode == 0:
        pytest.skip("no non-ancestor commit available")
    fake = FakeApi(head_runs=[staged_run(601, head, status="in_progress")], tip=head)
    # this run's head is `other`, the branch tip is already `head`
    out = run_main(monkeypatch, capsys, fake, "feature/x", other, tip=head)
    assert out["skip"] is True
    assert fake.cancelled == []
    assert out["cancel_ids"] == []
    assert out["cells"] == ""
    assert out["preserved"] == []


def test_branch_tip_lookup_failure_keeps_old_behavior(monkeypatch, capsys):
    """ORCH-22: a failed branch-tip lookup assumes this run is current (no
    stale skip), and the run proceeds with its gate."""
    head, old = head_shas()
    fake = FakeApi(head_runs=[staged_run(602, old)], tip=head)

    def failing_api(repo, path, token, method="GET", payload=None):
        if path.startswith("branches/"):
            raise RuntimeError("branches lookup unavailable")
        return fake(repo, path, token, method, payload)

    monkeypatch.setattr(orchestrate, "api", failing_api)
    monkeypatch.setattr(orchestrate, "cancel_run",
                        lambda repo, token, rid, dry_run: fake.cancelled.append(rid) or True)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "cell-a\ncell-b")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", head,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999"])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["skip"] is False
    assert out["cells"] == "cell-a\ncell-b"


# ---------------------------------------------------------------------------
# Preserve-unaffected supersede (issue #224; ORCH-18, INV-4).
# ---------------------------------------------------------------------------

def make_commit(parent, edits):
    """Throwaway commit on top of `parent` (unreachable from any branch).

    edits: {path: bytes}. Built via plumbing so tests control the diff
    without touching branches or the index.
    """
    def write_blob(data):
        r = subprocess.run(["git", "-C", str(REPO), "hash-object", "-w", "--stdin"],
                           input=data, capture_output=True)
        assert r.returncode == 0, r.stderr
        return r.stdout.decode().strip()

    def mktree(entries):
        lines = [f"{m} {t} {s}\t{n}" for n, (m, t, s) in entries.items()]
        r = subprocess.run(["git", "-C", str(REPO), "mktree"],
                           input="\n".join(lines) + ("\n" if lines else ""),
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        return r.stdout.strip()

    def update_tree(tree, parts, blob):
        entries = {}
        for ln in git("ls-tree", tree).stdout.splitlines():
            meta, name = ln.split("\t", 1)
            m, t, s = meta.split()
            entries[name] = (m, t, s)
        name = parts[0]
        if len(parts) == 1:
            entries[name] = ("100644", "blob", blob)
        else:
            sub = entries.get(name)
            sub_tree = sub[2] if sub else mktree({})
            entries[name] = ("040000", "tree", update_tree(sub_tree, parts[1:], blob))
        return mktree(entries)

    tree = git("rev-parse", f"{parent}^{{tree}}").stdout.strip()
    for path, data in edits.items():
        tree = update_tree(tree, path.split("/"), write_blob(data))
    r = git("commit-tree", tree, "-p", parent, "-m", "ci-test: throwaway supersede commit")
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def test_docs_only_followup_push_preserves_inprogress_run(monkeypatch, capsys):
    """ORCH-18: a follow-up commit touching no common code keeps the
    in-flight run alive (its cells are provably unaffected) and spawns only
    the complement of the preserved selection (issue #224)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old = head                                   # the in-flight run's SHA
    new = make_commit(head, {"docs/supersede-probe.md": b"probe note\n"})
    git("tag", "ci-ok-77", base)
    fake = FakeApi(head_runs=[staged_run(401, old)], tip=new)
    new_sel = ["cell-a", "cell-b", "cell-c"]
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="\n".join(new_sel), tip=new)
    assert out["preserved"] == ["401"]
    assert 401 not in out["cancel_ids"]
    preserved_cells = set(out["preserved_cells"])
    assert preserved_cells, "the estimator must classify the old selection"
    spawn = set(out["cells"].splitlines()) if out["cells"] not in ("", "[]") else set()
    assert spawn == set(new_sel) - preserved_cells          # complement math
    assert spawn | preserved_cells >= set(new_sel)          # INV-4 coverage


def test_common_code_followup_push_cancels_all(monkeypatch, capsys):
    """ORCH-16: a follow-up commit touching common code invalidates the
    in-flight cells: cancel-and-rerun as before (regression guard)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old = head
    new = make_commit(head, {"src/probe-supersede.h": b"/* probe */\n"})
    git("tag", "ci-ok-78", base)
    fake = FakeApi(head_runs=[staged_run(402, old)], tip=new)
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="cell-a\ncell-b", tip=new)
    assert out["cancel_ids"] == ["402"]
    assert out["preserved"] == []
    assert set(out["cells"].splitlines()) == {"cell-a", "cell-b"}


def test_preserved_only_run_emits_empty_matrix(monkeypatch, capsys):
    """INV-4: when everything selected is already in flight, the cells output
    must be the literal [] (run job skipped), never an empty string (which
    the workflow treats as a fatal selection signal)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old = head
    new = make_commit(head, {"docs/supersede-probe.md": b"probe note\n"})
    git("tag", "ci-ok-79", base)
    est = orchestrate.cells_for_paths(
        [p for p in git("diff", "--name-only", f"{base}...{old}").stdout.splitlines() if p])
    fake = FakeApi(head_runs=[staged_run(404, old)], tip=new)
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="\n".join(est), tip=new)
    assert out["preserved"] == ["404"]
    assert out["cells"] == "[]"


def test_multiple_preserved_runs_merge_estimates(monkeypatch, capsys):
    """ORCH-18/INV-4: two preserved runs merge their estimated selections
    into one preserved_cells set; the spawn is the complement of the union
    (previously untested)."""
    head, _ = head_shas()
    base = git("rev-parse", "HEAD~1").stdout.strip()
    old1, old2 = head, make_commit(head, {"docs/supersede-2.md": b"second\n"})
    new = make_commit(old2, {"docs/supersede-probe.md": b"probe note\n"})
    git("tag", "ci-ok-80", base)
    fake = FakeApi(head_runs=[staged_run(405, old1), staged_run(406, old2)], tip=new)
    out = run_main(monkeypatch, capsys, fake, "feature/x", new,
                   cells="cell-a\ncell-b\ncell-c", tip=new)
    assert sorted(out["preserved"]) == ["405", "406"]
    spawn = set(out["cells"].splitlines()) if out["cells"] not in ("", "[]") else set()
    assert spawn | set(out["preserved_cells"]) >= {"cell-a", "cell-b", "cell-c"}


# ---------------------------------------------------------------------------
# Anchor hygiene: dead shas and the recency window (ORCH-9).
# ---------------------------------------------------------------------------

DEAD_SHA = "1234567890abcdef1234567890abcdef12345678"


def test_tag_successful_runs_skips_dead_shas(monkeypatch, capsys):
    """ORCH-9: successful runs whose shas were rewritten away cannot be
    tagged and cannot anchor the diff: skip them with one summary line, not a
    per-sha git error dump."""
    head, _ = head_shas()
    runs = [{"run_number": 1, "head_sha": head},
            {"run_number": 2, "head_sha": DEAD_SHA}]
    made = orchestrate.tag_successful_runs(runs, dry_run=False)
    assert ("ci-ok-1", head) in made
    assert all(t != "ci-ok-2" for t, _ in made)
    err = capsys.readouterr().err
    assert "skipped 1" in err and "failed" not in err


def test_list_runs_restricts_successful_queries_to_recency(monkeypatch):
    """ORCH-9: anchor queries search recent successful runs only: ancient
    shas are almost always rewritten away on this repo."""
    captured = []

    def fake_api(repo, path, token, method="GET", payload=None):
        captured.append(path)
        return {"workflow_runs": []}

    monkeypatch.setattr(orchestrate, "api", fake_api)
    orchestrate.list_runs("r", "t", "ci-dispatch.yml",
                          branch="master", event="push", status="success")
    assert "created=" in captured[-1]
    orchestrate.list_runs("r", "t", "ci-dispatch.yml", branch="feature/x")
    assert "created=" not in captured[-1]


def test_list_runs_follows_pagination(monkeypatch):
    """ORCH (contract): the listing must be complete — pagination is
    followed instead of silently truncating at 100 entries (previously
    untested)."""
    pages = [[staged_run(900 + i, "a" * 40) for i in range(100)],
             [staged_run(1000 + i, "a" * 40) for i in range(100)],
             [staged_run(1100, "a" * 40)]]
    queries = []

    def fake_api(repo, path, token, method="GET", payload=None):
        queries.append(path)
        page = int(path.split("&page=")[1].split("&")[0])
        return {"workflow_runs": pages[page - 1] if page <= len(pages) else []}

    monkeypatch.setattr(orchestrate, "api", fake_api)
    runs = orchestrate.list_runs("r", "t", "ci-dispatch.yml", branch="feature/x")
    assert len(runs) == 201
    assert sum("page=" in q for q in queries) == 3


# ---------------------------------------------------------------------------
# Fail-safe and gates (INV-3, ORCH-7; previously untested policies).
# ---------------------------------------------------------------------------

def test_empty_token_fails_safe_to_full_gate(monkeypatch, capsys):
    """INV-3: a missing token degrades to skip=false + full-gate cells
    instead of crashing or skipping (previously untested)."""
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "full-cell")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "", "--head-ref", "feature/x", "--head-sha", "a" * 40,
        "--base-ref", "origin/master", "--event", "push"])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["skip"] is False
    assert out["cells"] == "full-cell"
    assert out["cancel_ids"] == []


def test_internal_failure_fails_safe_to_full_gate(monkeypatch, capsys):
    """INV-3: an internal error (here: the branch listing explodes) degrades
    to the full gate instead of a failure (previously untested)."""
    head, _ = head_shas()

    def exploding_api(repo, path, token, method="GET", payload=None):
        if path.startswith("pulls?"):
            return []                          # no open PR: the push gate runs
        if "branch=feature" in path:
            raise RuntimeError("listing unavailable")
        if path.startswith("branches/"):
            return {"commit": {"sha": head}}
        return {"workflow_runs": []}

    monkeypatch.setattr(orchestrate, "api", exploding_api)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "full-cell")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", head,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999"])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["skip"] is False
    assert out["cells"] == "full-cell"


def test_base_fallback_to_devel_second_candidate(monkeypatch, capsys):
    """ORCH-10: when the requested base yields no anchor, origin/devel is
    tried as the fallback candidate (previously untested)."""
    head, _ = head_shas()
    anchor_sha = git("rev-parse", "HEAD~1").stdout.strip()

    def fake_api(repo, path, token, method="GET", payload=None):
        if path.startswith("branches/"):
            return {"commit": {"sha": head}}
        if path.startswith("pulls?"):
            return []                          # no open PR: the push gate runs
        if "branch=master" in path:
            return {"workflow_runs": []}       # no successful master runs
        if "branch=devel" in path:
            return {"workflow_runs": [staged_run(801, anchor_sha, event="push",
                                                 status="completed",
                                                 conclusion="success")]}
        return {"workflow_runs": []}

    monkeypatch.setattr(orchestrate, "api", fake_api)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "cell-a\ncell-b")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", head,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999"])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["skip"] is False
    assert out["cells"] == "cell-a\ncell-b"


def test_dry_run_never_cancels_or_tags(monkeypatch, capsys):
    """ORCH-24: --dry-run prints the plan without cancelling runs or creating
    (or deleting) tags (previously untested)."""
    head, old = head_shas()
    git("tag", "ci-ok-88", old)   # pre-existing tag: dry-run must keep it
    fake = FakeApi(head_runs=[staged_run(603, old)], tip=head)

    monkeypatch.setattr(orchestrate, "api", fake)
    # the real cancel_run honors --dry-run itself: it must not POST
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "cell-a")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", head,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999",
        "--dry-run"])
    assert rc == 0
    assert fake.cancelled == []   # no cancel POST under --dry-run
    assert git("rev-parse", "ci-ok-88").returncode == 0  # the tag survived
    out = json.loads(capsys.readouterr().out)
    assert out["skip"] is False


def test_select_cells_failure_fails_safe(monkeypatch, capsys):
    """INV-3: a select-cells subprocess failure degrades to the fail-safe
    full-gate attempt instead of a crash (previously untested)."""
    head, _ = head_shas()

    def failing_select(base_sha=None, profile=None):
        raise RuntimeError("select-cells exploded")

    monkeypatch.setattr(orchestrate, "api",
                        lambda repo, path, token, method="GET", payload=None:
                        {"commit": {"sha": head}} if path.startswith("branches/")
                        else ([] if path.startswith("pulls?") else {"workflow_runs": []}))
    monkeypatch.setattr(orchestrate, "select_cells", failing_select)
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", head,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999"])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["skip"] is False
    assert out["cancel_ids"] == []


def test_cleanup_tags_runs_in_finally(monkeypatch, capsys):
    """ORCH-8: temporary ci-ok-* tags are always removed — even when the
    planning raised (previously untested)."""
    git("tag", "ci-ok-pre", "HEAD")
    monkeypatch.setattr(orchestrate, "api",
                        lambda repo, path, token, method="GET", payload=None:
                        (_ for _ in ()).throw(RuntimeError("api exploded")))
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", "a" * 40,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999"])
    assert rc == 0
    assert git("tag", "-l", "ci-ok-pre").stdout.strip() == ""  # cleaned in finally


def test_cancel_accounting_records_confirmed_only(monkeypatch, capsys):
    """ORCH-20: a failed cancellation is logged but not counted: cancel_ids
    carries confirmed cancellations only (previously the target was recorded
    unconditionally)."""
    head, old = head_shas()
    fake = FakeApi(head_runs=[staged_run(501, old)], tip=head)
    monkeypatch.setattr(orchestrate, "api", fake)

    def failing_cancel(repo, token, rid, dry_run):
        return False   # the cancel POST failed

    monkeypatch.setattr(orchestrate, "cancel_run", failing_cancel)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "cell-a")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", "feature/x", "--head-sha", head,
        "--base-ref", "origin/master", "--event", "push", "--run-id", "999"])
    assert rc == 0
    out = json.loads(capsys.readouterr().out)
    assert out["cancel_ids"] == []   # not confirmed → not reported
    assert out["skip"] is False


# ---------------------------------------------------------------------------
# Selection helpers (ORCH-3/18: delegation and the two-tier estimate).
# ---------------------------------------------------------------------------

def test_cells_for_paths_two_tiers():
    """ORCH-18: a common-prefix hit selects the change-addressed gate, an
    isolated path selects push-quick; empty input selects nothing (direct;
    previously untested)."""
    gate = orchestrate.cells_for_paths(["src/cursor.c"])
    quick = orchestrate.cells_for_paths(["docs/notes.md"])
    isolated = orchestrate.cells_for_paths([])
    assert isinstance(gate, list) and len(gate) >= len(quick) > 0
    assert isolated == []
    assert set(quick).issubset(set(gate))   # push-quick ⊂ gate


def test_diff_paths_three_dot():
    """ORCH-18: diff_paths uses the three-dot form (changes introduced on the
    side of `b` since the merge base with `a`; direct; previously
    untested)."""
    base = git("rev-parse", "HEAD~2").stdout.strip()
    tip = git("rev-parse", "HEAD").stdout.strip()
    paths = orchestrate.diff_paths(base, tip)
    expected = [p for p in git("diff", "--name-only", f"{base}...{tip}").stdout.splitlines() if p]
    assert paths == expected


def test_stale_gate_combination_matrix(monkeypatch, capsys):
    """INV-1 exhaustive: for every twin state × staleness × anchor presence,
    a skip always carries its coverage reason, and a non-skip never carries
    an empty gate (the planning core is pure, so the full matrix is cheap)."""
    head = "f" * 40
    twins = [runinfo(901, head, event="push", status="in_progress"),
             runinfo(902, head, event="push", status="completed", conclusion="success"),
             runinfo(903, head, event="push", status="completed", conclusion="failure"),
             runinfo(904, head, event="push", status="completed", conclusion="cancelled"),
             runinfo(905, head, event="pull_request", status="in_progress"),
             runinfo(906, head, event="pull_request", status="completed",
                     conclusion="success")]
    for twin in twins:
        for stale in (False, True):
            for base_sha in (None, "e" * 40):
                d = orchestrate.plan(event="pull_request", head_sha=head, run_id="999",
                                     runs=[twin], skip_self=stale, base_sha=base_sha,
                                     is_ancestor=lambda s: False,
                                     followup_touches_common=lambda s: False,
                                     estimate_cells=lambda s: [],
                                     select_gate=lambda: "cell-a")
                if d.skip:
                    assert d.cells == ""            # INV-1
                else:
                    assert d.cells, "INV-1: empty gate without skip"
                if stale:
                    assert d.skip is True           # ORCH-21
                    assert d.cancel_ids == []       # a stale run cancels nothing
