"""Тесты конвертера lcov → cobertura (tests/ci/lcov_to_cobertura.py)."""

import os
import subprocess
import sys
import xml.etree.ElementTree as ET

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "lcov_to_cobertura.py")


def _run(trace, tmp_path):
    src = tmp_path / "lines.info"
    src.write_text(trace, encoding="utf-8")
    r = subprocess.run([sys.executable, TOOL, str(src)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return ET.fromstring(r.stdout)


def test_line_coverage_fields(tmp_path):
    """SF/DA/LF/LH секция → класс с построчными hits и корневой line-rate."""
    root = _run("TN:\n"
                "SF:src/api-cold.c\n"
                "DA:1,5\n"
                "DA:2,0\n"
                "FNL:0,10,20\n"          # записи lcov 2.x игнорируются
                "FNA:0,5,name\n"
                "LF:2\n"
                "LH:1\n"
                "end_of_record\n"
                "SF:src/api-copy.c\n"
                "DA:7,2\n"
                "LF:1\n"
                "LH:1\n"
                "end_of_record\n", tmp_path)
    assert root.tag == "coverage"
    assert root.get("lines-valid") == "3"
    assert root.get("lines-covered") == "2"
    assert root.get("line-rate") == "0.6667"
    classes = {c.get("filename"): c for c in root.iter("class")}
    a, b = classes["src/api-cold.c"], classes["src/api-copy.c"]
    a_lines = {(l.get("number"), l.get("hits")) for l in a.iter("line")}
    assert a_lines == {("1", "5"), ("2", "0")}, "покрытые и непокрытые строки"
    assert a.get("line-rate") == "0.5000"
    assert b.get("line-rate") == "1.0000"


def test_empty_and_garbage_tolerated(tmp_path):
    """Пустой tracefile → валидный XML с нулями; мусорные DA пропускаются."""
    root = _run("", tmp_path)
    assert root.get("lines-valid") == "0"
    assert root.get("line-rate") == "0"
    root = _run("SF:src/x.c\n"
                "DA:1,notanumber\n"
                "DA:2,3\n"
                "end_of_record\n", tmp_path)
    lines = list(root.iter("line"))
    assert len(lines) == 1 and lines[0].get("number") == "2"
