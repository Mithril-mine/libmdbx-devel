"""Тесты структурного слоя (refactoring-map): symbols/call_edges/groups."""

import json

from mcp import Store
from tests.conftest import seed_vocab


def test_map_put_and_read_symbol(store):
    body = {"kind": "function", "name": "mdbx_env_open",
            "module": "api-env", "l0": 1, "l1": 10}
    store.map_put_symbol("fn:api-env:mdbx_env_open", body)
    got = store.map_symbol("fn:api-env:mdbx_env_open")
    assert got["name"] == "mdbx_env_open"
    assert got["module"] == "api-env"
    assert store.map_symbol("missing") == {}


def test_map_symbols_prefix(store):
    for k in ("fn:gc:defrag_cycle", "fn:gc:defrag_destroy",
              "type:gc:gc_state", "macro:gc:MDBX_GC"):
        store.map_put_symbol(k, {"kind": "x", "name": k.rsplit(":", 1)[-1]})
    fns = store.map_symbols(prefix="fn:gc:")
    assert len(fns) == 2
    assert "fn:gc:defrag_cycle" in fns


def test_map_edges_dupsort(store):
    store.map_put_edge("fn:a:caller", "fn:b:callee")
    store.map_put_edge("fn:a:caller", "fn:c:other")
    # повторная вставка не должна плодить дубликаты (put_nodupe)
    store.map_put_edge("fn:a:caller", "fn:b:callee")
    edges = [json.loads(e) for e in store.map_edges_of("fn:a:caller")]
    assert sorted(e["callee"] for e in edges) == ["fn:b:callee", "fn:c:other"]
    assert store.map_edges_of("fn:zz:absent") == []


def test_map_edges_kind_semantic(store):
    store.map_put_edge("fn:a:f1", "fn:b:f2", kind="semantic")
    edge = json.loads(store.map_edges_of("fn:a:f1")[0])
    assert edge["kind"] == "semantic"
    assert edge["callee"] == "fn:b:f2"


def test_map_groups_dupsort(store):
    store.map_put_group("group:subsystem:gc", "fn:gc:defrag_cycle")
    store.map_put_group("group:subsystem:gc", "fn:gc:defrag_destroy")
    store.map_put_group("group:subsystem:gc", "fn:gc:defrag_destroy")  # дубль
    store.map_put_group("group:subsystem:env", "fn:api-env:mdbx_env_open")
    members = store.map_groups_of("group:subsystem:gc")
    assert len(members) == 2
    allpairs = store.map_group_members(prefix="group:subsystem:gc")
    assert len(allpairs) == 2


def test_map_load_batch(store):
    symbols = {
        "fn:a:foo": {"kind": "function", "name": "foo", "module": "a"},
        "type:a:bar": {"kind": "type", "name": "bar", "module": "a"},
    }
    edges = [{"caller": "fn:a:foo", "callee": "fn:a:bar",
              "kind": "syntax", "resolved": True}]
    groups = {"group:subsystem:a": {"fn:a:foo", "type:a:bar"}}
    loaded = store.map_load_batch(symbols, edges, groups)
    assert loaded == {"symbols": 2, "edges": 1, "groups": 2}
    assert store.map_symbols() == ["fn:a:foo", "type:a:bar"]
    edge = json.loads(store.map_edges_of("fn:a:foo")[0])
    assert edge == {"kind": "syntax", "resolved": True, "ambiguous": False,
                    "callee": "fn:a:bar"}
    assert store.map_groups_of("group:subsystem:a") == ["fn:a:foo",
                                                        "type:a:bar"]
    # обратное ребро создано
    assert store.map_callers_of("fn:a:bar") == ["fn:a:foo"]


def test_map_load_batch_replace(store):
    """replace=True очищает перегенерируемые таблицы (самоизлечение)."""
    store.map_load_batch(
        {"fn:a:foo": {"kind": "function", "name": "foo", "module": "a"}},
        [{"caller": "fn:a:foo", "callee": "fn:x", "kind": "syntax",
          "resolved": False}], {}, replace=True)
    assert len(store.map_symbols()) == 1
    # повторная загрузка с меньшим набором удаляет исчезнувшие символы
    store.map_load_batch({}, [], {}, replace=True)
    assert store.map_symbols() == []
    assert store.map_edges_of("fn:a:foo") == []


