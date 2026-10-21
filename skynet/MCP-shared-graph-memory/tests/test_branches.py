"""Тесты нелинейного версионирования БЗ: слои веток и структурный diff."""

from mcp import Store


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


def test_branch_purge_removes_layer_data(store):
    """C1/N5: purge чистит все таблицы слоя; базовый и соседний слои целы."""
    import pytest
    from mcp.errors import MemoryError as ME
    L = "0.15.purge-me.7"
    body = {"module": "m", "l0": 1, "l1": 2, "kind": "function",
            "blocks": [{"id": "B1"}]}
    # feature-слой: map + coverage + блоки + probe + трассы + реестр
    store.map_load_batch(
        {"fn:m:a": body, "fn:m:b": body},
        [{"caller": "fn:m:a", "callee": "fn:m:b"}],
        {"grp:m:all": ["fn:m:a"]}, replace=True, layer=L)
    store.coverage_insert("ut/t1", ["fn:m:a"], pct=10.0, layer=L)
    store.coverage_blocks_insert(
        "ut/t1", [{"symbol": "fn:m:a", "block_id": "B1", "covered": True}],
        layer=L)
    store.probe_add("purge_tag", kind="watch", symbol="fn:m:a", layer=L)
    store.trace_ingest("fn:m:a", "fn:m:b", count=2, layer=L)
    store.branch_register("0.15", "purge-me", n_commits=7)
    # базовый и соседний feature-слой — не должны пострадать
    store.map_put_symbol("fn:base:x", body, layer="0.15.base")
    store.coverage_insert("ut/tb", ["fn:base:x"], pct=50.0, layer="0.15.base")
    store.map_put_symbol("fn:nb:y", body, layer="0.15.other.3")
    # P3: числовые id символов слоя и базового — до purge
    sid_feat = store.sym_id(L + "/fn:m:a")
    sid_base = store.sym_id("fn:base:x")
    assert sid_feat and sid_base and sid_feat != sid_base

    r = store.branch_purge(L)
    assert r["result"] == "purged"
    # все семейства слоя пусты
    assert store.map_symbols(layer=L) == []
    assert store.map_edges_of("fn:m:a", layer=L) == []
    assert store.map_group_members("grp:m:all", layer=L) == []
    assert store.coverage_all(layer=L) == []
    assert store.coverage_symbols("fn:m:a", layer=L) == []
    assert store.probe_get("purge_tag", layer=L) == {}
    assert store.trace_observed("fn:m:a", layer=L) == []
    # call_traces_rev (DUPSORT, обратные рёбра) тоже вычищен
    assert r["removed"].get("call_traces_rev", 0) >= 1
    # P3: sym_ids/sym_id2key слоя вычищены, базовые id целы
    assert r["removed"].get("sym_ids", 0) >= 1
    assert store.sym_key(sid_feat) == ""
    assert store.sym_key(sid_base) == "fn:base:x"
    # реестр очищен
    assert L not in [b["layer"] for b in store.branch_list()]
    # базовый слой и сосед целы
    assert "fn:base:x" in store.map_symbols(layer="0.15.base")
    assert [c["test"] for c in store.coverage_all(layer="0.15.base")] == \
        ["ut/tb"]
    assert "fn:nb:y" in store.map_symbols(layer="0.15.other.3")
    # базовый purge запрещён
    with pytest.raises(ME):
        store.branch_purge("0.15.base")
    with pytest.raises(ME):
        store.branch_purge("")  # пустой → базовый
    # несуществующий слой → notfound
    assert store.branch_purge("0.15.nope.9")["result"] == "notfound"


def test_branch_purge_dupsort_all_values(store):
    """C1: DUPSORT-таблицы чистятся по всем значениям ключа."""
    L = "0.15.dup.5"
    store.map_load_batch(
        {"fn:m:hub": {}, "fn:m:leaf1": {}, "fn:m:leaf2": {}},
        [{"caller": "fn:m:hub", "callee": "fn:m:leaf1"},
         {"caller": "fn:m:hub", "callee": "fn:m:leaf2"},
         {"caller": "fn:m:leaf1", "callee": "fn:m:leaf2"}],
        {}, replace=True, layer=L)
    assert len(store.map_edges_of("fn:m:hub", layer=L)) == 2
    r = store.branch_purge(L)
    assert r["result"] == "purged"
    # 3 fwd-пары (hub→leaf1, hub→leaf2, leaf1→leaf2) + 3 rev-пары
    assert r["removed"]["call_edges"] == 3
    assert r["removed"]["call_edges_rev"] == 3
    assert store.map_edges_of("fn:m:hub", layer=L) == []


