"""Тесты реестра probes (SystemMap)."""


def test_probe_add_get_dedup(store):
    r = store.probe_add("txn_write_started", kind="usdt", mechanism="txn",
                        symbol="mdbx::txn::write_started", file="src/txn-basal.c",
                        line=120)
    assert r["result"] == "created"
    # повторная регистрация того же тега — merge (dedup по тегу)
    r2 = store.probe_add("txn_write_started", kind="usdt", mechanism="txn",
                         file="src/txn-basal.c", line=121)
    assert r2["result"] == "merged"
    p = store.probe_get("txn_write_started")
    assert p["tag"] == "txn_write_started"
    assert p["line"] == 121  # обновлён


def test_probe_batch_and_list(store):
    store.probe_batch([
        {"tag": "cache_check_txn_err", "kind": "injected",
         "mechanism": "cache", "file": "src/api-get-cached.c", "line": 61},
        {"tag": "rkl_extend_solid_gap", "kind": "watch",
         "mechanism": "gc", "file": "src/rkl.c", "line": 216},
        {"tag": "dev_assert_ok", "kind": "dev_assert",
         "mechanism": "core", "file": "src/mdbx.c", "line": 10},
    ])
    lst = store.probe_list("")
    assert len(lst) == 3
    injected = store.probe_list(kind="injected")
    assert [p["tag"] for p in injected] == ["cache_check_txn_err"]
    # фильтр по kind dev_assert
    da = store.probe_list(kind="dev_assert")
    assert [p["tag"] for p in da] == ["dev_assert_ok"]


def test_probe_status_and_remove(store):
    store.probe_add("gc_alloc_source", kind="usdt", mechanism="gc")
    r = store.probe_status("gc_alloc_source", "validated")
    assert r["result"] == "merged"
    assert store.probe_get("gc_alloc_source")["status"] == "validated"
    store.probe_remove("gc_alloc_source")
    assert store.probe_get("gc_alloc_source") == {}
    assert store.probe_list("gc_alloc") == []


def test_probe_batch_merge_keeps_old_fields(store):
    store.probe_add("fault_dup", kind="injected", mechanism="gc", test_theme="gc-put")
    store.probe_batch([{"tag": "fault_dup", "kind": "injected", "file": "src/x.c"}])
    p = store.probe_get("fault_dup")
    assert p["mechanism"] == "gc"          # старое сохранено
    assert p["test_theme"] == "gc-put"     # старое сохранено
    assert p["file"] == "src/x.c"          # новое применено


def test_probe_layer_isolation(store):
    """Зонды feature-слоя изолированы от базового (C-5)."""
    store.probe_add("txn_start", kind="usdt", layer="0.15.base")
    store.probe_add("txn_start_v2", kind="usdt", layer="0.15.feat.2")
    base = store.probe_list(layer="0.15.base")
    feat = store.probe_list(layer="0.15.feat.2")
    assert {p["tag"] for p in base} == {"txn_start"}
    assert {p["tag"] for p in feat} == {"txn_start_v2"}
    # get/remove — в своём слое
    assert store.probe_get("txn_start", layer="0.15.feat.2") == {}
    assert store.probe_get("txn_start", layer="0.15.base")["kind"] == "usdt"
    store.probe_remove("txn_start_v2", layer="0.15.feat.2")
    assert store.probe_list(layer="0.15.feat.2") == []
    assert store.probe_list(layer="0.15.base")


def test_probe_kind_status_whitelist(store):
    """kind/status вне whitelist отвергаются (C-8)."""
    import pytest
    from mcp.errors import MemoryError as ME
    store.probe_add("ok_tag", kind="usdt", status="candidate")
    with pytest.raises(ME):
        store.probe_add("bad_kind", kind="magic-probe")
    with pytest.raises(ME):
        store.probe_add("bad_status", status="done-maybe")
    with pytest.raises(ME):
        store.probe_status("ok_tag", "archived")


def test_probe_batch_skips_invalid(store):
    """probe_batch: бракованные записи пропускаются с отчётом (A5-β)."""
    r = store.probe_batch([
        {"tag": "good_one", "kind": "watch"},
        {"tag": "", "kind": "watch"},
        {"tag": "bad_kind", "kind": "nope"},
        {"tag": "bad_status", "status": "weird"},
        {"tag": "good_two", "status": "validated"},
    ], layer="0.15.base")
    assert r["probes"] == 2
    assert len(r["skipped"]) == 3
    reasons = {x["reason"] for x in r["skipped"]}
    assert "empty-tag" in reasons
    assert any(x.startswith("bad-kind") for x in reasons)
    assert any(x.startswith("bad-status") for x in reasons)
    assert store.probe_get("good_one", layer="0.15.base")["kind"] == "watch"
    assert store.probe_get("bad_kind", layer="0.15.base") == {}
