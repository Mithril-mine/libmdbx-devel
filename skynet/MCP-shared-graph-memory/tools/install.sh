#!/usr/bin/env bash
# Безопасная установка/синхронизация shared-graph-memory в каталог установки.
#
# ПЕРВОПРИЧИНА потери данных 26.09: rsync --delete живого каталога данных
# удалил db.mdbx. Этот скрипт НИКОГДА не трогает файлы данных:
#   db.mdbx, db.mdbx-lck, backups/
#
# Использование:
#   tools/install.sh [DEST]
#   DEST по умолчанию: ~/.local/share/shared-graph-memory
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$HOME/.local/share/shared-graph-memory}"

mkdir -p "$DEST"
rsync -a \
  --exclude='db.mdbx' \
  --exclude='db.mdbx-lck' \
  --exclude='backups/' \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  --exclude='.git' \
  --exclude='tools/artifacts/' \
  "$SRC/mcp/" "$DEST/mcp/"
rsync -a \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  "$SRC/server.py" "$SRC/SKILL.md" "$SRC/pytest.ini" "$SRC/README.md" "$DEST/"
rsync -a \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  "$SRC/tools/" "$DEST/tools/"

echo "installed -> $DEST (данные БД и backups/ НЕ тронуты)"
ls -la "$DEST" | head