def test_branch_purge_prefix_collision(store):
    """C1: purge слоя "…feat.1" НЕ задевает слой "…feat.10".

    Защита — терминатор "/" в префиксе: "0.15.feat.1/" не является
    префиксом "0.15.feat.10/…". Регрессионный тест на случай рефакторинга
    layer_prefix/purge-скана (startswith без "/" затронул бы оба слоя).
    """
    body = {"module": "m", "kind": "function"}
    store.map_put_symbol("fn:m:one", body, layer="0.15.feat.1")
    store.map_put_symbol("fn:m:ten", body, layer="0.15.feat.10")
    store.coverage_insert("ut/c1", ["fn:m:one"], pct=10.0,
                          layer="0.15.feat.1")
    store.coverage_insert("ut/c10", ["fn:m:ten"], pct=20.0,
                          layer="0.15.feat.10")
    store.trace_ingest("fn:m:one", "fn:m:ten", layer="0.15.feat.1")
    store.trace_ingest("fn:m:ten", "fn:m:one", layer="0.15.feat.10")
    # семейства, которые collision-тест раньше не покрывал:
    store.map_load_batch({}, [{"caller": "fn:m:ten", "callee": "fn:m:one"}],
                         {"grp:m:g": ["fn:m:ten"]}, layer="0.15.feat.10")
    store.coverage_blocks_insert(
        "ut/c10",
        [{"symbol": "fn:m:ten", "block_id": "B1", "covered": True}],
        layer="0.15.feat.10")
    store.probe_add("ten_tag", kind="watch", symbol="fn:m:ten",
                    layer="0.15.feat.10")
    sid_ten = store.sym_id("0.15.feat.10/fn:m:ten")

    r = store.branch_purge("0.15.feat.1")
    assert r["result"] == "purged"
    # слой .1 пуст
    assert store.map_symbols(layer="0.15.feat.1") == []
    assert store.coverage_all(layer="0.15.feat.1") == []
    assert store.trace_observed("fn:m:one", layer="0.15.feat.1") == []
    # слой .10 ЦЕЛ во всех семействах (test_id в coverage_all с префиксом)
    assert "fn:m:ten" in store.map_symbols(layer="0.15.feat.10")
    assert [c["test"] for c in
            store.coverage_all(layer="0.15.feat.10")] == \
        ["0.15.feat.10/ut/c10"]
    assert store.trace_observed("fn:m:ten", layer="0.15.feat.10")
    assert store.map_edges_of("fn:m:ten", layer="0.15.feat.10")
    assert store.map_group_members("grp:m:g", layer="0.15.feat.10") == \
        [("grp:m:g", "fn:m:ten")]
    assert [b["block_id"] for b in
            store.coverage_blocks_get("ut/c10", "fn:m:ten",
                                      layer="0.15.feat.10")] == ["B1"]
    assert store.probe_get("ten_tag", layer="0.15.feat.10")
    assert store.sym_key(sid_ten) == "0.15.feat.10/fn:m:ten"


def test_branch_purge_registry_without_data(store):
    """C1: слой с реестром, но без данных → purged (не notfound), реестр
    удалён. Граничная ветка `removed={} and had_reg=True`."""
    r = store.branch_register("0.15", "empty", n_commits=4)
    assert r["layer"] == "0.15.empty.4"
    res = store.branch_purge("0.15.empty.4")
    assert res["result"] == "purged"
    assert res["removed"] == {}
    assert "0.15.empty.4" not in \
        [b["layer"] for b in store.branch_list()]
    # повтор — уже notfound
    assert store.branch_purge("0.15.empty.4")["result"] == "notfound"


