#!/usr/bin/env bash
# Regression test for MDBX_ATTRIBUTE_NO_SANITIZE_ADDRESS (issue #62).
#
# Under __SANITIZE_ADDRESS__ the macro must expand to a spelling the active
# compiler accepts: __declspec(no_sanitize_address) for MSVC (cl.exe does not
# parse __attribute__), and __attribute__((__no_sanitize_address__,
# __noinline__)) for GCC/Clang. Without ASan it must expand to the ELSEWISE
# argument.
#
# MSVC itself is not available on Linux, so the real src/preface.h is
# preprocessed with -D_MSC_VER to select the MSVC branch; empty stub headers
# stand in for the MSVC-only system headers so preprocessing runs on any host.
set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$SELF_DIR/.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

: >"$TMP/intrin.h"
: >"$TMP/assert.h"
: >"$TMP/windows.h"
: >"$TMP/crtdbg.h"

cat >"$TMP/probe.c" <<'EOF'
#include "preface.h"
MDBX_ATTRIBUTE_NO_SANITIZE_ADDRESS(MDBX_NOTHING) void probe(void) {}
EOF

probe() { gcc -I "$SRC/src" -isystem "$TMP" -E -P "$@"; }

pass=0
fail=0
expect() {
  # expect <name> <pattern> [--not] -- cmd args...
  local name="$1" pattern="$2"; shift 2
  local want=1 invert=0
  [ "$1" = "--not" ] && { invert=1; shift; }
  [ "$1" = "--" ] && shift
  local out
  out="$("$@" 2>/dev/null)"
  if printf '%s\n' "$out" | grep -q "$pattern"; then
    [ "$invert" = 0 ] && want=0
  else
    [ "$invert" = 1 ] && want=0
  fi
  if [ "$want" = 0 ]; then
    pass=$((pass + 1)); echo "  PASS $name"
  else
    fail=$((fail + 1)); echo "  FAIL $name"
    printf '%s\n' "$out" | grep -E 'probe|__attribute__|__declspec' | tail -2 | sed 's/^/       /'
  fi
}

echo "  check MDBX_ATTRIBUTE_NO_SANITIZE_ADDRESS expansion:"

# MSVC + ASan: must be __declspec(no_sanitize_address), never __attribute__.
# `-U__clang__` is required because clang masquerading as gcc (e.g. on macOS)
# predefines `__clang__`, which would select the __attribute__ branch despite
# -D_MSC_VER (the preface.h MSVC branch requires !defined(__clang__)).
expect msvc-asan '__declspec(no_sanitize_address)' -- \
  probe -D__SANITIZE_ADDRESS__ -D_MSC_VER=1930 -D_MSC_FULL_VER=193833135 -U__clang__ "$TMP/probe.c"
expect msvc-asan-not-attribute '__attribute__((__no_sanitize_address__' --not -- \
  probe -D__SANITIZE_ADDRESS__ -D_MSC_VER=1930 -D_MSC_FULL_VER=193833135 -U__clang__ "$TMP/probe.c"

# GCC/Clang + ASan: must be __attribute__((__no_sanitize_address__, __noinline__)).
expect gcc-asan '__attribute__((__no_sanitize_address__, __noinline__))' -- \
  probe -D__SANITIZE_ADDRESS__ "$TMP/probe.c"
expect gcc-asan-not-declspec '__declspec(no_sanitize_address)' --not -- \
  probe -D__SANITIZE_ADDRESS__ "$TMP/probe.c"

# Without ASan: ELSEWISE (nothing), no attribute spelling at all.
expect no-asan 'void probe(void) {}' -- \
  probe "$TMP/probe.c"
expect no-asan-no-attr '__attribute__|__declspec' --not -- \
  probe "$TMP/probe.c"

echo
echo "== check-asan-macro: pass=$pass fail=$fail"
[ "$fail" = 0 ] || exit 1
exit 0
