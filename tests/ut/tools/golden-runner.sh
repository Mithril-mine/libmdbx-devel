#!/usr/bin/env bash
# Golden characterization tests for the src/tools CLI utilities (TASK-45).
#
# Each case = (fixture setup) + (tool args) -> {stdout, stderr, exit-code}.
# The current C-tool behavior is the CONTRACT (spec-*.md, Phase A): do NOT fix
# the tools here, only record the behavior. The same suite is reused in Phase D
# to validate the rewritten C++ tools 1:1.
#
# Usage:
#   golden-runner.sh <tool> <tool-binary> <workdir> [--update] [<case>...]
#     tool      - chk|copy|defrag|drop|dump|load|stat
#     workdir   - scratch dir (created by the runner)
#     --update  - (re)write golden files instead of comparing
#     <case>... - run only the listed cases (default: all)
#
# Golden files live next to this script under golden/<tool>/<case>.{out,err,rc}
# and MUST be committed. Deterministic: fixtures are built from
# fixtures/base.dump via mdbx_load (no stochastic mdbx_test runs).
set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
FIXTURES="$SELF_DIR/fixtures"
GOLDEN="$SELF_DIR/golden"

TOOL="${1:?tool}"; shift
BIN="${1:?tool binary}"; shift
WORK="${1:?workdir}"; shift
UPDATE=0
if [ "${1:-}" = "--update" ]; then UPDATE=1; shift; fi
FILTER="$*"   # empty = all cases

# Tool binaries needed to build fixtures / for copy/load cross-checks.
LOAD_BIN="$(dirname "$BIN")/mdbx_load"
DUMP_BIN="$(dirname "$BIN")/mdbx_dump"
CHK_BIN="$(dirname "$BIN")/mdbx_chk"

pass=0; fail=0
declare -a FAILED_CASES

log() { printf '  %s\n' "$*"; }

# normalize volatile fields (version/build info, elapsed time, absolute paths)
# so golden files stay stable across builds, machines and runs.
normalize() {
  sed -E \
    -e 's/v[0-9]+\.[0-9]+\.[0-9]+(-[0-9]+)?-g[0-9a-f]+(-dirty)? \(.*\)/vVERSION (BUILD, T-HASH)/' \
    -e 's/T-[0-9a-f]{40}/T-HASH/' \
    -e 's/elapsed [0-9.]+ seconds/elapsed SECONDS/' \
    -e 's/dxb-id [0-9a-f]+-[0-9a-f]+/dxb-id <ID>/' \
    -e 's/^mdbx_[a-z_]* version [0-9.]+$/mdbx_TOOL version VERSION/' \
    -e 's/^ - source: .*/ - source: <SOURCE>/' \
    -e 's/^ - anchor: .*/ - anchor: <ANCHOR>/' \
    -e 's/^ - build: .*/ - build: <BUILD>/' \
    -e 's#^ - flags: .*# - flags: <FLAGS>#' \
    -e 's#^ - options: .*# - options: <OPTIONS>#' \
    -e 's#[^ ]*/(base\.db|copy\.db|bad\.db|compact\.db|out\.img|reload\.db|ver\.db|bad\.dump|exists\.db|named\.db)#/\1#g' \
    -e '/troika:/d' \
    -e 's#^usage: [^ ]*/mdbx_[a-z]+ #usage: mdbx_BIN #' \
    -e 's#^[^ ]*/mdbx_(load|dump|copy|chk|stat|drop|defrag): #mdbx_\1: #'
}

# rc-only case: exit code is the contract, output is volatile (e.g. reader
# table with live pids/threads).
run_case_rc() {
  local name="$1" want_rc="$2"; shift 2
  [ "$1" = "--" ] && shift
  if [ -n "$FILTER" ]; then
    local hit=0 c
    for c in $FILTER; do [ "$c" = "$name" ] && hit=1; done
    [ "$hit" = 0 ] && return
  fi
  ( "$@" ) >/dev/null 2>&1
  local rc=$?
  if [ "$rc" = "$want_rc" ]; then
    pass=$((pass+1)); log "PASS $TOOL/$name (rc-only)"
  else
    fail=$((fail+1)); FAILED_CASES+=("$TOOL/$name")
    log "FAIL $TOOL/$name (rc=$rc, want=$want_rc)"
  fi
}

