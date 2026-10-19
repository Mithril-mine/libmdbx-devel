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