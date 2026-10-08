"""Тесты записей: safe_store / merge / conflict / exists."""

import pytest

from mcp.errors import MemoryError
from tests.conftest import seed_crypto, seed_vocab


def test_safe_store_created(store):
    seed_vocab(store, [("crypto", "alignment")])
    r = store.safe_store("bug:crypto:alignment-arm64", "bug",
                         "Segfault on arm64 misaligned NEON.", 0.7)
    assert r["result"] == "created"
    assert r["key"] == "bug:crypto:alignment"
    assert r["id"] == 1
    body = store.get_record("bug:crypto:alignment")
    assert body["type"] == "bug"
    assert body["importance"] == 0.7
    assert body["_id"] == 1
    assert "segfault" in body["_terms"]


def test_keys_all_and_prefix(store):
    """keys() отдаёт весь корпус (нет cap в 10, как у recall)."""
    import random
    topics = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
              "golf", "hotel", "india", "juliet", "kilo", "lima"]
    seed_vocab(store, [("platform", t) for t in topics])
    rnd = random.Random(12345)
    pool = ["apple", "berry", "cherry", "date", "elder", "fig", "grape"]
    for t in topics:
        summary = " ".join(rnd.choices(pool, k=8))
        store.safe_store("fact:platform:%s" % t, "fact", summary, 0.5)
    all_keys = store.keys("")
    assert len(all_keys) >= 12  # recall вернул бы только 10
    assert all(k.startswith("fact:platform:") for k in all_keys)
    prefix_keys = store.keys("fact:platform:")
    assert prefix_keys and all(k.startswith("fact:platform:") for k in prefix_keys)
    limited = store.keys("", limit=3)
    assert len(limited) == 3


def test_safe_store_merged(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "First version of the bug.", 0.7)
    r = store.safe_store("bug:crypto:alignment", "bug", "Second version fixed.", 0.5)
    assert r["result"] == "merged"
    assert r["id"] == 1  # id сохраняется
    body = store.get_record("bug:crypto:alignment")
    assert body["summary"] == "Second version fixed."
    assert body["importance"] == 0.5


def test_safe_store_history_written(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "Old text here.", 0.7)
    store.safe_store("bug:crypto:alignment", "bug", "New text.", 0.6)
    with store.env.begin(readonly=True) as txn:
        rc, val = txn.get(store.dbi(txn, "history"), b"_history:bug:crypto:alignment:")
        # префикс не точный; вместо этого ищем через курсор
        found = None
        with txn.cursor(store.dbi(txn, "history")) as cur:
            rc, k, v = cur.get(0)  # FIRST
            while rc == 0:
                if k.startswith(b"_history:bug:crypto:alignment"):
                    found = v
                    break
                rc, k, v = cur.get(8)  # NEXT
        assert found is not None
        assert b"Old text here." in found


def test_safe_store_conflict_on_duplicate(store):
    seed_vocab(store, [("crypto", "alignment"), ("crypto", "align")])
    text = "Segfault AES-CBC arm64 buffer misaligned NEON align fix."
    r1 = store.safe_store("bug:crypto:alignment", "bug", text, 0.7)
    assert r1["result"] == "created"
    r2 = store.safe_store("bug:crypto:align", "bug", text + ".", 0.7)
    assert r2["result"] == "conflict"
    assert r2["key"] == "bug:crypto:alignment"


def test_safe_store_importance_clamp(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "text", 1.7)
    store.safe_store("bug:crypto:alignment", "bug", "text", -0.3)
    body = store.get_record("bug:crypto:alignment")
    assert 0.0 <= body["importance"] <= 1.0


def test_safe_store_empty_summary_invalid(store):
    seed_vocab(store, [("crypto", "alignment")])
    with pytest.raises(MemoryError) as ei:
        store.safe_store("bug:crypto:alignment", "bug", "   ", 0.5)
    assert "empty" in str(ei.value) or "пуста" in str(ei.value)


def test_safe_store_bad_type(store):
    seed_vocab(store, [("crypto", "alignment")])
    with pytest.raises(MemoryError):
        store.safe_store("epic:crypto:alignment", "epic", "text", 0.5)


def test_safe_store_unknown_key(store):
    with pytest.raises(MemoryError):
        store.safe_store("bug:nosuchmod:alignment", "bug", "text", 0.5)


def test_exists(store):
    seed_crypto(store)
    assert store.exists("bug:crypto:alignment") is True
    assert store.exists("bug:crypto:no-such") is False


def test_get_record_none(store):
    assert store.get_record("bug:crypto:alignment") is None


def test_next_id_increments(store):
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android")])
    r1 = store.safe_store("bug:crypto:alignment", "bug", "text one", 0.5)
    r2 = store.safe_store("fact:platform:android", "fact", "text two", 0.5)
    assert r1["id"] == 1 and r2["id"] == 2