# run_case <name> <expected-exit> -- cmd args...
run_case() {
  local name="$1" want_rc="$2"; shift 2
  [ "$1" = "--" ] && shift
  if [ -n "$FILTER" ]; then
    local hit=0 c
    for c in $FILTER; do [ "$c" = "$name" ] && hit=1; done
    [ "$hit" = 0 ] && return
  fi
  local d="$WORK/out/$name"
  mkdir -p "$d"
  ( "$@" ) >"$d/stdout" 2>"$d/stderr"   # subshell: survive child crashes
  local rc=$?
  normalize <"$d/stdout" >"$d/stdout.norm"
  normalize <"$d/stderr" >"$d/stderr.norm"
  if [ "$UPDATE" = 1 ]; then
    mkdir -p "$GOLDEN/$TOOL"
    cp "$d/stdout.norm" "$GOLDEN/$TOOL/$name.out"
    cp "$d/stderr.norm" "$GOLDEN/$TOOL/$name.err"
    printf '%s\n' "$rc" >"$GOLDEN/$TOOL/$name.rc"
    log "golden $TOOL/$name (rc=$rc)"
    return
  fi
  local ok=1
  [ -f "$GOLDEN/$TOOL/$name.out" ] || { ok=0; log "MISSING golden $TOOL/$name.out"; }
  normalize <"$GOLDEN/$TOOL/$name.out" >"$d/golden.out.norm"
  normalize <"$GOLDEN/$TOOL/$name.err" >"$d/golden.err.norm"
  diff -q "$d/stdout.norm" "$d/golden.out.norm" >/dev/null 2>&1 || ok=0
  diff -q "$d/stderr.norm" "$d/golden.err.norm" >/dev/null 2>&1 || ok=0
  local want_rc_file="$GOLDEN/$TOOL/$name.rc"
  [ -f "$want_rc_file" ] && want_rc="$(cat "$want_rc_file")"
  [ "$rc" = "$want_rc" ] || ok=0
  if [ "$ok" = 1 ]; then
    pass=$((pass+1)); log "PASS $TOOL/$name"
  else
    fail=$((fail+1)); FAILED_CASES+=("$TOOL/$name")
    log "FAIL $TOOL/$name (rc=$rc, want=$want_rc); diff:"
    diff "$d/stdout.norm" "$GOLDEN/$TOOL/$name.out" 2>&1 | head -8 | sed 's/^/      /'
  fi
}

# fresh_base <dir>  -> creates <dir>/base.db fixture (Main + meta + duptbl)
# with FIXED geometry (64 MiB lower=upper) so dump/stat output is machine-independent.
fresh_base() {
  local d="$1"
  rm -rf "$d"; mkdir -p "$d"
  "$LOAD_BIN" -nq -f "$FIXTURES/base.dump" -G 67108864:67108864:0:0:4096 "$d/base.db" 2>/dev/null
  [ -f "$d/base.db" ] || { echo "fixture build failed" >&2; exit 1; }
}

# corrupt_db <src> <dst>: flip a byte in an allocated data page (offset 20480
# reliably trips mdbx_chk for the 64KB fixture DB).
corrupt_db() {
  local src="$1" dst="$2"
  cp -f "$src" "$dst"
  printf '\xff' | dd of="$dst" bs=1 seek=20480 count=1 conv=notrunc 2>/dev/null
}

# named_base <dir>: deterministic DB with a named table via mdbx_test (fixed seed).
MDBX_TEST_BIN="$(dirname "$BIN")/mdbx_test"
named_base() {
  local d="$1"
  rm -rf "$d"; mkdir -p "$d"
  "$MDBX_TEST_BIN" --nops=300 --prng-seed=42 --repeat=1 --pathname="$d/named.db" \
    --dont-cleanup-after --append --table=+data.fixed >/dev/null 2>&1
  [ -f "$d/named.db" ] || { echo "named fixture build failed" >&2; exit 1; }
}

