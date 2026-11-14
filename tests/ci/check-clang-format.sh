#!/usr/bin/env bash
# Checks that the tracked C/C++ sources conform to the repository
# .clang-format style — the CI enforcement of the AGENTS.md §7 rule
# "run clang-format before publishing".
#
# Usage: tests/ci/check-clang-format.sh [clang-format-binary]
#   The binary defaults to $CLANG_FORMAT, then to `clang-format` on the
#   PATH. The style is version-robust (verified with clang-format
#   14/18/21 over the full corpus).
#
# On violations: lists the files and prints the corrective unified diff
# (apply it with `patch -p1` or `clang-format -i`); exits 1.

set -uo pipefail

CF="${CLANG_FORMAT:-${1:-clang-format}}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

command -v "$CF" > /dev/null 2>&1 || {
  echo "clang-format binary not found: $CF" >&2
  exit 2
}
[ -f .clang-format ] || {
  echo ".clang-format not found in the repository root" >&2
  exit 2
}

mapfile -t files < <(git ls-files -- '*.c' '*.c++' '*.h' '*.h++')
if [ ${#files[@]} -eq 0 ]; then
  echo "clang-format: no tracked C/C++ sources to check"
  exit 0
fi

diag="$(mktemp)"
fail_list="$(mktemp)"
trap 'rm -f "$diag" "$fail_list"' EXIT

# One clang-format invocation per batch (--dry-run -Werror reports every
# violation to stderr with the file path); the parallel batches share the
# diagnostics file.
printf '%s\0' "${files[@]}" | xargs -0 -P "$(nproc)" -n 16 "$CF" --dry-run -Werror 2> "$diag" || true

# The diagnostics read 'path:line:col: error|warning: code should be
# clang-formatted' - clang-format 18 emits 'error', 21 emits 'warning'.
grep -E '^[^ :].*:[0-9]+:[0-9]+: (error|warning): code should be clang-formatted' \
  "$diag" | sed -E 's/:[0-9]+:[0-9]+: .*//' | sort -u > "$fail_list"

if [ ! -s "$fail_list" ]; then
  echo "clang-format: all ${#files[@]} files conform"
  exit 0
fi

count=$(wc -l < "$fail_list")
echo "clang-format: $count file(s) do not conform to .clang-format:"
cat "$fail_list"
echo "== corrective patch (apply with: patch -p1) =="
while IFS= read -r f; do
  "$CF" "$f" | diff -u "$f" - || true
done < "$fail_list"
exit 1