def test_branch_purge_active_layer_blocked(store):
    """Ревью: purge активного слоя запрещён (иначе branch_current — мёртвый
    указатель, и все чтения с layer="" молча пусты)."""
    import pytest
    from mcp.errors import MemoryError as ME
    L = "0.15.active.8"
    store.branch_register("0.15", "active", n_commits=8)
    store.branch_activate(L)
    store.map_put_symbol("fn:m:act", {"module": "m"}, layer=L)
    # активный слой purge запрещён
    with pytest.raises(ME) as ei:
        store.branch_purge(L)
    assert "активным" in str(ei.value)
    # данные и активация целы
    assert store.branch_current()["layer"] == L
    assert "fn:m:act" in store.map_symbols(layer=L)
    # после переключения на другой слой — purge проходит
    store.branch_activate("0.15")
    r = store.branch_purge(L)
    assert r["result"] == "purged"
    assert store.map_symbols(layer=L) == []


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


def test_map_diff_branches_same_layer_is_empty(store):
    """Регрессия B2: diff слоя с самим собой (pre_a == pre_b) — пустой.

    Блокер ревью #155: безусловный break в коллекторе относил ключ только
    к первому префиксу — to-сторона оставалась пустой и всё отмечалось
    removed. Здесь from == to: ключ совпадает с ОБОИМИ префиксами.
    """
    body = {"module": "m", "l0": 1, "l1": 2, "kind": "function",
            "blocks": [{"id": "B1"}]}
    store.map_load_batch({"fn:x": body, "fn:y": body}, [], {},
                         replace=True, layer="0.15.feat.1")
    d = store.map_diff_branches("0.15.feat.1", "0.15.feat.1")
    c = d["counts"]
    assert c["added"] == 0 and c["removed"] == 0 and c["changed"] == 0
    assert d["total_from"] == 2 and d["total_to"] == 2
    # и для базового слоя с самим собой
    store.map_load_batch({"fn:b": body}, [], {}, replace=True,
                         layer="0.15.base")
    d2 = store.map_diff_branches("0.15.base", "0.15.base")
    assert d2["counts"]["removed"] == 0 and d2["total_to"] == 1


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


def test_env_layer_overrides_active_branch(store_path, monkeypatch):
    """SHARED_GRAPH_MEMORY_LAYER изолирует процесс от чужой активации (N1)."""
    monkeypatch.setenv("SHARED_GRAPH_MEMORY_LAYER", "0.15.mine.1")
    from mcp import Store
    s = Store(store_path)
    try:
        s.map_load_batch({"fn:mine": {"module": "m", "l0": 1, "l1": 2}},
                         [], {}, replace=True, layer="0.15.mine.1")
        s.map_load_batch({"fn:theirs": {"module": "m", "l0": 1, "l1": 2}},
                         [], {}, replace=True, layer="0.15.theirs.2")
        # другой агент активировал свою ветку — глобальный default переключён
        s.branch_activate("0.15.theirs.2")
        # мой процесс читает СВОЙ слой (env-override), а не активный чужой
        syms = s.map_symbols("fn:")
        assert "fn:mine" in syms
        assert "fn:theirs" not in syms
        # явный layer= побеждает env-override
        assert "fn:theirs" in s.map_symbols("fn:", layer="0.15.theirs.2")
    finally:
        s.close()


def test_branch_activate_expected_prev_cas(store):
    """CAS: активация с неверным expected_prev отклоняется конфликтом (N1)."""
    import pytest
    from mcp.errors import MemoryError as ME
    store.branch_register("0.15", "base")
    store.branch_activate("0.15")
    assert store.branch_current()["layer"] == "0.15"
    # гонка: другой агент уже переключил слой — CAS ловит расхождение
    with pytest.raises(ME) as ei:
        store.branch_activate("0.15.feat.1", expected_prev="0.15.stale.9")
    assert "CONFLICT" in str(ei.value)
    assert store.branch_current()["layer"] == "0.15"  # не переключился
    # корректный expected_prev — активация проходит
    r = store.branch_activate("0.15.feat.1", expected_prev="0.15")
    assert r["previous"] == "0.15" and r["current"] == "0.15.feat.1"


