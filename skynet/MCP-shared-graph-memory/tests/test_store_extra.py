"""Дополнительное покрытие edge-кейсов Store и утилит."""

import pytest

from mcp_memory import Store
from mcp_memory.errors import MemoryError
from mcp_memory.store import ACCESS_LEN, _pack_u64, pack_access, unpack_access
from tests.conftest import seed_vocab


def test_size_limit_long_key(store):
    seed_vocab(store, [("crypto", "alignment")])
    k = b"k" * 5000
    v = b"v"
    with pytest.raises(MemoryError):
        store._check_size(k, v)


def test_size_limit_long_value(store):
    k = b"key"
    v = b"v" * (17 << 20)  # > дефолтного лимита 16MB
    with pytest.raises(MemoryError):
        store._check_size(k, v)


def test_check_size_ok(store):
    store._check_size(b"key", b"value")  # не должно бросать


def test_pack_unpack_access_roundtrip():
    packed = pack_access(0.75, 1234567890, 42)
    assert len(packed) == ACCESS_LEN
    score, ts, cnt = unpack_access(packed)
    assert abs(score - 0.75) < 1e-9
    assert ts == 1234567890
    assert cnt == 42


def test_unpack_access_bad_length():
    from mcp_memory.store import unpack_access
    with pytest.raises(AssertionError):
        unpack_access(b"short")


def test_find_conflict_none(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "totally different text", 0.5)
    with store.env.begin(readonly=True) as txn:
        assert store._find_conflict(txn, "completely unrelated words here") is None


def test_search_empty_tokens(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
    assert store.search("!! ???") == []


def test_lookup_no_words(store):
    seed_vocab(store, [("crypto", "alignment")])
    assert store.lookup("") == []


def test_graph_depth_clamp(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
    g = store.graph("bug:crypto:alignment", depth=99)
    assert set(g["nodes"]) == {"bug:crypto:alignment"}


def test_recall_limit_clamp_high(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
    rows = store.recall("bug:", limit=100)
    assert len(rows) == 1


def test_vocab_add_empty_module(store):
    with pytest.raises(MemoryError):
        store.vocab_add("   ")


def test_stats_empty(store_path):
    s = Store(store_path)
    try:
        st = s.stats()
        assert st["records"] == 0
        assert st["avg_importance"] == 0.0
    finally:
        s.close()


def test_begin_write_busy_retry(store, monkeypatch):
    from mcp_memory import libmdbx as mdbx
    seed_vocab(store, [("crypto", "alignment")])
    calls = []
    orig = store.env.begin

    def fake(readonly=False, parent=None):
        calls.append(readonly)
        if len(calls) < 3:
            raise mdbx.LibmdbxError(mdbx.RC_BUSY, "t")
        return orig(readonly=readonly, parent=parent)

    monkeypatch.setattr(store.env, "begin", fake)
    txn = store._begin_write()
    assert len(calls) >= 3
    txn.abort()


def test_begin_write_busy_exhaust(store, monkeypatch):
    from mcp_memory import libmdbx as mdbx
    from mcp_memory.errors import MemoryError as ME

    def fake(readonly=False, parent=None):
        raise mdbx.LibmdbxError(mdbx.RC_BUSY, "t")

    monkeypatch.setattr(store.env, "begin", fake)
    with pytest.raises(ME):
        store._begin_write()


def test_importance_non_numeric(store):
    seed_vocab(store, [("crypto", "alignment")])
    with pytest.raises(MemoryError):
        store.safe_store("bug:crypto:alignment", "bug", "text", "abc")


def test_lookup_limit_break(store):
    seed_vocab(store, [("crypto", "alignment"), ("crypto", "align")])
    store.safe_store("bug:crypto:alignment", "bug", "shared common token", 0.5)
    store.safe_store("bug:crypto:align", "bug", "shared common token too", 0.5)
    keys = store.lookup("shared", limit=1)
    assert len(keys) == 1


def test_unlink_missing_record(store):
    seed_vocab(store, [("platform", "android"), ("platform", "android-abi")])
    store.safe_store("fact:platform:android-abi", "fact", "exists", 0.5)
    r = store.unlink("fact:platform:android", "related-to",
                     "fact:platform:android-abi")
    assert r["result"] == "notfound"


def test_graph_cycle_visited(store):
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android"),
                       ("platform", "android-abi")])
    store.safe_store("bug:crypto:alignment", "bug", "a bug", 0.5)
    store.safe_store("fact:platform:android-abi", "fact", "a fact", 0.5)
    store.link("bug:crypto:alignment", "related-to", "fact:platform:android-abi")
    store.link("fact:platform:android-abi", "related-to", "bug:crypto:alignment")
    g = store.graph("bug:crypto:alignment", depth=3)
    assert set(g["nodes"]) == {"bug:crypto:alignment", "fact:platform:android-abi"}


def test_gc_context_snapshot_hot(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "a bug", 0.01)
    store.dump_context(task="T")
    res = store.gc(dry_run=True)
    assert res["hot"] >= 1  # context-snapshot всегда hot


def test_gc_hot_by_access_and_importance(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "very important bug", 1.0)
    for _ in range(10):
        store.touch("bug:crypto:alignment")
    res = store.gc(dry_run=True)
    assert res["hot"] == 1


def test_touch_missing(store):
    with pytest.raises(MemoryError):
        store.touch("bug:crypto:no-such")


def test_bump_rate_limited(store):
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "a bug", 0.5)
    store._bump_access("bug:crypto:alignment", force=True)
    # второй вызов в пределах 60с не должен увеличить счётчик
    store._bump_access("bug:crypto:alignment", force=False)
    with store.env.begin(readonly=True) as txn:
        _, packed = txn.get(store.dbi(txn, "access"), _pack_u64(1))
        _, _, cnt = unpack_access(packed)
    assert cnt == 1


def test_remove_topic_normalizer():
    from mcp_memory.normalize import Normalizer
    n = Normalizer()
    n.add_topic("alignment")
    assert n.remove_topic("alignment") is True
    assert n.remove_topic("nope") is False


def test_begin_write_non_busy_error(store, monkeypatch):
    from mcp_memory import libmdbx as mdbx
    from mcp_memory.errors import MemoryError as ME

    def fake(readonly=False, parent=None):
        raise mdbx.LibmdbxError(mdbx.RC_MAP_FULL, "t")

    monkeypatch.setattr(store.env, "begin", fake)
    with pytest.raises(mdbx.LibmdbxError):
        store._begin_write()


def test_bump_access_missing_record(store):
    store._bump_access("fact:platform:nothing")  # не должно бросать


def test_bump_access_suppresses_busy(store, monkeypatch):
    from mcp_memory import libmdbx as mdbx
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)

    def fake(readonly=False, parent=None):
        raise mdbx.LibmdbxError(mdbx.RC_BUSY, "t")

    monkeypatch.setattr(store.env, "begin", fake)
    store._bump_access("bug:crypto:alignment")  # MemoryError подавляется


def test_store_context_manager(store_path):
    with Store(store_path) as s:
        seed_vocab(s, [("crypto", "alignment")])
        s.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
        assert s.exists("bug:crypto:alignment")
    # после выхода из контекста повторное открытие возможно
    s2 = Store(store_path)
    try:
        assert s2.exists("bug:crypto:alignment")
    finally:
        s2.close()