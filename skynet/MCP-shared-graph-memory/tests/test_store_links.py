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


def test_purge_removes_incoming_fwd_halves(store):
    """A4: purge записи удаляет и fwd-половины входящих рёбер (без висяков).

    Раньше _delete_links_of удалял только back:-рёбра записи, а чужие
    fwd-рёбра на неё оставались висячими; плюс полный скан links. Теперь
    оба конца удаляются, чужие рёбра и счётчики входящих не задеты.
    """
    setup(store)
    store.safe_store("fact:crypto:extra", "fact", "Extra note.", 0.5)
    # два входящих на target + одно исходящее из target + чужое независимое
    store.link("bug:crypto:alignment", "related-to", "fact:crypto:extra")
    store.link("fact:platform:android-abi", "related-to", "fact:crypto:extra")
    store.link("fact:crypto:extra", "part-of", "bug:crypto:alignment")
    store.link("decision:testing:split-monolith", "related-to",
               "bug:crypto:alignment")
    # входящие у bug:crypto:alignment: от extra(part-of) и от decision — 2.
    # _id читаем в ТОЙ ЖЕ read-txn (get_record открыл бы вложенную — BAD_RSLOT)
    import json as _json
    with store.env.begin(readonly=True) as txn:
        rc, rv = txn.get(store.dbi(txn, "records"),
                         b"bug:crypto:alignment")
        bug_id = _json.loads(rv.decode())["_id"]
        inc_bug_before = store._incoming_count(txn, bug_id)
    assert inc_bug_before == 2

    r = store.purge(["fact:crypto:extra"])
    assert r["removed"] == ["fact:crypto:extra"]
    # чужие рёбра на extra исчезли (нет висячих fwd-половин): graph от
    # bug:crypto:alignment не показывает extra
    g = store.graph("bug:crypto:alignment", depth=2)
    for e in g["edges"]:
        assert "fact:crypto:extra" not in (e["subject"] + e["object"])
    # независимое ребро decision→bug цело; входящие bug упали ровно на 1
    # (part-of от extra), ребро decision осталось
    with store.env.begin(readonly=True) as txn:
        inc_bug_after = store._incoming_count(txn, bug_id)
    assert inc_bug_after == 1
    g2 = store.graph("decision:testing:split-monolith", depth=1)
    assert any(e["object"] == "bug:crypto:alignment" for e in g2["edges"])