############################################# mdbx_chk
case_chk_clean() {
  local d="$WORK/chk_clean"; fresh_base "$d"
  run_case clean 0 -- "$BIN" "$d/base.db"
}
case_chk_verbose() {
  local d="$WORK/chk_v"; fresh_base "$d"
  run_case verbose 0 -- "$BIN" -v "$d/base.db"
}
case_chk_specific_table() {
  local d="$WORK/chk_s"; named_base "$d"
  run_case specific_table 0 -- "$BIN" -s data "$d/named.db"
}
case_chk_corrupt() {
  local d="$WORK/chk_c"; fresh_base "$d"
  corrupt_db "$d/base.db" "$d/bad.db"
  run_case corrupt 2 -- "$BIN" "$d/bad.db"
}
case_chk_quiet_corrupt() {
  local d="$WORK/chk_q"; fresh_base "$d"
  corrupt_db "$d/base.db" "$d/bad.db"
  run_case quiet_corrupt 2 -- "$BIN" -q "$d/bad.db"
}
case_chk_usage() {
  run_case usage 5 -- "$BIN"
}
case_chk_version() {
  run_case version 0 -- "$BIN" -V
}
# case_chk_ignore_order SKIPPED: mdbx_chk -i segfaults on any DB containing named
# tables (assert 'tbl->cookie', chk_handle_kv():970) — see known-defects.md D1.

############################################# mdbx_copy
case_copy_basic() {
  local d="$WORK/copy_b"; fresh_base "$d"
  run_case basic 0 -- "$BIN" "$d/base.db" "$d/copy.db"
}
case_copy_compact() {
  local d="$WORK/copy_c"; fresh_base "$d"
  run_case compact 0 -- "$BIN" -c "$d/base.db" "$d/compact.db"
}
case_copy_force() {
  local d="$WORK/copy_f"; fresh_base "$d"
  touch "$d/exists.db"
  run_case force 0 -- "$BIN" -f "$d/base.db" "$d/exists.db"
}
case_copy_stdout() {
  local d="$WORK/copy_o"; fresh_base "$d"
  # tool writes the DB image to stdout; redirect it to a file, then verify the
  # produced image with mdbx_chk (its verdict is the observable contract).
  ( "$BIN" "$d/base.db" ) > "$d/out.img" 2>/dev/null
  run_case stdout 0 -- "$CHK_BIN" "$d/out.img"
}
case_copy_usage() {
  run_case usage 1 -- "$BIN"
}

############################################# mdbx_defrag
case_defrag_quick() {
  local d="$WORK/defrag_1"; fresh_base "$d"
  run_case quick 0 -- "$BIN" -1 "$d/base.db"
}
case_defrag_ratio() {
  local d="$WORK/defrag_r"; fresh_base "$d"
  run_case ratio 0 -- "$BIN" -f 100 "$d/base.db"
}
case_defrag_timeout() {
  local d="$WORK/defrag_t"; fresh_base "$d"
  run_case timeout 0 -- "$BIN" -t 2 "$d/base.db"
}

############################################# mdbx_drop
case_drop_empty() {
  local d="$WORK/drop_e"; fresh_base "$d"
  run_case empty 0 -- "$BIN" "$d/base.db"
}
case_drop_delete() {
  local d="$WORK/drop_d"; fresh_base "$d"
  run_case delete 0 -- "$BIN" -d "$d/base.db"
}
case_drop_named() {
  local d="$WORK/drop_n"; fresh_base "$d"
  run_case named 0 -- "$BIN" -s meta "$d/base.db"
}

