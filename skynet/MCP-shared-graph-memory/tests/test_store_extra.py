"""Дополнительное покрытие edge-кейсов Store и утилит."""

import pytest

from mcp import Store
from mcp.errors import MemoryError
from mcp.store import ACCESS_LEN, _pack_u64, pack_access, unpack_access
from tests.conftest import seed_vocab


def test_size_limit_long_key(store):
    seed_vocab(store, [("crypto", "alignment")])
    # страница движка следует за страницей ОС (4K linux/win, 16K на arm64-mac),
    # поэтому берём ключ гарантированно больше любого лимита (даже 64K-страницы)
    k = b"k" * (1 << 20)
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
    from mcp.store import unpack_access
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
    from mcp import libmdbx as mdbx
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
    from mcp import libmdbx as mdbx
    from mcp.errors import MemoryError as ME

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
    # второй вызов в пределах 60с не должен попасть в буфер касаний
    store._bump_access("bug:crypto:alignment", force=False)
    store._flush_touches()  # одна write-txn на все касания
    with store.env.begin(readonly=True) as txn:
        _, packed = txn.get(store.dbi(txn, "access"), _pack_u64(1))
        _, _, cnt = unpack_access(packed)
    assert cnt == 1


def test_bump_batched_single_txn(store, monkeypatch):
    """Касания нескольких записей сбрасываются одной write-txn, а не N."""
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android")])
    store.safe_store("bug:crypto:alignment", "bug", "bug one", 0.5)
    store.safe_store("fact:platform:android", "fact", "fact one", 0.5)
    store._lru_last.clear()
    writes = []

    import mcp.store as store_mod
    orig = store_mod.Store._begin_write

    def counting(self):
        writes.append(1)
        return orig(self)

    monkeypatch.setattr(store_mod.Store, "_begin_write", counting)
    store._bump_access("bug:crypto:alignment", force=True)
    store._bump_access("fact:platform:android", force=True)
    store._flush_touches()
    assert len(writes) == 1  # одна транзакция на оба касания
    with store.env.begin(readonly=True) as txn:
        _, p1 = txn.get(store.dbi(txn, "access"), _pack_u64(1))
        _, p2 = txn.get(store.dbi(txn, "access"), _pack_u64(2))
    assert unpack_access(p1)[2] == 1
    assert unpack_access(p2)[2] == 1


def test_recall_flushes_one_txn(store, monkeypatch):
    """recall(limit=5) делает одну write-txn на LRU, а не по одной на запись."""
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android"),
                       ("platform", "android-abi"), ("build", "release")])
    store.safe_store("bug:crypto:alignment", "bug", "bug alpha", 0.7)
    store.safe_store("fact:platform:android", "fact", "fact beta", 0.9)
    store.safe_store("proc:build:release", "proc", "proc gamma", 0.6)
    store._lru_last.clear()
    writes = []

    import mcp.store as store_mod
    orig = store_mod.Store._begin_write

    def counting(self):
        writes.append(1)
        return orig(self)

    monkeypatch.setattr(store_mod.Store, "_begin_write", counting)
    store.recall("fact:*")
    assert len(writes) == 1


def test_sync_poll_thread_lifecycle(store, monkeypatch):
    """Фоновый тред шлёт sync_poll раз в секунду и останавливается на close."""
    calls = []
    real_sync = store.env.sync

    def spy_sync(force=False, nonblock=True):
        calls.append((force, nonblock))
        return real_sync(force=force, nonblock=nonblock)

    monkeypatch.setattr(store.env, "sync", spy_sync)
    assert store._sync_thread.is_alive()

    import time as _time
    _time.sleep(1.4)
    assert len(calls) >= 1
    assert all(force is False and nonblock is True for force, nonblock in calls)

    store.close()
    assert store._closed is True
    assert not store._sync_thread.is_alive()


def test_remove_topic_normalizer():
    from mcp.normalize import Normalizer
    n = Normalizer()
    n.add_topic("alignment")
    assert n.remove_topic("alignment") is True
    assert n.remove_topic("nope") is False


