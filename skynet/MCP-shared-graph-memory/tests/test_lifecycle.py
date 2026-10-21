"""Тесты жизненного цикла карты: stale-пометка и поколения/diff."""


def _mk_sym(kind="function"):
    return {"kind": kind, "name": "x", "module": "m", "l0": 1, "l1": 2}


def test_map_mark_stale(store):
    store.map_put_symbol("fn:a:foo", _mk_sym())
    store.map_put_symbol("fn:a:bar", _mk_sym())
    store.map_put_symbol("fn:a:zzz", _mk_sym())
    r = store.map_mark_stale(["fn:a:foo", "fn:a:bar"])
    assert r["marked_stale"] == 1  # zzz стал stale
    assert r["marked_alive"] == 2  # foo, bar стали alive
    assert store.map_symbol("fn:a:zzz")["status"] == "stale"
    assert store.map_symbol("fn:a:foo")["status"] == "alive"
    # идемпотентность: повтор не меняет счётчики
    r2 = store.map_mark_stale(["fn:a:foo", "fn:a:bar"])
    assert r2["marked_stale"] == 0 and r2["marked_alive"] == 0


def test_map_mark_stale_all_stale_when_empty(store):
    store.map_put_symbol("fn:a:only", _mk_sym())
    r = store.map_mark_stale([])
    assert r["marked_stale"] == 1


def test_map_generations_and_snapshot(store):
    store.map_put_symbol("fn:a:foo", _mk_sym())
    gen0 = store.map_generations()
    assert gen0["symbols"] == 1
    snap = store.map_snapshot()
    assert "fn:a:foo" in snap["symbols"]
    assert snap["generation"] == gen0["generation"]


def test_map_diff(store):
    store.map_put_symbol("fn:a:foo", _mk_sym())
    store.map_put_symbol("fn:a:bar", _mk_sym())
    other = {"symbols": ["fn:a:foo"]}  # более ранний снимок
    d = store.map_diff(other=other)
    assert d["added"] == ["fn:a:bar"]
    assert d["removed"] == []
    assert d["common"] == ["fn:a:foo"]
    # удаление: текущая карта не содержит ключа из other
    d2 = store.map_diff(other={"symbols": ["fn:a:foo", "fn:a:gone"]})
    assert d2["removed"] == ["fn:a:gone"]


def test_map_load_batch_bumps_generation(store):
    """replace-загрузка инкрементирует поколение (canary.y)."""
    g0 = store.map_generations()["generation"]
    store.map_load_batch({"fn:core:m1": _mk_sym()}, [], {}, replace=True)
    g1 = store.map_generations()["generation"]
    assert g1 == g0 + 1


def test_map_load_batch_chunked(store, monkeypatch):
    """Чанкование (A2): несколько write-txn, итог и поколение как у монолита."""
    syms = {"fn:m:s%d" % i: _mk_sym() for i in range(5)}
    edges = [{"caller": "fn:m:s0", "callee": "fn:m:s%d" % i}
             for i in (1, 2, 3)]
    groups = {"grp:m:all": ["fn:m:s%d" % i for i in range(5)]}
    orig = store._begin_write
    calls = {"n": 0}

    def spy():
        calls["n"] += 1
        return orig()

    monkeypatch.setattr(store, "_begin_write", spy)
    g0 = store.map_generations()["generation"]
    # chunk=2: 5 sym + 3 edge + 5 grp = 13 элементов → 7 чанков + 1 txn
    # поколения (canary.y в финале, базовый слой)
    r = store.map_load_batch(syms, edges, groups, chunk=2, replace=True)
    monkeypatch.undo()
    assert r == {"symbols": 5, "edges": 3, "groups": 5}
    assert calls["n"] == 8, \
        "ожидались 7 чанков + txn поколения, %d" % calls["n"]
    # итог идентичен монолитной загрузке
    assert sorted(store.map_symbols("fn:")) == sorted(syms)
    assert len(store.map_edges_of("fn:m:s0")) == 3
    assert len(store.map_group_members("grp:m:all")) == 5
    # поколение инкрементировано ровно один раз (не по чанку)
    assert store.map_generations()["generation"] == g0 + 1
    # идемпотентность: повтор той же загрузки — тот же контент, поколение +1
    store.map_load_batch(syms, edges, groups, chunk=2, replace=True)
    assert len(store.map_edges_of("fn:m:s0")) == 3  # нет дублей
    assert store.map_generations()["generation"] == g0 + 2


def test_map_snapshot_single_txn_and_hash(store, monkeypatch):
    """map_snapshot открывает РОВНО одну read-txn; content_hash чувствителен."""
    store.map_put_symbol("fn:a:foo", {"module": "m", "l0": 1, "l1": 5,
                                      "signature": "int foo()", "blocks": []})
    calls = {"n": 0}
    orig_begin = store.env.begin

    def spy(*a, **kw):
        calls["n"] += 1
        return orig_begin(*a, **kw)

    monkeypatch.setattr(store.env, "begin", spy)
    snap = store.map_snapshot()
    assert calls["n"] == 1, "snapshot должен открывать ровно одну txn"
    assert snap["symbols"] == ["fn:a:foo"]
    assert snap["content_hash"]
    monkeypatch.undo()
    # хэш стабилен без изменений
    snap2 = store.map_snapshot()
    assert snap2["content_hash"] == snap["content_hash"]
    # хэш меняется после структурного изменения
    store.map_put_symbol("fn:a:foo", {"module": "m", "l0": 1, "l1": 9,
                                      "signature": "int foo()", "blocks": []})
    snap3 = store.map_snapshot()
    assert snap3["content_hash"] != snap["content_hash"]
    # map_diff: быстрый путь по хэшу
    d = store.map_diff(other=snap3)
    assert d.get("identical") is True
    # map_diff: обычный путь со списком
    d2 = store.map_diff(other={"symbols": []})
    assert d2["added"] == ["fn:a:foo"]


