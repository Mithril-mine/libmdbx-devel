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