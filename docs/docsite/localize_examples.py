#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Localize a per-locale copy of the canonical examples/ tree for the textbook
# and the unified docsite. The canonical tree stays bilingual (comments carry
# the '// EN:' / '// RU:' marker) and is what CI builds and tests; this script
# produces the locale-pure variant used by the generated docs/{ru,en}/textbook
# trees (see GNUmakefile 'sync-examples').
#
# For the requested locale the script:
#   - copies the canonical tree into the destination;
#   - drops comment lines marked with the other language ('// EN:' in the
#     'ru' copy and '// RU:' in the 'en' copy) and strips the now-redundant
#     language marker from the kept ones ('// RU: text' -> '// text');
#   - substitutes per-locale translations of the prose files when they are
#     tracked next to the canonical ones:
#       * README.md          <- README.<locale>.md      (ship as README.txt)
#       * sample-bdb.txt     <- sample-bdb.ru.txt       (RU copy only)
#
# All other files (CMakeLists.txt, license headers, mechanical comments,
# etc.) are copied and shipped unchanged.

import os
import re
import shutil
import sys

CODE_SUFFIXES = ('.c', '.c++', '.h', '.h++')
LOCALES = ('en', 'ru')

# '// EN: text' / '// RU: text' at the beginning of a comment line, keeping
# any leading indentation.
LANGUAGE_COMMENT = re.compile(r'^(\s*)//\s*(EN|RU):(.*)$')


def filter_comments(lines, locale):
    """Return a locale-pure copy of a source file (list of lines)."""
    out = []
    for line in lines:
        body = line
        newline = ''
        if body.endswith('\r\n'):
            body, newline = body[:-2], '\r\n'
        elif body.endswith('\n'):
            body, newline = body[:-1], '\n'
        m = LANGUAGE_COMMENT.match(body)
        if m:
            if m.group(2).lower() == locale:
                text = m.group(3)
                if text.startswith(' '):
                    text = text[1:]
                out.append(f'{m.group(1)}// {text}{newline}')
            # lines marked with the other language are dropped
        else:
            out.append(line)
    return out


def process_file(path, locale):
    if path.endswith(CODE_SUFFIXES):
        with open(path, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        filtered = filter_comments(lines, locale)
        if filtered != lines:
            with open(path, 'w', encoding='utf-8', newline='') as f:
                f.writelines(filtered)


def substitute_translations(root, locale):
    """Replace prose files with (or by) their per-locale translations.

    Works on the copied tree; translation sources are tracked next to the
    canonical files in the source tree (README.<locale>.md, sample-bdb.ru.txt)
    and are removed from the generated copy afterwards.
    """
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            if name == 'README.md':
                translated = os.path.join(dirpath, f'README.{locale}.md')
                if os.path.isfile(translated):
                    shutil.copyfile(translated, path)
                os.replace(path, os.path.join(dirpath, 'README.txt'))
            elif name == 'sample-bdb.txt' and locale == 'ru':
                translated = os.path.join(dirpath, 'sample-bdb.ru.txt')
                if os.path.isfile(translated):
                    shutil.copyfile(translated, path)


def drop_aux_files(root):
    """Remove the per-locale translation sources from the generated copy.

    The translations (README.<locale>.md, sample-bdb.ru.txt) are tracked next
    to the canonical files and must not ship inside the generated copies;
    their content is either substituted into the final file names during
    substitute_translations() or irrelevant for this locale.
    """
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if re.match(r'^README\.(en|ru)\.md$', name) or name == 'sample-bdb.ru.txt':
                os.remove(os.path.join(dirpath, name))


def main():
    if len(sys.argv) != 4:
        sys.exit(f'usage: {sys.argv[0]} <en|ru> <dst>')
    locale = sys.argv[1].lower()
    if locale not in LOCALES:
        sys.exit(f'unknown locale: {sys.argv[1]!r}')
    src, dst = sys.argv[2], sys.argv[3]
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst, symlinks=False)
    substitute_translations(dst, locale)
    drop_aux_files(dst)
    for dirpath, _dirnames, filenames in os.walk(dst):
        for name in filenames:
            process_file(os.path.join(dirpath, name), locale)


if __name__ == '__main__':
    main()