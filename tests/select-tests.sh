#!/usr/bin/env bash
#
# select-tests.sh — impact-based CTest selection for libmdbx-devel (TASK-22).
#
# Maps changed paths onto the hierarchical CTest label tree (see AGENTS.md
# "Segmented test execution" and tests/CMakeLists.txt) so an agent runs only
# the relevant tests instead of the whole corpus:
#
#   tests/select-tests.sh [--base <ref>] [path...]
#   tests/select-tests.sh                # changed = git diff vs origin/devel
#   tests/select-tests.sh src/cursor.c tests/ut/dbi/dbi_nested_txn.c++
#
# Output: the recommended ctest invocation on stdout, the collected labels on
# stderr. The script itself never runs ctest; combine with the label regex:
#
#   ctest -L "$(tests/select-tests.sh | ...)"   # see --run suggestion
#
# Exit status: 0 = clean; 1 = nothing selected; 2 = usage/error.
set -eu

BASE=origin/devel
MODE=query
ARGS=()

while [ $# -gt 0 ]; do
	case "$1" in
	--base)
		shift
		[ $# -gt 0 ] || { echo "select-tests: --base needs a ref" >&2; exit 2; }
		BASE="$1"
		;;
	--run)
		MODE=run
		;;
	-h | --help)
		sed -n '2,20p' "$0" | sed 's/^# \?//'
		exit 0
		;;
	--)
		shift
		ARGS+=("$@")
		break
		;;
	-*)
		echo "select-tests: unknown option $1" >&2
		exit 2
		;;
	*)
		ARGS+=("$1")
		;;
	esac
	shift
done

repo_root="$(cd "$(dirname "$0")/.." && pwd)"

if [ ${#ARGS[@]} -gt 0 ]; then
	paths=("${ARGS[@]}")
else
	if ! git -C "$repo_root" rev-parse --verify -q "$BASE" >/dev/null 2>&1; then
		echo "select-tests: base '$BASE' not found (give --base <ref> or explicit paths)" >&2
		exit 1
	fi
	mapfile -t paths < <(git -C "$repo_root" diff --name-only "$BASE"...HEAD 2>/dev/null)
	# Include uncommitted (staged+unstaged+untracked) changes so the tool also
	# works while validating a dirty working tree before commit.
	mapfile -t -O ${#paths[@]} paths < <(
		git -C "$repo_root" diff --name-only 2>/dev/null
		git -C "$repo_root" ls-files --others --exclude-standard 2>/dev/null
	)
	if [ ${#paths[@]} -eq 0 ]; then
		echo "select-tests: no changes vs $BASE" >&2
		exit 1
	fi
fi

# label-regex fragments; matches union into LABELS below
declare -A AREA=(
	[tests/ut/api/]='ut\.api'
	[tests/ut/cxx/]='ut\.cxx'
	[tests/ut/env/]='ut\.env'
	[tests/ut/dbi/]='ut\.dbi'
	[tests/ut/txn/]='ut\.txn'
	[tests/ut/cursor/]='ut\.cursor'
	[tests/ut/gc/]='ut\.gc'
	[tests/ut/issues/]='ut\.issues'
)

LABELS=""
HEAVY=0

classify() {
	local p="$1"
	case "$p" in
	tests/ut/api/*) echo 'ut\.api' ;;
	tests/ut/cxx/*) echo 'ut\.cxx' ;;
	tests/ut/env/*) echo 'ut\.env' ;;
	tests/ut/dbi/*) echo 'ut\.dbi' ;;
	tests/ut/txn/*) echo 'ut\.txn' ;;
	tests/ut/cursor/*) echo 'ut\.cursor' ;;
	tests/ut/gc/*) echo 'ut\.gc' ;;
	tests/ut/issues/*) echo 'ut\.issues' ;;
	tests/ut/*) echo 'ut\.' ;;
	tests/framework/*) echo $'smoke-t1\nsmoke-t2\nsmoke-t3\nstochastic' ;;
	tests/*.sh | tests/*.py | tests/**/*.sh | tests/**/*.py | tests/CMakeLists.txt) echo $'smoke-t1\nsmoke-t2\nsmoke-t3' ;;
	cmake/* | CMakeLists.txt | GNUmakefile | Makefile | cmake/CMakeLists.txt) echo $'smoke-t1\nsmoke-t2\nut\.' ;;
	mdbx.h | mdbx.h++ | mdbx++/*) echo $'ut\.\nsmoke-t1\nsmoke-t2' ;;
	src/*.c | src/*.h | src/alloy.c) echo $'ut\.\nsmoke-t1\nsmoke-t2' ;;
	docs/* | skynet/* | .github/* | .gitignore) echo '' ;;
	*.md) echo '' ;;
	*) echo '' ;;
	esac
}

for p in "${paths[@]}"; do
	[ -n "$p" ] || continue
	got="$(classify "$p")"
	if [ -z "$got" ]; then
		echo "select-tests: no test area for '$p'" >&2
		continue
	fi
	while read -r g; do
		[ -n "$g" ] || continue
		case "$LABELS" in
		*"$g"*) ;;
		*) LABELS="${LABELS:+$LABELS|}$g" ;;
		esac
	done <<<"$got"
done

if [ -z "$LABELS" ]; then
	echo "select-tests: no tests affected" >&2
	exit 1
fi

echo "affected labels: $LABELS" >&2
echo "changed paths:   ${paths[*]}" >&2

# Fast unit set by default (skip ut.heavy); a core/build change also pulls in
# the tiered smoke labels. Integrity-only *_chk tests carry the orthogonal `chk`
# label (B26) and are excluded from routine runs (run them with `ctest -L chk`;
# plain `ctest` still includes them). The regex is applied per-label by CTest,
# so the alternation needs no grouping.
CMD="ctest -L '${LABELS}' -LE 'ut\.heavy|chk'"
echo "$CMD"

if [ "$MODE" = run ]; then
	# The ctest invocation needs a configured build dir. If the current
	# directory is not one (e.g. running from the repo root), look for the
	# canonical/local build dirs that carry CTestTestfile.cmake.
	if [ ! -f ./CTestTestfile.cmake ]; then
		BUILD_DIR=""
		for d in "@ci-cmake-build" "@cmake-build" "@cmake-stochastic-build" build cmake-build-* p1-gcc p1-clang; do
			if [ -f "$d/CTestTestfile.cmake" ]; then
				BUILD_DIR="$d"
				break
			fi
		done
		if [ -n "$BUILD_DIR" ]; then
			CMD="ctest --test-dir \"$BUILD_DIR\" -L '${LABELS}' -LE 'ut\.heavy|chk'"
			echo "select-tests: using build dir '$BUILD_DIR'" >&2
		else
			echo "select-tests: no CTestTestfile.cmake here and no build dir found — run from your build dir (or pass --test-dir)" >&2
			exit 2
		fi
	fi
	echo "select-tests: running: $CMD" >&2
	# shellcheck disable=SC2086
	eval "$CMD"
fi