def test_map_load_batch_replace_dupsort(store):
    """replace очищает DUPSORT-таблицы (call_edges/groups) без сбоя."""
    edges = [{"caller": "fn:c:%d" % i, "callee": "fn:t:%d" % (i + j),
              "kind": "syntax", "resolved": True}
             for i in range(200) for j in range(3)]
    store.map_load_batch({}, edges, {}, replace=True)
    assert store.map_edges_of("fn:c:0")
    store.map_load_batch({}, [], {}, replace=True)
    assert store.map_edges_of("fn:c:0") == []
    assert len(store.map_edges_of("fn:c:199")) == 0


def test_map_load_regions_replace(store):
    """replace очищает regions/uncovered (DB_DEFAULTS) перед загрузкой."""
    regions = [{"id": "region:a:%d" % i, "file": "src/a.c", "kind": "if",
                "cond": "#if X", "l0": i, "l1": i + 1} for i in range(5)]
    uncovered = [{"file": "src/a.c", "l0": i, "l1": i + 1} for i in range(3)]
    store.map_load_regions(regions, uncovered, replace=True)
    assert len(store.map_regions(limit=10 ** 9)) == 5
    assert len(store.map_uncovered(limit=10 ** 9)) == 3
    store.map_load_regions([], [], replace=True)
    assert store.map_regions(limit=10 ** 9) == []
    assert store.map_uncovered(limit=10 ** 9) == []


def test_map_load_batch_replace_preserves_curated(store):
    """replace трогает только структурные таблицы, не курируемый слой."""
    seed_vocab(store, [("meta", "test")])
    r = store.safe_store("fact:meta:test", "fact", "слой изолирован", 0.5)
    store.map_load_batch({"fn:a:foo": {"kind": "function", "name": "foo",
                                       "module": "a"}}, [], {}, replace=True)
    assert store.exists(r["key"])
    # и записи, и связи целы
    assert store.map_symbols() == ["fn:a:foo"]


def test_map_load_batch_preserves_curated(store):
    """Батч загрузки структурного слоя не должен трогать курируемый слой."""
    seed_vocab(store, [("meta", "test")])
    r = store.safe_store("fact:meta:test", "fact", "слой изолирован", 0.5)
    store.map_load_batch({"fn:a:foo": {"name": "foo", "module": "a"}}, [], {})
    assert store.exists(r["key"])


def test_map_symbol_json_roundtrip(store):
    body = {"kind": "function", "name": "mdbx_env_open",
            "signature": "int (MDBX_env *, const char *)",
            "file": "src/api-env.c", "module": "api-env",
            "l0": 414, "l1": 524,
            "blocks": [{"id": "B1", "l0": 414, "l1": 524, "kind": "compoundstmt"}]}
    store.map_put_symbol("fn:api-env:mdbx_env_open", body)
    got = store.map_symbol("fn:api-env:mdbx_env_open")
    assert got == body  # полный round-trip без потерь


def test_sym_id_stable_across_replace(store):
    """Числовые id символов стабильны: replace чистит symbols, но не sym_ids."""
    store.map_put_symbol("fn:a:foo", {"kind": "function", "name": "foo",
                                      "module": "a"})
    first = store.sym_id("fn:a:foo")
    second = store.sym_id("fn:a:foo")
    assert first == second
    # повторная загрузка карты (replace) не должна менять id
    store.map_load_batch({"fn:a:foo": {"kind": "function", "name": "foo",
                                       "module": "a"}}, [], {}, replace=True)
    assert store.sym_id("fn:a:foo") == first
    # новый символ получает новый id
    other = store.sym_id("fn:b:bar")
    assert other != first


def test_sym_id_roundtrip_key(store):
    a = store.sym_id("fn:x:alpha")
    b = store.sym_id("fn:y:beta")
    assert a != b
    assert store.sym_key(a) == "fn:x:alpha"
    assert store.sym_key(b) == "fn:y:beta"
    assert store.sym_key(99) == ""


def test_sym_and_record_ids_do_not_collide(store):
    """Числовые id символов и записей не пересекаются (общий next_id)."""
    seed_vocab(store, [("crypto", "alignment")])
    r = store.safe_store("bug:crypto:alignment", "bug", "AES issue", 0.5)
    sid = store.sym_id("fn:mdbx_env_open")
    assert sid != r["id"]
    assert store.sym_key(r["id"]) == ""
    assert store.sym_key(sid) == "fn:mdbx_env_open"