def test_upgrade_recreates_incompatible_table(tmp_path):
    """Миграция: таблица со старыми флагами пересоздаётся при rw-upgrade."""
    from mcp import libmdbx as mdbx
    db = str(tmp_path / "old.mdbx")
    # «старая» БД: call_traces была DUPSORT, остальные базовые таблицы
    env = mdbx.Env(db, maxdbs=64, readonly=False, sync_mode="safe_nosync")
    with env.begin() as txn:
        txn.open_dbi("meta", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("records", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("call_traces", mdbx.CREATE | mdbx.DUPSORT)
    env.close()
    # rw-открытие не должно падать на INCOMPATIBLE (tolerant), upgrade проходит
    s = Store(db, readonly=False)
    r = s.upgrade()
    assert r["created"] == [] or "call_traces" in r["created"]
    s.close()
    # readonly после миграции работает
    s2 = Store(db, readonly=True)
    assert s2.trace_observed() == []
    s2.close()


def test_readonly_old_db_without_new_tables(tmp_path):
    """Старая БД (без новых таблиц) открывается readonly без падений."""
    from mcp import libmdbx as mdbx
    db = str(tmp_path / "min.mdbx")
    env = mdbx.Env(db, maxdbs=64, readonly=False, sync_mode="safe_nosync")
    with env.begin() as txn:
        txn.open_dbi("meta", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("records", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("links", mdbx.CREATE | mdbx.DUPSORT)
        txn.open_dbi("vocab", mdbx.CREATE | mdbx.DB_DEFAULTS)
    env.close()
    s = Store(db, readonly=True)
    assert s.branch_list() == []
    assert s.probe_list() == []
    assert s.coverage_symbols("fn:") == []
    assert s.adr_outline() == []
    assert s.ci_jobs_all() == []
    assert s.test_metrics_all() == []
    assert s.cbm_linked() == []
    s.close()


def _make_old_schema_db(path, table="call_traces", flags=None, rows=None):
    """Создаёт БД «старой схемы»: таблица с иными флагами (+опц. данные)."""
    from mcp import libmdbx as mdbx
    env = mdbx.Env(path, maxdbs=64, readonly=False, sync_mode="safe_nosync")
    with env.begin() as txn:
        txn.open_dbi("meta", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("records", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("vocab", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("ids", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("id2key", mdbx.CREATE | mdbx.INTEGERKEY)
        txn.open_dbi("inverted",
                     mdbx.CREATE | mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP)
        txn.open_dbi("links",
                     mdbx.CREATE | mdbx.DUPSORT | mdbx.DUPFIXED | mdbx.INTEGERDUP)
        txn.open_dbi("simhash", mdbx.CREATE | mdbx.INTEGERKEY)
        txn.open_dbi("access", mdbx.CREATE | mdbx.INTEGERKEY)
        txn.open_dbi("archive", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("history", mdbx.CREATE | mdbx.DB_DEFAULTS)
        txn.open_dbi("meta", mdbx.CREATE | mdbx.DB_DEFAULTS)
        d = txn.open_dbi(table, mdbx.CREATE | (flags if flags is not None
                                               else mdbx.DUPSORT))
        if rows:
            for k, v in rows:
                txn.put(d, k.encode(), v.encode())
    env.close()


def test_upgrade_empty_incompatible_passes_and_works(tmp_path):
    """Пустая несовместимая таблица: upgrade проходит, traces работают (C-2).

    libmdbx не проверяет флаги для пустой таблицы — INCOMPATIBLE возникает
    только при fetch с данными, поэтому recreation не требуется; достаточно,
    чтобы upgrade создал недостающие таблицы и БД осталась рабочей.
    """
    db = str(tmp_path / "old.mdbx")
    _make_old_schema_db(db)  # call_traces = DUPSORT, пусто
    s = Store(db, readonly=False)
    s.upgrade()  # не должно бросать: пустая несовместимая таблица открывается
    s.trace_ingest("fn:a", "fn:b")
    assert [t["callee"] for t in s.trace_observed("fn:a")] == ["fn:b"]
    s.close()
    # БД осталась рабочей в readonly
    s2 = Store(db, readonly=True)
    assert len(s2.trace_observed("fn:")) == 1
    s2.close()


def test_upgrade_rejects_nonempty_incompatible_table(tmp_path):
    """НЕпустая несовместимая таблица → schema-mismatch, данные целы (C-2)."""
    import pytest
    from mcp import libmdbx as mdbx
    from mcp.errors import MemoryError as ME
    db = str(tmp_path / "old2.mdbx")
    _make_old_schema_db(db, rows=[("legacy_row", '{"x":1}')])
    s = Store(db, readonly=False)
    with pytest.raises(ME) as ei:
        s.upgrade()
    assert "INVALID" in str(ei.value)
    assert "schema-mismatch" in str(ei.value)
    s.close()
    # данные целы (старый handle, старый формат)
    env = mdbx.Env(db, maxdbs=64, readonly=True)
    with env.begin(readonly=True) as txn:
        d = txn.open_dbi("call_traces", mdbx.DUPSORT)
        rc, v = txn.get(d, b"legacy_row")
        assert v is not None and b"x" in v
    env.close()


def test_coverage_migrate_isolates_source(store):
    """C3/N5: миграция покрытия merge-ветки — копия, источник цел."""
    L = "0.15.merge-me.9"
    store.map_put_symbol("fn:m:a", {"module": "m"}, layer=L)
    store.map_put_symbol("fn:m:b", {"module": "m"}, layer=L)
    store.coverage_insert("ut/t1", ["fn:m:a"], pct=10.0, layer=L)
    store.coverage_blocks_insert(
        "ut/t1", [{"symbol": "fn:m:a", "block_id": "B1", "covered": True}],
        layer=L)
    store.trace_ingest("fn:m:a", "fn:m:b", count=2, layer=L)

    r = store.coverage_migrate(L, "0.15.base")
    assert r["tests"] == 1 and r["blocks"] == 1 and r["traces"] == 1

    # целевой слой получил данные
    c = store.coverage_get("ut/t1", layer="0.15.base")
    assert c["symbols"] == ["fn:m:a"] and c["pct"] == 10.0
    assert [b["block_id"] for b in
            store.coverage_blocks_get("ut/t1", "fn:m:a",
                                      layer="0.15.base")] == ["B1"]
    assert store.trace_observed("fn:m:a", layer="0.15.base")[0]["count"] == 2
    # covered_by в целевом слое согласован
    cb = store.coverage_symbols("fn:m:a", layer="0.15.base")
    assert any(x["symbol"] == "fn:m:a" and x["tests"] == ["ut/t1"]
               for x in cb)

    # источник НЕ изменился
    c2 = store.coverage_get("ut/t1", layer=L)
    assert c2["symbols"] == ["fn:m:a"]
    assert store.trace_observed("fn:m:a", layer=L)[0]["count"] == 2


def test_coverage_migrate_symbol_filter_and_conflicts(store):
    """C3: фильтр symbols; конфликт test_id в целевом слое — перезапись."""
    import pytest
    from mcp.errors import MemoryError as ME
    L = "0.15.merge2.3"
    store.coverage_insert("ut/t1", ["fn:m:a", "fn:m:b"], pct=50.0, layer=L)
    store.coverage_insert("ut/t2", ["fn:m:b"], pct=100.0, layer=L)
    # целевой слой уже имеет ut/t1 со старыми данными — миграция перезапишет
    store.coverage_insert("ut/t1", ["fn:old:sym"], pct=1.0, layer="0.15.base")

    r = store.coverage_migrate(L, "0.15.base", symbols=["fn:m:b"])
    # оба теста содержат fn:m:b → перенесены оба
    assert r["tests"] == 2
    # ut/t1 в базовом перезаписан: старые символы ушли, пришёл fn:m:b
    c = store.coverage_get("ut/t1", layer="0.15.base")
    assert c["symbols"] == ["fn:m:b"] and c["pct"] == 50.0
    # stale-членство fn:old:sym вычищено (_coverage_put_locked)
    assert store.coverage_symbols("fn:old", layer="0.15.base") == []
    # новый член covered_by появился
    cb = store.coverage_symbols("fn:m:b", layer="0.15.base")
    assert cb and sorted(cb[0]["tests"]) == ["ut/t1", "ut/t2"]

    # совпадающие префиксы ("0.15" и "0.15.base" оба → "") — invalid
    with pytest.raises(ME):
        store.coverage_migrate("0.15", "0.15.base")
    with pytest.raises(ME):
        store.coverage_migrate(L, L)


def test_coverage_migrate_blocks_colon_in_test_id(store):
    """C3/P1: test_id с ":" не мисатрибутируется при миграции блоков.

    Ключ "coverage_blocks:{pre}{test}:{sym}" неоднозначен, когда и test_id,
    и sym содержат ":" (ut/api:case + fn:m:a). Раньше split(":",1) резал по
    ПЕРВОМУ ":" → блок уезжал под чужой test_id "ut/api". Теперь sym берётся
    из тела JSON, test_id — хвост ключа без суффикса ":"+sym.
    """
    L = "0.15.colon.4"
    store.coverage_blocks_insert(
        "ut/api:case",
        [{"symbol": "fn:m:a", "block_id": "B1", "covered": True}], layer=L)
    r = store.coverage_migrate(L, "0.15.base")
    assert r["blocks"] == 1
    # блок под ПРАВИЛЬНЫМ test_id (не под мисатрибутированным "ut/api")
    got = store.coverage_blocks_get("ut/api:case", "fn:m:a", layer="0.15.base")
    assert [b["block_id"] for b in got] == ["B1"]
    # старый баг обрезал test_id до первого ":" → ключ ".../ut/api:fn:m:a"
    # (без ":case"); его быть не должно
    assert store.coverage_blocks_get("ut/api", "fn:m:a",
                                     layer="0.15.base") == []


def test_coverage_migrate_blocks_symbol_filter_colon(store):
    """C3/P1: фильтр symbols работает и при test_id с ":" (sym из тела)."""
    L = "0.15.colon2.5"
    store.coverage_blocks_insert(
        "ut/api:case",
        [{"symbol": "fn:m:a", "block_id": "B1", "covered": True},
         {"symbol": "fn:m:b", "block_id": "B9", "covered": True}], layer=L)
    # переносим только fn:m:a
    store.coverage_migrate(L, "0.15.base", symbols=["fn:m:a"])
    got_a = store.coverage_blocks_get("ut/api:case", "fn:m:a",
                                      layer="0.15.base")
    assert [b["block_id"] for b in got_a] == ["B1"]
    # fn:m:b не перенесён (фильтр)
    assert store.coverage_blocks_get("ut/api:case", "fn:m:b",
                                     layer="0.15.base") == []


def _rev_callers(store, callee, layer=""):
    """Прямое чтение call_traces_rev (внутренний индекс, читателя нет)."""
    from mcp import libmdbx as _mdbx
    pre = store.layer_prefix(layer)
    with store.env.begin(readonly=True) as txn:
        dbi = store.dbi(txn, "call_traces_rev")
        out = []
        with txn.cursor(dbi) as cur:
            rc, k, v = cur.get(_mdbx.CURSOR_SET_RANGE, (pre + callee).encode())
            while rc == _mdbx.RC_SUCCESS and k == (pre + callee).encode():
                out.append(v.decode()[len(pre):])
                rc, k, v = cur.get(_mdbx.CURSOR_NEXT)
    return out


def test_coverage_migrate_traces_rev_no_dangling(store):
    """C3/P2: перезапись caller в целевом слое чистит висячие rev-рёбра.

    Прямой индекс call_traces[caller] перезаписывается телом источника;
    обратный call_traces_rev для callee, исчезнувших из тела, должен быть
    удалён — иначе rev расходится с прямым (висячие callee→caller).
    """
    L = "0.15.trmig.6"
    # base: caller fn:m:a -> fn:old:callee
    store.trace_ingest("fn:m:a", "fn:old:callee", layer="0.15.base")
    # feature: тот же caller fn:m:a -> fn:new:callee
    store.trace_ingest("fn:m:a", "fn:new:callee", layer=L)
    store.coverage_migrate(L, "0.15.base")
    # прямой индекс согласован: только new_callee
    obs = store.trace_observed("fn:m:a", layer="0.15.base")
    assert [o["callee"] for o in obs] == ["fn:new:callee"]
    # обратный: old_callee больше НЕ указывает на fn:m:a (перезатёрт)
    assert _rev_callers(store, "fn:old:callee", "0.15.base") == []
    # new_callee указывает корректно
    assert _rev_callers(store, "fn:new:callee", "0.15.base") == ["fn:m:a"]


def test_coverage_migrate_skips_corrupt_records(store):
    """Ревью: битые/несогласованные записи пропускаются с отчётом skipped.

    Миграция не падает на мусоре источника и не порождает мусорных ключей:
    corrupt-JSON, не-dict блоки и несовпадение symbol-тела с sym-ключом
    — все три случая попадают в отчёт "skipped" (паттерн probe_batch),
    остальные записи переносятся нормально (включая legacy без symbol).
    """
    L = "0.15.corrupt.7"
    with store._begin_write() as txn:
        dbi = store.dbi(txn, "coverage_blocks")
        # мусорный JSON-элемент (не dict)
        txn.put(dbi, ("coverage_blocks:%s/ut/c1:fn:m:a" % L).encode(),
                b"[1]")
        # несогласованная: тело symbol=fn:OTHER, ключ sym=fn:m:a
        txn.put(dbi, ("coverage_blocks:%s/ut/c2:fn:m:a" % L).encode(),
                b'[{"symbol": "fn:OTHER", "block_id": "B9", "covered": true}]')
        # legacy без symbol в теле (пустой sym → ключ оканчивается ":")
        txn.put(dbi, ("coverage_blocks:%s/ut/c3:" % L).encode(),
                b'[{"block_id": "B4", "covered": false}]')
        # нормальная запись
        txn.put(dbi, ("coverage_blocks:%s/ut/c4:fn:m:b" % L).encode(),
                b'[{"symbol": "fn:m:b", "block_id": "B1", "covered": true}]')
        cov = store.dbi(txn, "coverage")
        txn.put(cov, ("coverage:%s/ut/c5" % L).encode(), b"{oops")
        txn.put(cov, ("coverage:%s/ut/c6" % L).encode(),
                b'{"symbols": ["fn:m:b"], "pct": 1.0}')

    r = store.coverage_migrate(L, "0.15.base")
    reasons = sorted(s["reason"] for s in r["skipped"])
    assert reasons == ["corrupt-blocks", "corrupt-coverage", "sym-mismatch"]
    # хорошие записи перенесены
    assert r["tests"] == 1 and r["blocks"] == 2
    assert store.coverage_get("ut/c6", layer="0.15.base")["symbols"] == \
        ["fn:m:b"]
    assert [b["block_id"] for b in
            store.coverage_blocks_get("ut/c4", "fn:m:b",
                                      layer="0.15.base")] == ["B1"]
    assert [b["block_id"] for b in
            store.coverage_blocks_get("ut/c3", "",
                                      layer="0.15.base")] == ["B4"]
    # мусор НЕ перенесён (и не склеен в чужие ключи)
    assert store.coverage_blocks_get("ut/c1", "fn:m:a", layer="0.15.base") \
        == []
    assert store.coverage_blocks_get("ut/c2", "fn:m:a", layer="0.15.base") \
        == []
    assert store.coverage_blocks_get("ut/c2", "fn:OTHER", layer="0.15.base") \
        == []


def test_merge_flow_migrate_then_purge(store):
    """Merge-цепочка BRANCHES.md end-to-end: migrate → purge.

    coverage_migrate(feature, base) переносит прогон, branch_purge(feature)
    сносит мёртвый слой; целевой слой данные сохранил, источник пуст.
    Ловит регрессии, где purge затирает мигрированные данные (пересечение
    префиксов coverage/blocks в чистке).
    """
    L = "0.15.flow.11"
    body = {"module": "m", "kind": "function"}
    store.map_put_symbol("fn:m:new", body, layer=L)
    store.coverage_insert("ut/f1", ["fn:m:new"], pct=100.0, layer=L)
    store.coverage_blocks_insert(
        "ut/f1",
        [{"symbol": "fn:m:new", "block_id": "B1", "covered": True}], layer=L)
    store.trace_ingest("fn:m:new", "fn:m:base", layer=L)
    store.branch_register("0.15", "flow", n_commits=11)

    m = store.coverage_migrate(L, "0.15.base")
    assert m["tests"] == 1 and m["blocks"] == 1 and m["traces"] == 1
    p = store.branch_purge(L)
    assert p["result"] == "purged"
    # источник пуст
    assert store.map_symbols(layer=L) == []
    assert store.coverage_all(layer=L) == []
    # мигрированные данные в base ЦЕЛЫ после purge источника
    c = store.coverage_get("ut/f1", layer="0.15.base")
    assert c["symbols"] == ["fn:m:new"] and c["pct"] == 100.0
    assert [b["block_id"] for b in
            store.coverage_blocks_get("ut/f1", "fn:m:new",
                                      layer="0.15.base")] == ["B1"]
    assert store.trace_observed("fn:m:new", layer="0.15.base")
    cb = store.coverage_symbols("fn:m:new", layer="0.15.base")
    assert cb and cb[0]["tests"] == ["ut/f1"]
