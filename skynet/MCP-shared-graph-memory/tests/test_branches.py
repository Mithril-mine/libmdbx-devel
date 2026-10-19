"""Тесты нелинейного версионирования БЗ: слои веток и структурный diff."""


def test_branch_register_and_prefix(store):
    r = store.branch_register("0.15", "base", 0,
                              revision="v0.15.0-152-gabc")
    assert r["layer"] == "0.15"
    assert store.layer_prefix("0.15") == ""
    r2 = store.branch_register("0.15", "gc-pilot-2", 17,
                               revision="abc123", base_branch="master")
    assert r2["layer"] == "0.15.gc-pilot-2.17"
    assert store.layer_prefix(r2["layer"]) == "0.15.gc-pilot-2.17/"
    lst = store.branch_list()
    assert {b["layer"] for b in lst} == {"0.15", "0.15.gc-pilot-2.17"}
    assert store.branch_list(base_only=True)[0]["layer"] == "0.15"


def test_branch_activate_current(store):
    store.branch_register("0.15", "base")
    store.branch_activate("0.15")
    assert store.branch_current()["layer"] == "0.15"
    r = store.branch_activate("0.15.gc")
    assert r["previous"] == "0.15"
    assert store.branch_current()["layer"] == "0.15.gc"


def test_branch_remove(store):
    r = store.branch_register("0.14", "stable")
    assert store.branch_remove(r["layer"])["result"] == "removed"
    assert store.branch_remove(r["layer"])["result"] == "notfound"


def test_map_load_layers_isolated(store):
    """Feature-слой не затирает базовый (prefix в ключах)."""
    store.map_load_batch({"fn:m1": {"module": "gc", "l0": 1, "l1": 5}},
                         [], {}, replace=True, layer="0.15.base")
    store.map_load_batch({"fn:m1": {"module": "gc", "l0": 1, "l1": 9},
                          "fn:m2": {"module": "gc", "l0": 2, "l1": 3}},
                         [], {}, replace=True, layer="0.15.gc.3")
    base = store.map_symbols("fn:", layer="0.15.base")
    feat = store.map_symbols("fn:", layer="0.15.gc.3")
    assert base == ["fn:m1"]
    assert feat == ["fn:m1", "fn:m2"]
    # базовый слой не видит чужие feature-ключи
    assert store.map_symbol("fn:m2", layer="0.15.base") == {}


def test_map_diff_branches_content(store):
    store.map_load_batch({"fn:foo": {"module": "gc", "l0": 1, "l1": 5,
                                     "kind": "function",
                                     "blocks": [{"id": "B1"}]}},
                         [], {}, replace=True, layer="0.15.base")
    store.map_load_batch({"fn:foo": {"module": "gc", "l0": 1, "l1": 9,
                                     "kind": "function",
                                     "blocks": [{"id": "B1"}, {"id": "B2"}]},
                          "fn:new": {"module": "gc", "l0": 20, "l1": 30,
                                     "kind": "function",
                                     "blocks": [{"id": "B1"}]}},
                         [], {}, replace=True, layer="0.15.gc.7")
    d = store.map_diff_branches("0.15.base", "0.15.gc.7")
    c = d["counts"]
    assert c["added"] == 1 and c["changed"] == 1 and c["removed"] == 0
    assert c["added_functions"] == 1
    assert c["added_blocks"] == 1       # fn:new блок B1
    assert list(d["added"].keys()) == ["gc"]
    assert list(d["changed"].keys()) == ["gc"]

def test_effective_layer_active_branch_defaults(store):
    """После branch_activate слой используется инструментами по умолчанию."""
    store.branch_register("0.15", "base")
    store.branch_register("0.15", "feat", 3)
    store.map_load_batch({"fn:base_only": {"module": "m", "l0": 1, "l1": 2}},
                         [], {}, replace=True, layer="0.15.base")
    store.map_load_batch({"fn:feat_only": {"module": "m", "l0": 1, "l1": 2}},
                         [], {}, replace=True, layer="0.15.feat.3")
    # до активации виден базовый слой
    assert "fn:base_only" in store.map_symbols("fn:", layer="")
    assert "fn:feat_only" not in store.map_symbols("fn:", layer="")
    # активируем feature-слой — дефолтные чтения переключаются
    store.branch_activate("0.15.feat.3")
    syms = store.map_symbols("fn:")
    assert "fn:feat_only" in syms
    assert "fn:base_only" not in syms
    # явный layer перебивает активную ветку
    assert "fn:base_only" in store.map_symbols("fn:", layer="0.15.base")