############################################# mdbx_dump
case_dump_main() {
  local d="$WORK/dump_m"; fresh_base "$d"
  run_case main 0 -- "$BIN" "$d/base.db"
}
case_dump_all() {
  local d="$WORK/dump_a"; fresh_base "$d"
  run_case all 0 -- "$BIN" -a "$d/base.db"
}
case_dump_named() {
  local d="$WORK/dump_s"; fresh_base "$d"
  run_case named 0 -- "$BIN" -s meta "$d/base.db"
}
case_dump_list() {
  local d="$WORK/dump_l"; fresh_base "$d"
  run_case list 0 -- "$BIN" -l "$d/base.db"
}
case_dump_print() {
  local d="$WORK/dump_p"; fresh_base "$d"
  run_case print 0 -- "$BIN" -p "$d/base.db"
}
case_dump_concise_dups() {
  local d="$WORK/dump_c"; fresh_base "$d"
  run_case concise_dups 0 -- "$BIN" -c -s duptbl "$d/base.db"
}

############################################# mdbx_load
case_load_roundtrip() {
  local d="$WORK/load_rt"; fresh_base "$d"
  "$DUMP_BIN" -a "$d/base.db" > "$d/out.dump" 2>/dev/null
  run_case roundtrip 0 -- "$BIN" -nf "$d/out.dump" "$d/reload.db"
}
case_load_stdin() {
  local d="$WORK/load_si"; rm -rf "$d"; mkdir -p "$d"
  ( "$BIN" -nf "$d/base.db" ) < "$FIXTURES/base.dump" > "$d/stdout" 2> "$d/stderr"
  local rc=$?
  local name=stdin
  # Issue #50: stdin-by-default must load the dump into the dbpath (the last
  # positional arg), not mis-bind it to '-f'. The loaded database must be
  # identical to one loaded from the same stream via '-f file'.
  local data_ok=1
  "$LOAD_BIN" -nq -f "$FIXTURES/base.dump" "$d/ref.db" 2>/dev/null
  if "$DUMP_BIN" -q "$d/base.db" >"$d/reloaded.dump" 2>/dev/null &&
     "$DUMP_BIN" -q "$d/ref.db" >"$d/ref.dump" 2>/dev/null; then
    normalize <"$d/reloaded.dump" >"$d/reloaded.norm"
    normalize <"$d/ref.dump" >"$d/ref.norm"
    cmp -s "$d/reloaded.norm" "$d/ref.norm" || data_ok=0
  else
    data_ok=0
  fi
  if [ "$UPDATE" = 1 ]; then
    mkdir -p "$GOLDEN/$TOOL"
    normalize <"$d/stdout" >"$GOLDEN/$TOOL/$name.out"
    normalize <"$d/stderr" >"$GOLDEN/$TOOL/$name.err"
    printf '%s\n' "$rc" >"$GOLDEN/$TOOL/$name.rc"
    log "golden $TOOL/$name (rc=$rc, data_ok=$data_ok)"
    return
  fi
  local ok=1
  normalize <"$d/stdout" >"$d/stdout.norm"; normalize <"$d/stderr" >"$d/stderr.norm"
  normalize <"$GOLDEN/$TOOL/$name.out" >"$d/golden.out.norm"
  normalize <"$GOLDEN/$TOOL/$name.err" >"$d/golden.err.norm"
  diff -q "$d/stdout.norm" "$d/golden.out.norm" >/dev/null 2>&1 || ok=0
  diff -q "$d/stderr.norm" "$d/golden.err.norm" >/dev/null 2>&1 || ok=0
  local want_rc="$(cat "$GOLDEN/$TOOL/$name.rc")"
  [ "$rc" = "$want_rc" ] || ok=0
  [ "$data_ok" = 1 ] || { ok=0; log "  $TOOL/$name: loaded data mismatch (issue #50 regression)"; }
  if [ "$ok" = 1 ]; then pass=$((pass+1)); log "PASS $TOOL/$name"
  else fail=$((fail+1)); FAILED_CASES+=("$TOOL/$name"); log "FAIL $TOOL/$name (rc=$rc want=$want_rc)"; fi
}
case_load_bad_mapsize() {
  local d="$WORK/load_bm"; fresh_base "$d"
  printf 'VERSION=3\nformat=bytevalue\ntype=btree\nmapsize=99999999999999999999\nHEADER=END\nDATA=END\n' > "$d/bad.dump"
  run_case bad_mapsize 1 -- "$BIN" -nf "$d/bad.dump" "$d/bad.db"
}
case_load_bad_version() {
  local d="$WORK/load_bv"; mkdir -p "$d"
  printf 'VERSION=2\nHEADER=END\nDATA=END\n' > "$d/ver.dump"
  run_case bad_version 1 -- "$BIN" -nf "$d/ver.dump" "$d/ver.db"
}
case_load_purge() {
  local d="$WORK/load_p"; fresh_base "$d"
  run_case purge 0 -- "$BIN" -pnf "$FIXTURES/base.dump" "$d/base.db"
}

