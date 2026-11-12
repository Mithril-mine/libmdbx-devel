# Unit tests for tests/ci/orchestrate.py pure helpers (no network).
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


def test_nearest_ancestor_prefers_closest():
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
    # A tag on a commit that is not an ancestor of HEAD must be ignored.
    # We use an unrelated ref if available; otherwise create one via a sibling
    # branch tip. Simpler: a tag pointing at a commit in another branch.
    branches = git("for-each-ref", "refs/remotes/origin", "--format=%(refname)").stdout.split()
    other = None
    for br in branches:
        sha = git("rev-parse", br).stdout.strip()
        if sha and git("merge-base", "--is-ancestor", sha, "HEAD").returncode != 0:
            other = sha
            break
    if other is None:
        pytest.skip("no non-ancestor commit available")
    git("tag", "ci-ok-other", other)
    # If only a non-ancestor tag exists, nothing is selected.
    # (With no ancestor tags at all, nearest returns (None, None).)
    # To keep the test independent of other ci-ok-* tags (none in repo), assert
    # that the non-ancestor alone is not chosen when an ancestor exists.
    git("tag", "ci-ok-anc", "HEAD~1")
    sha, _ = orchestrate.nearest_ancestor_sha()
    assert sha == git("rev-parse", "HEAD~1").stdout.strip()


def test_classification_kind():
    # rewritten-history vs superseded classification is done inline in main();
    # here we at least assert the git primitive used for it behaves as expected.
    assert git("merge-base", "--is-ancestor", "HEAD~1", "HEAD").returncode == 0
    assert git("merge-base", "--is-ancestor", "HEAD", "HEAD~1").returncode != 0


# ---------------------------------------------------------------------------
# Supersede/dedup planning (fake Actions API; real git for classification).
# ---------------------------------------------------------------------------

def staged_run(rid, sha, status="in_progress", conclusion=None, event="push"):
    return {"id": rid, "head_sha": sha, "status": status,
            "conclusion": conclusion, "event": event, "run_number": rid}


class FakeRuns:
    """Stands in for orchestrate.list_runs/cancel_runs.

    staged: branch name -> list of run dicts; the Actions API `branch=`
    filter expects the bare branch name, so a query with a `refs/heads/`
    prefix finds nothing staged — exactly like the real service.
    """

    def __init__(self, staged=None):
        self.staged = staged or {}
        self.queries = []
        self.cancelled = []

    def list_runs(self, repo, token, workflow_id, branch=None, event=None,
                  status=None, per_page=100):
        self.queries.append(branch)
        return list(self.staged.get(branch, []))

    def cancel_runs(self, repo, token, run_ids, dry_run):
        self.cancelled.extend(run_ids)
        return list(run_ids)


def run_main(monkeypatch, capsys, fake, head_ref, head_sha, run_id="999",
             base_ref="origin/master", event="push"):
    monkeypatch.setattr(orchestrate, "list_runs", fake.list_runs)
    monkeypatch.setattr(orchestrate, "cancel_runs", fake.cancel_runs)
    monkeypatch.setattr(orchestrate, "select_cells",
                        lambda base_sha=None, profile=None: "cell-a\ncell-b")
    rc = orchestrate.main([
        "--repo", "acme/repo", "--workflow-id", "ci-dispatch.yml",
        "--token", "fake-token", "--head-ref", head_ref, "--head-sha", head_sha,
        "--base-ref", base_ref, "--event", event, "--run-id", run_id])
    assert rc == 0
    return json.loads(capsys.readouterr().out)


def test_push_prefixed_head_ref_normalizes_and_supersedes(monkeypatch, capsys):
    """Push events pass refs/heads/<branch>; the API filter needs the bare
    name, so the orchestrator must normalize (else no supersede on push)."""
    head, old = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(101, old)]})
    out = run_main(monkeypatch, capsys, fake, "refs/heads/feature/x", head)
    assert fake.queries, "head branch must be queried"
    assert not [q for q in fake.queries if q and q.startswith("refs/heads/")]
    assert "feature/x" in fake.queries
    assert out["cancel_ids"] == [101]
    assert out["skip"] is False


def test_empty_head_ref_disables_supersede_and_dedup(monkeypatch, capsys):
    """repository_dispatch has no branch context: the orchestrator must not
    touch runs of other branches and must not treat them as duplicates."""
    head, old = head_shas()
    cross = [staged_run(201, old), staged_run(202, head)]
    fake = FakeRuns({"": cross})
    out = run_main(monkeypatch, capsys, fake, "", head)
    assert fake.cancelled == []
    assert out["cancel_ids"] == []
    assert out["skip"] is False


def test_bare_head_ref_supersedes_ancestor_run(monkeypatch, capsys):
    head, old = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(101, old)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["cancel_ids"] == [101]
    assert out["skip"] is False


def test_same_sha_duplicate_skips(monkeypatch, capsys):
    head, _ = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(101, head)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["skip"] is True
    assert out["cancel_ids"] == []
    assert out["cells"] == ""


def test_rewritten_history_run_is_cancelled(monkeypatch, capsys):
    head, _ = head_shas()
    other = git("rev-parse", "stable").stdout.strip()
    if not other or git("merge-base", "--is-ancestor", other, "HEAD").returncode == 0:
        pytest.skip("no non-ancestor commit available")
    fake = FakeRuns({"feature/x": [staged_run(303, other)]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head)
    assert out["cancel_ids"] == [303]
    assert out["skip"] is False


def test_own_run_is_never_a_cancel_candidate(monkeypatch, capsys):
    """The run's own entry appears in the branch listing while it is
    in_progress; it must never fall into the cancel path (a live
    self-cancel race seen on CI: the run killed itself mid-resolve).
    The int-vs-string id comparison must not break the exemption."""
    head, old = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(501, old),
                                   staged_run(999, head, status="in_progress")]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="999")
    assert fake.cancelled == [501]
    assert out["cancel_ids"] == [501]
    assert out["skip"] is False


def test_plain_push_does_not_skip_on_own_entry(monkeypatch, capsys):
    """A push with no other run on the branch must NOT be deduplicated
    against its own listing entry: with the int/string id mismatch the run
    saw itself as a foreign duplicate and silently produced an empty gate."""
    head, _ = head_shas()
    fake = FakeRuns({"feature/x": [staged_run(999, head, status="in_progress")]})
    out = run_main(monkeypatch, capsys, fake, "feature/x", head, run_id="999")
    assert out["skip"] is False
    assert fake.cancelled == []
    assert out["cells"] != ""


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