def test_bridge_link_record_to_symbol(store):
    """Мост curated ↔ structural: link запись → символ через sym_id."""
    seed_vocab(store, [("crypto", "alignment")])
    r = store.safe_store("bug:crypto:alignment", "bug",
                         "Segfault in AES on arm64", 0.7)
    store.map_put_symbol("fn:mdbx_env_open", {"kind": "function",
                                              "name": "mdbx_env_open"})
    res = store.link("bug:crypto:alignment", "related-to", "fn:mdbx_env_open")
    assert res["result"] == "linked"
    # повторный линк — exists, не дублируется
    assert store.link("bug:crypto:alignment", "related-to",
                      "fn:mdbx_env_open")["result"] == "exists"
    # несуществующий символ — ошибка
    try:
        store.link("bug:crypto:alignment", "related-to", "fn:no_such")
        assert False, "должен был упасть"
    except Exception:
        pass


def test_bridge_link_still_works_for_records(store):
    """Обычный link запись→запись не сломан расширением."""
    seed_vocab(store, [("crypto", "alignment"), ("platform", "android-abi")])
    a = store.safe_store("bug:crypto:alignment", "bug", "AES issue", 0.5)
    b = store.safe_store("fact:platform:android-abi", "fact", "arm64 abi", 0.8)
    res = store.link(a["key"], "related-to", b["key"])
    assert res["result"] == "linked"


def test_canary_put_get(store):
    store.map_canary_put(x=0x52464D4D, y=7, z=3)
    g = store.map_canary_get()
    assert g["x"] == 0x52464D4D
    assert g["y"] == 7
    assert g["z"] == 3
    assert g["v"] > 0  # номер транзакции


def test_map_refresh_stale_removes_dangling_symbol_links(store):
    """refresh_stale удаляет ссылки на исчезнувшие символы."""
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "AES issue", 0.5)
    store.map_put_symbol("fn:gone", {"kind": "function", "name": "gone"})
    store.link("bug:crypto:alignment", "related-to", "fn:gone")
    # регенерация карты: fn:gone исчез, остался fn:alive
    store.map_load_batch({"fn:alive": {"kind": "function", "name": "alive"}},
                         [], {}, replace=True)
    # fn:alive получит id; fn:gone больше нет в symbols
    store.sym_id("fn:alive")
    res = store.map_refresh_stale()
    # связь к fn:gone удалена
    g = store.graph("bug:crypto:alignment", depth=1)
    outs = [rel.get("object") for rel in g.get("edges", [])]
    assert "fn:gone" not in str(outs)


def test_map_alias_set_and_target(store):
    """map_alias фиксирует переименование, target разрешает цепочки."""
    assert store.map_alias("fn:old", "fn:new")["result"] == "aliased"
    assert store.map_alias("fn:old", "fn:new")["result"] == "exists"
    assert store.map_alias_target("fn:old") == "fn:new"
    assert store.map_alias_target("fn:missing") == ""
    # цепочка old → mid → new
    store.map_alias("fn:mid", "fn:new")
    assert store.map_alias_target("fn:mid") == "fn:new"


def test_map_refresh_stale_redirects_via_alias(store):
    """Связь на исчезнувший символ с алиасом перенаправляется, не удаляется."""
    seed_vocab(store, [("crypto", "alignment")])
    store.safe_store("bug:crypto:alignment", "bug", "AES issue", 0.5)
    store.map_put_symbol("fn:old_impl", {"kind": "function", "name": "old_impl"})
    store.link("bug:crypto:alignment", "related-to", "fn:old_impl")
    # переименование: old_impl → cpp::impl
    store.map_alias("fn:old_impl", "fn:cpp::impl")
    # регенерация: старый символ исчез, новый появился
    store.map_load_batch({"fn:cpp::impl": {"kind": "function",
                                           "name": "cpp::impl"}},
                         [], {}, replace=True)
    res = store.map_refresh_stale()
    assert res["redirected_links"] >= 1
    g = store.graph("bug:crypto:alignment", depth=1)
    outs = [rel.get("object") for rel in g.get("edges", [])]
    assert "fn:cpp::impl" in str(outs)
    assert "fn:old_impl" not in str(outs)