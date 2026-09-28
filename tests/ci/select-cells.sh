#!/usr/bin/env bash
# Infra v3 (C1): diff -> cell selection. Maps changed paths onto the
# path_zones registry (tests/ci/config.json) and prints the union of cell
# ids to run for the fast tier. Combined with run-cell.sh --scope fast this
# is the change-addressed push gate: "never run what did not change".
#
# Usage:
#   tests/ci/select-cells.sh [--base <ref>] [path...]
#   tests/ci/select-cells.sh                    # changed = git diff vs origin/devel
#   tests/ci/select-cells.sh src/cursor.c       # explicit paths
#   tests/ci/select-cells.sh --profile push-quick   # fixed profile (nightly/full)
#
# Output: newline-separated cell ids on stdout; stats on stderr.
# Exit: 0 = ok (may be empty), 1 = nothing selected, 2 = usage/error.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REGISTRY="${REPO_ROOT}/tests/ci/config.json"

BASE=origin/devel
MODE=diff
PROFILE=""
ARGS=()

while [ $# -gt 0 ]; do
	case "$1" in
	--base)
		shift
		[ $# -gt 0 ] || { echo "select-cells: --base needs a ref" >&2; exit 2; }
		BASE="$1"
		;;
	--profile)
		shift
		[ $# -gt 0 ] || { echo "select-cells: --profile needs a name" >&2; exit 2; }
		MODE=profile
		PROFILE="$1"
		;;
	-h | --help)
		sed -n '2,13p' "$0" | sed 's/^# \?//'
		exit 0
		;;
	--)
		shift
		ARGS+=("$@")
		break
		;;
	-*)
		echo "select-cells: unknown option $1" >&2
		exit 2
		;;
	*)
		ARGS+=("$1")
		;;
	esac
	shift
done

if [ "$MODE" = "profile" ]; then
	python3 -c "
import json, sys
cfg = json.load(open('${REGISTRY}'))
profiles = cfg.get('profiles', {})
if '${PROFILE}' not in profiles:
    print('select-cells: unknown profile \"${PROFILE}\"', file=sys.stderr)
    sys.exit(1)
print('\n'.join(profiles['${PROFILE}']))
"
	exit 0
fi

paths=("${ARGS[@]}")
if [ ${#paths[@]} -eq 0 ]; then
	if ! git -C "$REPO_ROOT" rev-parse --verify -q "$BASE" >/dev/null 2>&1; then
		echo "select-cells: base '$BASE' not found (give --base <ref> or explicit paths)" >&2
		exit 1
	fi
	mapfile -t paths < <(git -C "$REPO_ROOT" diff --name-only "$BASE"...HEAD 2>/dev/null)
	mapfile -t -O ${#paths[@]} paths < <(
		git -C "$REPO_ROOT" diff --name-only 2>/dev/null
		git -C "$REPO_ROOT" ls-files --others --exclude-standard 2>/dev/null
	)
	if [ ${#paths[@]} -eq 0 ]; then
		echo "select-cells: no changes vs $BASE" >&2
		exit 1
	fi
fi

python3 -c "
import json, sys
cfg = json.load(open('${REGISTRY}'))
zones = list(cfg.get('path_zones', {}).keys())
print('\n'.join(zones))
" > "${TMPDIR:-/tmp}/select-cells-zones.$$"
mapfile -t ZONES < "${TMPDIR:-/tmp}/select-cells-zones.$$"
rm -f "${TMPDIR:-/tmp}/select-cells-zones.$$"

declare -A seen
result=()
for p in "${paths[@]}"; do
	matched=false
	for zone in "${ZONES[@]}"; do
		if [[ "$p" == "$zone"* ]]; then
			matched=true
			while IFS= read -r cid; do
				[ -n "$cid" ] || continue
				[ -n "${seen[$cid]:-}" ] && continue
				seen[$cid]=1
				result+=("$cid")
			done < <(python3 -c "
import json
cfg = json.load(open('${REGISTRY}'))
for cid in cfg.get('path_zones', {}).get('${zone}', []):
    print(cid)
")
		fi
	done
	if ! $matched; then
		echo "select-cells: no zone for '$p'" >&2
	fi
done

if [ ${#result[@]} -eq 0 ]; then
	echo "select-cells: no cells selected (docs-only change?)" >&2
	exit 1
fi
printf '%s\n' "${result[@]}"
echo "select-cells: selected ${#result[@]} cells for ${#paths[@]} changed paths" >&2
exit 0