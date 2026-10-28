# Unit tests for tests/ci/orchestrate.py pure helpers (no network).
#
# Run: python3 -m pytest tests/ci/test_orchestrate.py -q
# or: python3 tests/ci/test_orchestrate.py

import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import orchestrate  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent


def git(*a):
    return subprocess.run(["git", "-C", str(REPO), *a], capture_output=True, text=True)


@pytest.fixture(autouse=True)
def clean_tags():
    git("tag", "-l", "ci-ok-*")
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


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
