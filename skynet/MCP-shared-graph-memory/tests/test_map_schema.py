"""Структурный слой карты: regions/uncovered, load_map, обновление словаря.

Покрывает:
  - загрузку и чтение regions (#if-дерево) и uncovered-островов;
  - CLI load_map.py на реальном артефакте (regions+uncovered+symbols+edges);
  - перезагрузку словаря при переоткрытии env (_reopen) после внешних
    vocab_add (stale-vocab bug).
"""

import os
import subprocess
import sys

import pytest

from mcp import Store

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(MODULE_DIR, "tools")


def test_regions_uncovered_load_and_query(tmp_path):
    db = str(tmp_path / "t.mdbx")
    s = Store(db, _skip_sync_thread=True)
    try:
        regions = [
            {"id": "region:api-cold:1", "file": "src/api-cold.c",
             "module": "api-cold", "cond": "IS_WINDOWS", "kind": "if",
             "l0": 176, "l1": 190, "depth": 1, "parent": None},
            {"id": "region:api-cold:2", "file": "src/api-cold.c",
             "module": "api-cold", "cond": "defined(RLIMIT_RSS)", "kind": "ifdef",
             "l0": 191, "l1": 202, "depth": 1, "parent": "region:api-cold:1"},
        ]
        uncovered = [
            {"file": "src/api-copy.c", "l0": 8, "l1": 16,
             "lines": ["MDBX_env *env;"], "region": "region:api-copy:1"},
            {"file": "src/api-copy.c", "l0": 23, "l1": 23, "lines": [],
             "region": None},
        ]
        r = s.map_load_regions(regions, uncovered)
        assert r == {"regions": 2, "uncovered": 2}

        regs = s.map_regions("region:api-cold", limit=10)
        assert len(regs) == 2
        by_key = {x["key"]: x["region"] for x in regs}
        assert by_key["region:api-cold:2"]["parent"] == "region:api-cold:1"
        assert s.map_region("region:api-cold:1")["cond"] == "IS_WINDOWS"
        assert s.map_region("region:nope") == {}

        unc = s.map_uncovered("api-copy")
        assert len(unc) == 2
        assert all(x["key"].startswith("uncovered:api-copy:") for x in unc)
    finally:
        s.close()


def test_load_map_cli_real_artifact(tmp_path):
    """load_map.py на реальном merged-артефакте (symbols+edges+regions+uncovered)."""
    artifact = os.path.join(TOOLS, "artifacts", "refactoring-map.json")
    if not os.path.isfile(artifact):
        pytest.skip("артефакт сканера не в git (вне git, перегенерируемый); "
                    "собирается локально через scan_symbols.py")
    db = str(tmp_path / "map.mdbx")
    env = dict(os.environ, PYTHONPATH=MODULE_DIR)
    p = subprocess.run(
        [sys.executable, os.path.join(TOOLS, "load_map.py"),
         "--db", db, "--replace"],
        capture_output=True, text=True, env=env, timeout=300)
    assert p.returncode == 0, p.stderr[-2000:]
    s = Store(db, readonly=True, _skip_sync_thread=True)
    try:
        assert len(s.map_symbols()) >= 4000
        assert len(s.map_regions(limit=10 ** 9)) >= 1000
        assert len(s.map_uncovered(limit=10 ** 9)) >= 400
        assert len(s.map_regions("region:api-", limit=10)) > 0
    finally:
        s.close()


def test_reopen_reloads_vocab(tmp_path):
    """Словарь, добавленный ВНЕШНИМ процессом, виден после переоткрытия env."""
    db = str(tmp_path / "t.mdbx")
    sA = Store(db, _skip_sync_thread=True)
    try:
        # внешний процесс (как distill.py/restore.py) добавляет тему в словарь
        mod = MODULE_DIR
        code = ("import sys; sys.path.insert(0,%r); from mcp import Store; "
                "s=Store(sys.argv[1], _skip_sync_thread=True); "
                "print(s.vocab_add('practice','vocab-reopen')['result']); s.close()"
                % mod)
        p = subprocess.run([sys.executable, "-c", code, db],
                           capture_output=True, text=True, timeout=60)
        assert p.returncode == 0, p.stderr
        # без переоткрытия запись с новой темой падала бы (stale vocab)
        with pytest.raises(Exception):
            sA.safe_store("fact:practice:vocab-reopen", "fact", "x", 0.5)
        # переоткрытие (ro -> rw) перечитывает словарь
        sA.set_readonly(True)
        sA.set_readonly(False)
        res = sA.safe_store("fact:practice:vocab-reopen", "fact", "x", 0.5)
        assert res["result"] in ("created", "merged"), res
    finally:
        sA.close()