def test_map_port_status(store):
    """Приборная панель C→C++: агрегат cpp_status по модулям."""
    store.map_put_symbol("fn:m:done_fn", {"module": "api-env",
                                          "kind": "function",
                                          "cpp_status": "done",
                                          "cpp_eq": "mdbx::env::open"},
                         layer="0.15.base")
    store.map_put_symbol("fn:m:part_fn", {"module": "api-env",
                                          "kind": "function",
                                          "cpp_status": "partial",
                                          "blocks": [{"id": "B1"}, {"id": "B2"},
                                                     {"id": "B3"}]},
                         layer="0.15.base")
    store.map_put_symbol("fn:m:none_fn", {"module": "gc-get",
                                          "kind": "function"},
                         layer="0.15.base")
    st = store.map_port_status(layer="0.15.base")
    assert st["modules"]["api-env"]["total"] == 2
    assert st["modules"]["api-env"]["done"] == 1
    assert st["modules"]["api-env"]["partial"] == 1
    assert st["total"]["none"] == 1
    assert st["total"]["pct_done"] == 33.3
    # топ непортированных: partial_fn с 3 блоками первый
    assert st["top_not_ported"][0]["symbol"] == "fn:m:part_fn"
    # фильтр по модулю
    st2 = store.map_port_status(module="gc-get", layer="0.15.base")
    assert set(st2["modules"].keys()) == {"gc-get"}


def _spy_read_txns(store, monkeypatch):
    """Счётчик read-txn через env.begin(readonly=True)."""
    orig = store.env.begin
    calls = {"n": 0}

    def spy(*a, **kw):
        if kw.get("readonly") or (a and a[0] is True):
            calls["n"] += 1
        return orig(*a, **kw)

    monkeypatch.setattr(store.env, "begin", spy)
    return calls


def test_dashboards_single_read_txn(store, monkeypatch):
    """B1: дашборды читают карту+coverage РОВНО одной read-txn на вызов."""
    store.map_put_symbol("fn:m:a", {"module": "m", "l0": 1, "l1": 2})
    store.map_put_symbol("fn:m:b", {"module": "m", "l0": 3, "l1": 4})
    store.coverage_insert("ut/t1", ["fn:m:a"], pct=10.0)
    store.coverage_blocks_insert("ut/t1", [
        {"symbol": "fn:m:a", "block_id": "B1", "covered": True}])
    for method, kw in (("map_port_status", {}),
                       ("coverage_summary", {}),
                       ("coverage_gaps", {})):
        calls = _spy_read_txns(store, monkeypatch)
        getattr(store, method)(**kw)
        monkeypatch.undo()
        assert calls["n"] == 1, \
            "%s: ожидалась 1 read-txn, получено %d" % (method, calls["n"])


def test_map_diff_branches_single_read_txn(store, monkeypatch):
    """B1: diff двух слоёв — из одного MVCC-снимка (1 read-txn)."""
    body = {"module": "m", "l0": 1, "l1": 2, "kind": "function",
            "blocks": [{"id": "B1"}]}
    store.map_load_batch({"fn:x": body}, [], {}, replace=True,
                         layer="0.15.base")
    store.map_load_batch({"fn:x": body, "fn:y": body}, [], {}, replace=True,
                         layer="0.15.feat.1")
    calls = _spy_read_txns(store, monkeypatch)
    d = store.map_diff_branches("0.15.base", "0.15.feat.1")
    monkeypatch.undo()
    assert calls["n"] == 1, "diff: ожидалась 1 read-txn, %d" % calls["n"]
    assert d["counts"]["added"] == 1


def test_map_diff_branches_removed_parity(store):
    """B2: hash-based diff — removed отдаёт полные функции/блоки (паритет)."""
    fa = {"module": "m", "l0": 1, "l1": 9, "kind": "function",
          "blocks": [{"id": "B1"}, {"id": "B2"}]}
    fb = {"module": "m", "l0": 1, "l1": 2, "kind": "function",
          "blocks": [{"id": "B1"}]}
    store.map_load_batch({"fn:gone": fa, "fn:stay": fb}, [], {},
                         replace=True, layer="0.15.base")
    store.map_load_batch({"fn:stay": fb}, [], {}, replace=True,
                         layer="0.15.feat.1")
    d = store.map_diff_branches("0.15.base", "0.15.feat.1")
    c = d["counts"]
    assert c["removed"] == 1
    # removed-символ — полное тело: функция и её блоки учитываются
    assert c["removed_functions"] == 1
    assert c["removed_blocks"] == 2
    assert list(d["removed"].keys()) == ["m"]
    assert d["removed"]["m"] == ["fn:gone"]
