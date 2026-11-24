#!/usr/bin/env python3
"""lcov_to_cobertura.py — конвертация lcov tracefile в cobertura XML.

codecov стабильно прогоняет cobertura-отчёты (формат coverage.py — тот
же, что у python-сканов ci-codegraph), а lcov-варианты принимает с
HTTP 200 и молча теряет при обработке (наблюдение 2026-11-24: не
появились ни tracefile lcov 2.x с FNL:/FNA:, ни сведённый к
классическому SF/DA). Записи функций lcov 2.x FNL:/FNA: и ветви BRDA:
игнорируются — codecov мержит построчное покрытие (SF/DA), из LF:/LH:
берётся итог для секции.

Usage:
  python3 tests/ci/lcov_to_cobertura.py coverage/lines.info > coverage/coverage.xml
"""

import argparse
import time
import xml.etree.ElementTree as ET


def parse_tracefile(path):
    """Секции tracefile → [(filename, [(line, hits), ...], lf, lh), ...].

    Неизвестные записи (TN:, FNL:, FNA:, BRDA:, ...) пропускаются; DA
    с нечисловыми полями пропускается, а не валит конвертацию.
    """
    files = []
    cur = None
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith("SF:"):
                cur = {"name": line[3:], "lines": [], "lf": 0, "lh": 0}
                files.append(cur)
            elif cur is None:
                continue
            elif line.startswith("DA:"):
                parts = line[3:].split(",")
                if len(parts) < 2 or not parts[0].isdigit() or not parts[1].isdigit():
                    # Мусорная DA (усечённая запись, посторонний мусор) не
                    # должна ронять конвертацию всего отчёта.
                    continue
                cur["lines"].append((int(parts[0]), int(parts[1])))
            elif line.startswith("LF:") and line[3:].isdigit():
                cur["lf"] = int(line[3:])
            elif line.startswith("LH:") and line[3:].isdigit():
                cur["lh"] = int(line[3:])
    return files


def build_coverage(files):
    """Корневой <coverage> cobertura: <class> на файл, <line> на DA."""
    attrs = {
        "line-rate": "0",
        "branch-rate": "0",
        "lines-covered": "0",
        "lines-valid": "0",
        "branches-covered": "0",
        "branches-valid": "0",
        "complexity": "0",
        "version": "1.9",
        "timestamp": str(int(time.time())),
    }
    root = ET.Element("coverage", attrs)
    ET.SubElement(ET.SubElement(root, "sources"), "source").text = "."
    packages = ET.SubElement(root, "packages")
    valid_total = covered_total = 0
    for f in files:
        name, lines = f["name"], f["lines"]
        lf, lh = f["lf"], f["lh"]
        # LF/LH секции авторитетнее подсчёта по DA (lcov может писать
        # условные LH при отрицательных счётчиках, отфильтрованных
        # --ignore-errors).
        valid, covered = (lf, lh) if lf >= lh and lf > 0 else (len(lines), sum(h > 0 for _, h in lines))
        valid_total += valid
        covered_total += covered
        cls = ET.SubElement(
            ET.SubElement(
                ET.SubElement(packages, "package",
                              {"name": name.rsplit("/", 1)[0] if "/" in name else ".",
                               "line-rate": f"{covered / valid:.4f}" if valid else "0",
                               "branch-rate": "0",
                               "complexity": "0"}),
                "classes"),
            "class",
            {"name": name.rsplit("/", 1)[-1], "filename": name,
             "line-rate": f"{covered / valid:.4f}" if valid else "0",
             "branch-rate": "0", "complexity": "0"})
        ET.SubElement(cls, "methods")
        lines_el = ET.SubElement(cls, "lines")
        for num, hits in lines:
            ET.SubElement(lines_el, "line",
                          {"number": str(num), "hits": str(hits), "branch": "false"})
    root.set("lines-covered", str(covered_total))
    root.set("lines-valid", str(valid_total))
    root.set("line-rate", f"{covered_total / valid_total:.4f}" if valid_total else "0")
    return root


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tracefile", help="lcov tracefile (lines.info)")
    args = ap.parse_args()
    files = parse_tracefile(args.tracefile)
    root = build_coverage(files)
    ET.indent(root)
    print('<?xml version="1.0" ?>')
    print(ET.tostring(root, encoding="unicode"))


if __name__ == "__main__":
    main()