############################################# mdbx_stat
case_stat_main() {
  local d="$WORK/stat_m"; fresh_base "$d"
  run_case main 0 -- "$BIN" "$d/base.db"
}
case_stat_envinfo() {
  local d="$WORK/stat_e"; fresh_base "$d"
  run_case envinfo 0 -- "$BIN" -e "$d/base.db"
}
case_stat_all() {
  local d="$WORK/stat_a"; fresh_base "$d"
  run_case all 0 -- "$BIN" -a "$d/base.db"
}
case_stat_named() {
  local d="$WORK/stat_s"; fresh_base "$d"
  run_case named 0 -- "$BIN" -s meta "$d/base.db"
}
case_stat_readers() {
  local d="$WORK/stat_r"; fresh_base "$d"
  # reader table contains live pid/thread/txnid -> rc-only contract
  run_case_rc readers 0 -- "$BIN" -r "$d/base.db"
}
case_stat_gc() {
  local d="$WORK/stat_g"; fresh_base "$d"
  run_case gc 0 -- "$BIN" -f "$d/base.db"
}

# dispatch per tool
tool_funcs="$(declare -F | awk '{print $3}' | grep "^case_${TOOL}_")"
for f in $tool_funcs; do
  case "$f" in
    case_chk_clean) case_chk_clean;;
    case_chk_verbose) case_chk_verbose;;
    case_chk_specific_table) case_chk_specific_table;;
    case_chk_corrupt) case_chk_corrupt;;
    case_chk_quiet_corrupt) case_chk_quiet_corrupt;;
    case_chk_usage) case_chk_usage;;
    case_chk_version) case_chk_version;;
    case_copy_basic) case_copy_basic;;
    case_copy_compact) case_copy_compact;;
    case_copy_force) case_copy_force;;
    case_copy_stdout) case_copy_stdout;;
    case_copy_usage) case_copy_usage;;
    case_defrag_quick) case_defrag_quick;;
    case_defrag_ratio) case_defrag_ratio;;
    case_defrag_timeout) case_defrag_timeout;;
    case_drop_empty) case_drop_empty;;
    case_drop_delete) case_drop_delete;;
    case_drop_named) case_drop_named;;
    case_dump_main) case_dump_main;;
    case_dump_all) case_dump_all;;
    case_dump_named) case_dump_named;;
    case_dump_list) case_dump_list;;
    case_dump_print) case_dump_print;;
    case_dump_concise_dups) case_dump_concise_dups;;
    case_load_roundtrip) case_load_roundtrip;;
    case_load_stdin) case_load_stdin;;
    case_load_bad_mapsize) case_load_bad_mapsize;;
    case_load_bad_version) case_load_bad_version;;
    case_load_purge) case_load_purge;;
    case_stat_main) case_stat_main;;
    case_stat_envinfo) case_stat_envinfo;;
    case_stat_all) case_stat_all;;
    case_stat_named) case_stat_named;;
    case_stat_readers) case_stat_readers;;
    case_stat_gc) case_stat_gc;;
  esac
done

echo
echo "== $TOOL: pass=$pass fail=$fail"
if [ "$fail" != 0 ]; then
  printf 'failed: %s\n' "${FAILED_CASES[@]}"
  exit 1
fi
exit 0