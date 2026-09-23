#!/usr/bin/env bash
# Local cell runner for the testing-infra-v2 registry (TASK-28 stage 10).
#
# Usage:
#   tests/ci/run-cell.sh <cell-id> [--build-dir <dir>]
#
# Parses tests/ci/config.json, configures+builds+tests exactly one "cell"
# (a build configuration mirrored from the legacy workflow matrices). Pure
# CMake/CTest, no ci.sh. Exit codes:
#   0  success (or build_only cell built OK)
#   1  build/test failure (rc propagated)
#   2  unknown cell id / usage error
#
# Toolchain env from cell.env is exported before cmake. Android (build_only)
# cells need ANDROID_NDK_HOME / NDK_PATH set by the caller. Windows/macOS cells
# are only runnable on their own platforms (this script is for local repro of
# Linux-capable cells; the rest are validated via addressable GitHub dispatch).
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REGISTRY="${REPO_ROOT}/tests/ci/config.json"

# Expand ${VAR} placeholders against the current environment (no command
# substitution, no $VAR shorthand — only ${NAME}, matching the registry usage).
expand_vars() {
	local s="$1" out="" rest="$1" m key val
	while [[ "$rest" =~ \$\{([A-Za-z_][A-Za-z0-9_]*)\} ]]; do
		m="${BASH_REMATCH[0]}"
		key="${BASH_REMATCH[1]}"
		val="${!key:-}"
		out+="${rest%%$m*}$val"
		rest="${rest#*$m}"
	done
	out+="$rest"
	printf '%s' "$out"
}

CELL_ID=""
BUILD_DIR=""

while [ $# -gt 0 ]; do
	case "$1" in
	--build-dir)
		BUILD_DIR="$2"
		shift 2
		;;
	*)
		CELL_ID="$1"
		shift
		;;
	esac
done

if [ -z "$CELL_ID" ]; then
	echo "usage: $0 <cell-id> [--build-dir <dir>]" >&2
	exit 2
fi

if [ ! -f "$REGISTRY" ]; then
	echo "error: registry not found: $REGISTRY" >&2
	exit 2
fi

CELL_JSON="$(python3 -c "
import json, sys
with open('${REGISTRY}') as fh:
    cfg = json.load(fh)
cid = '${CELL_ID}'
for c in cfg['cells']:
    if c['id'] == cid:
        json.dump(c, sys.stdout)
        sys.exit(0)
sys.exit(3)
")" || {
	rc=$?
	if [ "$rc" = "3" ]; then
		echo "error: unknown cell id '$CELL_ID' (see tests/ci/config.json)" >&2
	else
		echo "error: failed to parse ${REGISTRY}" >&2
	fi
	exit 2
}

get() { python3 -c "import json,sys; c=json.loads('''${CELL_JSON}'''); v=c.get('$1'); print('' if v is None else json.dumps(v) if isinstance(v,(list,dict)) else v)"; }

RUNS_ON="$(get runs-on)"
BUILD_ONLY="$(get build_only)"
CELL_NAME="$(get id)"

if [ -z "$BUILD_DIR" ]; then
	BUILD_DIR="${REPO_ROOT}/@ci-cmake-build/${CELL_NAME}"
fi

echo "==> run-cell: ${CELL_NAME} (runs-on: ${RUNS_ON})"
echo "==> build dir: ${BUILD_DIR}"
mkdir -p "$BUILD_DIR"

# --- toolchain env ----------------------------------------------------------
ENV_JSON="$(python3 -c "import json,sys; c=json.loads('''${CELL_JSON}'''); json.dump(c.get('env') or {}, sys.stdout)")"
if [ "$ENV_JSON" != "{}" ]; then
	while IFS= read -r line; do
		[ -z "$line" ] && continue
		key="${line%%=*}"
		value="${line#*=}"
		value="$(expand_vars "$value")"
		if [ "$key" = "PATH" ]; then
			export PATH="$value:$PATH"
		else
			export "$key=$value"
		fi
	done < <(python3 -c "
import json
env = json.loads('''${ENV_JSON}''')
for k, v in env.items():
    print(f'{k}={v}')
")
fi

# --- cmake args (each registry entry is 'flag|value' or a plain '-D...' arg) -
CMAKE_ARGS=()
BUILD_CONFIG=""
while IFS= read -r line; do
	[ -z "$line" ] && continue
	line="$(expand_vars "$line")"
	if [[ "$line" =~ ^-DCMAKE_BUILD_TYPE=(.*)$ ]]; then
		BUILD_CONFIG="${BASH_REMATCH[1]}"
	fi
	flag="${line%%|*}"
	rest="${line#*|}"
	if [ "$rest" != "$line" ]; then
		CMAKE_ARGS+=("$flag" "$rest")
	else
		CMAKE_ARGS+=("$line")
	fi
done < <(python3 -c "
import json
c = json.loads('''${CELL_JSON}''')
for a in c.get('cmake') or []:
    print(a)
")

# Deterministic generator selection: only default to Ninja when the cell does
# not pin its own generator/platform/toolset (legacy ci.sh semantics).
if command -v ninja >/dev/null 2>&1; then
	case " ${CMAKE_ARGS[*]} " in
	*' -G '*|*' -A '*|*' -T '*)
		GENERATOR=""
		;;
	*)
		GENERATOR="-G Ninja"
		;;
	esac
else
	GENERATOR=""
fi

echo "==> cmake configure: ${CMAKE_ARGS[*]}"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" $GENERATOR "${CMAKE_ARGS[@]}" || exit 1
echo "==> cmake build"
if [ -n "$BUILD_CONFIG" ]; then
	cmake --build "$BUILD_DIR" --parallel --config "$BUILD_CONFIG" || exit 1
else
	cmake --build "$BUILD_DIR" --parallel || exit 1
fi

if [ "$BUILD_ONLY" = "True" ]; then
	echo "==> build_only cell: skipping ctest (${CELL_NAME})"
	exit 0
fi

# --- ctest ------------------------------------------------------------------
CTEST_RUN=""
CTEST_EXCL=""
while IFS= read -r line; do
	[ -z "$line" ] && continue
	CTEST_RUN="$line"
done < <(python3 -c "
import json
c = json.loads('''${CELL_JSON}''')
print(c.get('ctest') and c['ctest'].get('regex') or '')
")
while IFS= read -r line; do
	[ -z "$line" ] && continue
	CTEST_EXCL="$line"
done < <(python3 -c "
import json
c = json.loads('''${CELL_JSON}''')
print(c.get('ctest') and c['ctest'].get('exclude') or '')
")

CTEST_CMD=(ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel 3 --schedule-random --no-tests=error)
[ -n "$BUILD_CONFIG" ] && CTEST_CMD+=(-C "$BUILD_CONFIG")
[ -n "$CTEST_RUN" ] && CTEST_CMD+=(-R "$CTEST_RUN")
[ -n "$CTEST_EXCL" ] && CTEST_CMD+=(-E "$CTEST_EXCL")
echo "==> ctest: ${CTEST_CMD[*]}"
GTEST_SHUFFLE=1 GTEST_RUNTIME_LIMIT=99 MALLOC_CHECK_=7 MALLOC_PERTURB_=42 "${CTEST_CMD[@]}"
rc=$?
if [ "$rc" != "0" ]; then
	echo "==> cell ${CELL_NAME} FAILED (rc=$rc); LastTest.log: ${BUILD_DIR}/Testing/Temporary/LastTest.log" >&2
fi
exit "$rc"