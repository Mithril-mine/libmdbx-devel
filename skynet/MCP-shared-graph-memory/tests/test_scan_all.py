"""Тесты оркестратора многоплатформенного сканирования (tools/scan_all.py).

Инструменты не запускаются по-настоящему: subprocess.run мокается, чтобы
тесты не зависели от _build-scan/compile_commands и работали на любой
платформе (без сравнений pagesize и производных с константами).
"""

import json
import os
import subprocess
import sys

import pytest

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "tools")
sys.path.insert(0, TOOLS)
import scan_all as sa  # noqa: E402


class _FakeProc:
    returncode = 0
    stdout = ""
    stderr = ""


@pytest.fixture()
def fake_run(monkeypatch, tmp_path):
    calls = []

    def _run(cmd, **kwargs):
        calls.append(cmd)
        return _FakeProc()

    monkeypatch.setattr(subprocess, "run", _run)
    # все артефакты пишем во временный каталог, чтобы не мусорить в репо
    monkeypatch.setattr(sa, "ARTIFACTS", str(tmp_path))
    return calls


def _argv(configs="linux,win32", **flags):
    a = ["--configs", configs, "--db", ":memory:", "--skip-build"]
    # --skip-load по умолчанию, если явно не передан skip_load=False
    if not flags.get("skip_load", True):
        flags.pop("skip_load", None)
    else:
        flags.setdefault("skip_load", True)
    for k, v in flags.items():
        if v is True:
            a.append("--" + k.replace("_", "-"))
        elif v:
            a += ["--" + k.replace("_", "-"), str(v)]
    return a


def _write_artifact(tmp_path, name, functions):
    path = os.path.join(str(tmp_path), name)
    body = {
        "config": name.split("-")[1].split(".")[0],
        "configs": [name.split("-")[1].split(".")[0]],
        "platform": "test",
        "counts": {"functions": functions, "types": 1, "macros": 1,
                   "blocks": 10, "edges": 2, "unresolved_edges": 0,
                   "regions": 3},
        "symbols": {"fn:f%d" % i: {"kind": "function", "name": "f%d" % i,
                                    "file": "src/x.c", "module": "x",
                                    "l0": 1, "l1": 2, "blocks": [], "regions": []}
                    for i in range(functions)},
        "edges": [], "regions": [], "tus": [], "errors": {},
    }
    with open(path, "w") as f:
        json.dump(body, f)
    return path


def test_win32_scan_uses_mingw_target(fake_run, tmp_path):
    """win32-конфиг сканируется через clang --target=x86_64-w64-windows-gnu."""
    _write_artifact(tmp_path, "refactoring-map-linux.json", 3)
    _write_artifact(tmp_path, "refactoring-map-win32.json", 3)
    sa.main(_argv())
    win = [c for c in fake_run if "--config" in c and "win32" in c[c.index("--config") + 1:]]
    assert win, "нет вызова scan_symbols.py --config win32"
    assert "--target" in win[0] and win[0][win[0].index("--target") + 1] == "x86_64-w64-windows-gnu", win[0]


def test_macos_without_ci_artifact_fails(fake_run):
    """macos без --artifacts-from-ci — понятная ошибка (локально нет SDK)."""
    with pytest.raises(SystemExit) as exc:
        sa.main(_argv(configs="linux,macos"))
    assert exc.value.code == 1


def test_merge_requires_two_artifacts(fake_run, tmp_path):
    """<2 артефакта для merge — понятная ошибка (fail-loudly)."""
    _write_artifact(tmp_path, "refactoring-map-linux.json", 3)
    with pytest.raises(SystemExit) as exc:
        sa.main(_argv(configs="linux,win32"))
    assert exc.value.code == 1


def test_report_prints_merged_counts(fake_run, tmp_path):
    """После успешного merge report печатает счётчики merged-артефакта."""
    _write_artifact(tmp_path, "refactoring-map-linux.json", 4)
    _write_artifact(tmp_path, "refactoring-map-win32.json", 5)
    # merged-артефакт (оркестратор бы его создал; тут — вручную для отчёта)
    merged = os.path.join(str(tmp_path), "refactoring-map.json")
    body = {"configs": ["linux", "win32"], "counts": {"functions": 7},
            "uncovered": [], "symbols": {}, "edges": [], "regions": []}
    with open(merged, "w") as f:
        json.dump(body, f)
    sa.main(_argv())


def test_dry_run_prints_commands_without_executing(fake_run, tmp_path):
    """--dry-run не исполняет subprocess, только печатает."""
    sa.main(_argv(dry_run=True))
    assert fake_run == [], "dry-run не должен вызывать subprocess"


def test_merge_fails_loudly_on_missing_requested_config(fake_run, tmp_path, capsys):
    """Запрошенный конфиг без артефакта — явная ошибка, не тихий пропуск.

    Раньше оркестратор фильтровал merge_inputs по os.path.exists() и молча
    терял запрошенную платформу из merged-артефакта.
    """
    _write_artifact(tmp_path, "refactoring-map-linux.json", 3)
    # win32 запрошен, но его артефакта нет
    with pytest.raises(SystemExit) as exc:
        sa.main(_argv(configs="linux,win32"))
    assert exc.value.code == 1
    err = capsys.readouterr().err
    assert "win32" in err and "artifact" in err.lower()

def test_load_makes_pin_backup_before_replace(fake_run, tmp_path, capsys):
    """load: pin-бэкап (mcp.cli backup) идёт ПЕРЕД load_map --replace.

    Защита от перезаписи структурного слоя без контрактной точки
    сохранения — то, что обещает REFACTORING-MAP.md.
    """
    _write_artifact(tmp_path, "refactoring-map-linux.json", 3)
    _write_artifact(tmp_path, "refactoring-map-win32.json", 3)
    # merged-артефакт для отчёта
    merged = os.path.join(str(tmp_path), "refactoring-map.json")
    with open(merged, "w") as f:
        json.dump({"configs": ["linux", "win32"], "counts": {"functions": 7},
                   "uncovered": [], "symbols": {}, "edges": [], "regions": []}, f)
    sa.main(_argv(skip_load=False))
    cmds = [str(c) for c in fake_run]
    backup_i = next((i for i, c in enumerate(cmds) if "mcp.cli" in c and "backup" in c), None)
    load_i = next((i for i, c in enumerate(cmds) if "load_map.py" in c), None)
    assert backup_i is not None, "ожидался pin-бэкап перед load"
    assert load_i is not None, "ожидался load_map"
    assert backup_i < load_i, "pin-бэкап должен идти раньше load_map --replace"


def test_load_skips_backup_with_skip_backup(fake_run, tmp_path):
    """--skip-backup отключает pin-бэкап, load_map остаётся."""
    _write_artifact(tmp_path, "refactoring-map-linux.json", 3)
    _write_artifact(tmp_path, "refactoring-map-win32.json", 3)
    merged = os.path.join(str(tmp_path), "refactoring-map.json")
    with open(merged, "w") as f:
        json.dump({"configs": ["linux", "win32"], "counts": {"functions": 7},
                   "uncovered": [], "symbols": {}, "edges": [], "regions": []}, f)
    sa.main(_argv(skip_load=False, skip_backup=True))
    cmds = [str(c) for c in fake_run]
    assert not any("mcp.cli" in c and "backup" in c for c in cmds),         "--skip-backup не должен вызывать pin-бэкап"
    assert any("load_map.py" in c for c in cmds), "load_map должен запуститься"


