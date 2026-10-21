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
