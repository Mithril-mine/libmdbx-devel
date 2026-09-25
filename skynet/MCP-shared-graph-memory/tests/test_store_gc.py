"""Тесты gc / purge / архивирования / контекста."""

import pytest

from mcp_memory.errors import MemoryError
from tests.conftest import seed_vocab


def setup(s):
    seed_vocab(s, [("crypto", "alignment"), ("platform", "android"),
                   ("platform", "android-abi")])
    # важные записи (warm/hot)
    s.safe_store("bug:crypto:alignment", "bug", "Important bug text.", 0.9)
    s.safe_store("fact:platform:android-abi", "fact", "Android fact.", 0.9)
    return s


def test_gc_dry_run_tiers(store):
    setup(store)
    res = store.gc(dry_run=True)
    assert res["dry_run"] is True
    assert res["hot"] + res["warm"] + res["cold"] == 2
    assert res["cold_keys"] == []


def test_gc_archive_moves_cold(store):
    setup(store)
    # создаём явно холодную запись (низкая важность, без связей)
    store.safe_store("fact:platform:android", "fact", "trivial", 0.01)
    res = store.gc(dry_run=False, archive=True)
    assert res["archived"] >= 1
    # запись переехала в archive и удалена из records
    assert store.exists("fact:platform:android") is False
    with store.env.begin(readonly=True) as txn:
        rc, val = txn.get(store.dbi(txn, "archive"), b"fact:platform:android")
        assert val is not None


def test_gc_dry_run_without_archive_keeps(store):
    setup(store)
    store.safe_store("fact:platform:android", "fact", "trivial", 0.01)
    res = store.gc(dry_run=False, archive=False)
    assert "archived" not in res
    assert store.exists("fact:platform:android") is True


def test_purge(store):
    setup(store)
    res = store.purge(["bug:crypto:alignment"])
    assert res["removed"] == ["bug:crypto:alignment"]
    assert store.exists("bug:crypto:alignment") is False
    # индексы тоже очищены
    assert store.lookup("important") == []
    assert store.lookup("bug") == []


def test_purge_missing(store):
    setup(store)
    res = store.purge(["fact:platform:android"])
    assert res["missing"] == ["fact:platform:android"]


def test_purge_removes_links(store):
    setup(store)
    store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    store.purge(["bug:crypto:alignment"])
    g = store.graph("fact:platform:android-abi", depth=1)
    assert "bug:crypto:alignment" not in g["nodes"]
    assert len(g["edges"]) == 0


def test_dump_and_restore_context(store):
    setup(store)
    r = store.dump_context(task="Fix bug", milestone="M1",
                           keys=["bug:crypto:alignment"],
                           hypotheses=["align to 16"])
    assert r["key"] == "proc:memory:context-snapshot"
    rows = store.restore_context(r["context_id"])
    assert rows
    assert rows[0]["key"] == "proc:memory:context-snapshot"


def test_stats(store):
    setup(store)
    st = store.stats()
    assert st["records"] == 2
    assert st["by_type"]["bug"] == 1
    assert st["avg_importance"] > 0.5
    assert st["next_id"] == 3
    assert st["vocab_modules"] >= 10