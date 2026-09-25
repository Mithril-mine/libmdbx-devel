"""Тесты графа связей: link / unlink / graph."""

import pytest

from mcp.errors import MemoryError
from tests.conftest import seed_vocab


def setup(s):
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi"), ("testing", "split-monolith")])
    s.safe_store("bug:crypto:alignment", "bug", "Segfault misaligned.", 0.7)
    s.safe_store("fact:platform:android-abi", "fact", "Android ABIs.", 0.9)
    s.safe_store("decision:testing:split-monolith", "decision",
                 "Split monolithic test suite.", 0.9)
    return s


def test_link_creates_and_graph(store):
    setup(store)
    r = store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    assert r["result"] == "linked"
    g = store.graph("bug:crypto:alignment", depth=1)
    assert g["nodes"] == ["bug:crypto:alignment", "fact:platform:android-abi"]
    assert len(g["edges"]) == 1
    assert g["edges"][0]["predicate"] == "related-to"


def test_link_exists_second_time(store):
    setup(store)
    store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    r = store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    assert r["result"] == "exists"


def test_graph_depth2(store):
    setup(store)
    store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    store.link("decision:testing:split-monolith", "related-to", "bug:crypto:alignment")
    g = store.graph("fact:platform:android-abi", depth=2)
    assert "bug:crypto:alignment" in g["nodes"]
    assert "decision:testing:split-monolith" in g["nodes"]


def test_unlink(store):
    setup(store)
    store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    r = store.unlink("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    assert r["result"] == "unlinked"
    g = store.graph("bug:crypto:alignment", depth=1)
    assert len(g["edges"]) == 0
    # повторный unlink -> notfound
    r2 = store.unlink("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    assert r2["result"] == "notfound"


def test_link_missing_subject(store):
    setup(store)
    with pytest.raises(MemoryError) as ei:
        store.link("fact:platform:android", "related-to", "fact:platform:android-abi")
    assert "субъект" in str(ei.value)


def test_link_missing_object(store):
    setup(store)
    with pytest.raises(MemoryError):
        store.link("bug:crypto:alignment", "related-to", "fact:platform:android")


def test_link_bad_predicate(store):
    setup(store)
    with pytest.raises(MemoryError):
        store.link("bug:crypto:alignment", "bad:pred", "fact:platform:android-abi")


def test_graph_unknown_key(store):
    setup(store)
    with pytest.raises(MemoryError):
        store.graph("fact:platform:android", depth=1)