def test_begin_write_non_busy_error(store, monkeypatch):
    from mcp import libmdbx as mdbx
    from mcp.errors import MemoryError as ME

    def fake(readonly=False, parent=None):
        raise mdbx.LibmdbxError(mdbx.RC_MAP_FULL, "t")

    monkeypatch.setattr(store.env, "begin", fake)
    with pytest.raises(mdbx.LibmdbxError):
        store._begin_write()


def test_bump_access_missing_record(store):
    store._bump_access("fact:platform:nothing")  # не должно бросать


def test_flush_touches_suppresses_busy(store, monkeypatch):
    from mcp import libmdbx as mdbx
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
    store._lru_last.clear()
    store._bump_access("bug:crypto:alignment", force=True)

    def fake(readonly=False, parent=None):
        raise mdbx.LibmdbxError(mdbx.RC_BUSY, "t")

    monkeypatch.setattr(store.env, "begin", fake)
    store._flush_touches()  # сбой флаша не критичен — MemoryError подавляется


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


def test_search_flushes_one_txn(store, monkeypatch):
    """search() с несколькими результатами делает одну write-txn на LRU."""
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android"),
                       ("platform", "android-abi")])
    store.safe_store("bug:crypto:alignment", "bug", "buffer overflow crash", 0.7)
    store.safe_store("fact:platform:android", "fact", "android overflow fix note", 0.9)
    store._lru_last.clear()
    writes = []

    import mcp.store as store_mod
    orig = store_mod.Store._begin_write

    def counting(self):
        writes.append(1)
        return orig(self)

    monkeypatch.setattr(store_mod.Store, "_begin_write", counting)
    recs = store.search("overflow", limit=10)
    assert len(recs) == 2
    assert len(writes) == 1


def test_close_final_sync(store, monkeypatch):
    """close() делает финальный force-sync (force=True, nonblock=False)."""
    calls = []
    real_sync = store.env.sync

    def spy_sync(force=False, nonblock=True):
        calls.append((force, nonblock))
        return real_sync(force=force, nonblock=nonblock)

    monkeypatch.setattr(store.env, "sync", spy_sync)
    store.close()
    assert (True, False) in calls


def test_close_flushes_pending_touches(store_path):
    """Отложенные касания сбрасываются при close (переживают переоткрытие)."""
    s = Store(store_path)
    try:
        seed_vocab(s, [("crypto", "alignment")])
        s.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
        s._lru_last.clear()
        s._bump_access("bug:crypto:alignment", force=True)  # в буфер, без flush
        assert s._touch_ids  # касание отложено
    finally:
        s.close()
    s2 = Store(store_path)
    try:
        with s2.env.begin(readonly=True) as txn:
            _, packed = txn.get(s2.dbi(txn, "access"), _pack_u64(1))
            assert packed is not None
            assert unpack_access(packed)[2] == 1
    finally:
        s2.close()


def test_recall_all_rate_limited_zero_txn(store, monkeypatch):
    """Если все касания уже в 60s-фильтре, recall не пишет вообще."""
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
    store.recall("bug:*")  # прогреваем фильтр (касание + флаш)
    writes = []

    import mcp.store as store_mod
    orig = store_mod.Store._begin_write

    def counting(self):
        writes.append(1)
        return orig(self)

    monkeypatch.setattr(store_mod.Store, "_begin_write", counting)
    store.recall("bug:*")  # в пределах 60с — касание проигнорировано
    assert len(writes) == 0


def test_double_close_idempotent(store):
    store.close()
    store.close()  # повторный вызов не должен падать


def test_touch_single_txn(store, monkeypatch):
    """touch() пишет сразу одной транзакцией."""
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "some bug", 0.5)
    writes = []

    import mcp.store as store_mod
    orig = store_mod.Store._begin_write

    def counting(self):
        writes.append(1)
        return orig(self)

    monkeypatch.setattr(store_mod.Store, "_begin_write", counting)
    store.touch("bug:crypto:alignment")
    assert len(writes) == 1