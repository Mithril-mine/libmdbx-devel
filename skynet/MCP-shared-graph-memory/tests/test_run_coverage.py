"""Тесты пайплайна пер-тестового покрытия (run_coverage.py).

Ориентированы на формат вывода ctest: нумерация тестов выравнивается
пробелами (`Test   #1: c_api`), а имена могут содержать regex-метасимволы
(`c++_api`). Оба случая ранее ломали список/прогон тестов.

Прогон через субпроцесс мокается, чтобы не зависеть от реальной ctest-сборки
(кроссплатформенно, без сравнений pagesize и производных с константами).
"""

import os
import re
import subprocess
import sys

import pytest

TOOLS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "tools")
sys.path.insert(0, TOOLS)
import run_coverage as r  # noqa: E402

# Реальный вывод `ctest -N` (см. ctest :выравнивание по колонкам):
#     Test   #1: c_api
#     Test   #2: c++_api
#     ...
#     Test   #99: txn
#     Test #100: crash_safety
CTEST_N_SAMPLE = (
    "Internal ctest changing into directory: /build\n"
    "Test project /build\n"
    "  Test   #1: c_api\n"
    "  Test   #2: c++_api\n"
    "  Test   #3: mdbx_ex_01-hello\n"
    "  Test  #99: txn\n"
    "  Test #100: crash_safety\n"
    "  Test #101: buffers\n"
    "Total Tests: 6\n"
)

PADDED_LINES = [
    "  Test   #1: c_api",
    "  Test   #2: c++_api",
    "  Test  #99: txn",
    "  Test #100: crash_safety",
]


def _fake_ctest(monkeypatch, stdout_text, returncode=0):
    """Подмена subprocess.run на фейк, возвращающий заданный вывод ctest."""

    calls = {}

    def fake_run(cmd, capture_output=False, text=False):
        calls["cmd"] = cmd
        return type("FakeProc", (), {"returncode": returncode,
                                     "stdout": stdout_text,
                                     "stderr": ""})()

    monkeypatch.setattr(subprocess, "run", fake_run)
    return calls


def test_ctest_list_parses_padded_test_numbers(monkeypatch, tmp_path):
    """ctest -N выравнивает номера пробелами: 'Test   #1: c_api'.

    Старый парсер (startswith("Test #")) пропускал такие строки и терял
    первые 99 тестов; должен разбирать и однозначный, и выровненный вывод.
    """
    calls = _fake_ctest(monkeypatch, CTEST_N_SAMPLE)
    tests = r.ctest_list(str(tmp_path))
    assert calls["cmd"][:2] == ["ctest", "--test-dir"]
    assert calls["cmd"][2] == str(tmp_path)
    assert tests == ["c_api", "c++_api", "mdbx_ex_01-hello",
                     "txn", "crash_safety", "buffers"]


@pytest.mark.parametrize("line,expected", [
    ("  Test   #1: c_api", "c_api"),
    ("  Test  #99: txn", "txn"),
    ("  Test #100: crash_safety", "crash_safety"),
])
def test_ctest_line_regex_matches_padded_numbers(line, expected):
    """Прямая проверка regex: выровненные номера матчатся, имя не теряется."""
    # то же выражение, что в ctest_list (локальная копия — защита от рефакторинга
    # теста в отрыве от реализации)
    m = re.match(r"^\s*Test\s+#\d+:\s*(.+)$", line)
    assert m
    assert m.group(1) == expected


def test_ctest_list_handles_error(monkeypatch, tmp_path):
    """При падении ctest -N список пуст и в stderr пишется причина."""
    _fake_ctest(monkeypatch, "", returncode=1)
    assert r.ctest_list(str(tmp_path)) == []


def test_run_one_test_escapes_regex_metachars(monkeypatch, tmp_path):
    """Имя теста с '+' (c++_api) не должно трактоваться как regex.

    Старая команда: ctest -R "^c++_api$" — ноль совпадений ('+' = 1 и более).
    Новая: паттерн экранируется через re.escape.
    """
    calls = _fake_ctest(monkeypatch, "", returncode=0)
    ok = r.run_one_test(str(tmp_path), "c++_api", timeout=42)
    assert ok
    cmd = calls["cmd"]
    assert cmd[0] == "ctest"
    assert "--test-dir" in cmd
    assert str(tmp_path) in cmd
    i = cmd.index("-R")
    assert cmd[i + 1] == "^c\\+\\+_api$"
    assert "42" in cmd


def test_run_one_test_reports_failure(monkeypatch, tmp_path):
    _fake_ctest(monkeypatch, "", returncode=1)
    assert r.run_one_test(str(tmp_path), "any") is False