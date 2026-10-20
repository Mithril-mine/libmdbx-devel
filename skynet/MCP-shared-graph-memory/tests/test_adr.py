"""Тесты ADR (архитектурные решения)."""


def test_adr_create_get(store):
    r = store.adr_update("refactor-map-phases", {
        "title": "Refactor-map phases",
        "status": "accepted",
        "date": "2026-10-19",
        "context": "Structural layer needed for C++ port.",
        "decision": "Adopt refactoring-map with implementations.",
        "consequences": "Multi-config scan required.",
    })
    assert r["result"] == "created"
    adr = store.adr_get("refactor-map-phases")
    assert adr["status"] == "accepted"
    assert adr["decision"].startswith("Adopt")


def test_adr_update_merges(store):
    store.adr_update("a1", {"context": "ctx", "decision": "dec"})
    r = store.adr_update("a1", {"decision": "dec2", "sections": {"ADR": "x"}})
    assert r["result"] == "merged"
    adr = store.adr_get("a1")
    assert adr["context"] == "ctx"      # старое сохранено
    assert adr["decision"] == "dec2"    # новое применено
    assert adr["sections"] == {"ADR": "x"}


def test_adr_outline_prefix(store):
    store.adr_update("schema-coverage", {"title": "Coverage schema"})
    store.adr_update("lifecycle-stale", {"title": "Stale policy"})
    store.adr_update("other-thing", {"title": "Other"})
    out = store.adr_outline(prefix="lifecycle")
    assert [a["name"] for a in out] == ["lifecycle-stale"]
    assert out[0]["title"] == "Stale policy"
    all_ = store.adr_outline()
    assert len(all_) == 3


def test_adr_delete(store):
    store.adr_update("tmp", {"decision": "x"})
    assert store.adr_get("tmp") != {}
    store.adr_delete("tmp")
    assert store.adr_get("tmp") == {}
    assert store.adr_delete("tmp")["result"] == "notfound"
