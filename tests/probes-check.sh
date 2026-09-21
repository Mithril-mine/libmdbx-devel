#!/bin/sh
# Check that a built libmdbx artifact carries the expected USDT/DTrace probes.
# Usage: probes-check.sh <elffile>
# No root and no systemtap runtime required: inspects .note.stapsdt via readelf.
# Override the expected set with MDBX_EXPECTED_PROBES="a b c".

set -u

elf="${1:-}"
if [ -z "$elf" ] || [ ! -r "$elf" ]; then
  echo "usage: $0 <elffile>" >&2
  exit 2
fi

expected="${MDBX_EXPECTED_PROBES:-panic alloc__source commit__gc_update__begin commit__gc_update__end spill__trigger txn__write_started}"

if ! command -v readelf >/dev/null 2>&1; then
  echo "probes-check: readelf not found" >&2
  exit 2
fi

actual="$(readelf -n "$elf" 2>/dev/null | sed -n 's/^[[:space:]]*Name: //p')"

missing=0
for probe in $expected; do
  if ! printf '%s\n' "$actual" | grep -qx "$probe"; then
    echo "MISSING: mdbx:$probe in $elf" >&2
    missing=1
  fi
done

if [ "$missing" -eq 0 ]; then
  count=$(printf '%s\n' "$actual" | wc -l)
  echo "probes-check: OK — $(printf '%s\n' "$actual" | tr '\n' ' ') (${count} markers)"
  exit 0
fi

echo "probes-check: FAILED — $elf has: $(printf '%s\n' "$actual" | tr '\n' ' ')" >&2
exit 1