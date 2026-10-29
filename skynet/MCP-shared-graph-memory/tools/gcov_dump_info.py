#!/usr/bin/env python3
"""gcov_dump_info.py — построить lines.info (lcov) из .gcov-файлов.

Используется CI в Windows-ветке coverage report (нет lcov на windows,
mingw-gcov пишет .gcov-файлы): пробегает по glob-у, собирает
SF/DA/end_of_record. Пути нормализует: оставляет как git-путь (relative
к repo-root или уже git-relative), срезая префикс root.

Вывод — в stdout (перенаправляется в coverage/lines.info в workflow).

Usage:
  python3 tools/gcov_dump_info.py [--gcov-glob '/tmp/*.gcov'] [--root <abs>]
"""

import argparse
import glob
import os
import re

_COVERED_RE = re.compile(r"^\s*(\d+)\*?:\s*(\d+):")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcov-glob", default="/tmp/*.gcov")
    ap.add_argument("--root", default="")
    args = ap.parse_args()

    out = []
    for gcov in sorted(glob.glob(args.gcov_glob)):
        src, covered = None, set()
        for ln in open(gcov, errors="replace"):
            if ln.startswith("-:0:Source:"):
                src = ln.split("Source:", 1)[1].strip()
                continue
            m = _COVERED_RE.match(ln)
            if m and int(m.group(1)) > 0:
                covered.add(int(m.group(2)))
        if not src:
            continue
        if args.root and src.startswith(args.root):
            src = src[len(args.root):].lstrip("/\\")
        out.append("SF:%s" % src)
        for l in sorted(covered):
            out.append("DA:%d,1" % l)
        out.append("end_of_record")
    print("\n".join(out))


if __name__ == "__main__":
    main()