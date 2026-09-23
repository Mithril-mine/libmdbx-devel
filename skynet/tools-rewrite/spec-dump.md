# Spec: mdbx_dump — dump DB content in BDB-compatible format

Source: `src/tools/dump.c` (505 lines). Man page: `src/man1/mdbx_dump.1`.

## CLI

```
usage: mdbx_dump [-V] [-q] [-c] [-f file] [-l] [-p] [-r] [-a|-s table] [-u|U] dbpath
```

getopt optstring (`dump.c:249`): `"uUaf:lnps:Vrcq"`

| Opt | Arg | Meaning |
|---|---|---|
| `-V` | — | version banner, exit 0 |
| `-q` | — | quiet |
| `-c` | — | concise mode (dups on one line); **incompatible with BDB/LMDB** |
| `-f file` | yes | write to file instead of stdout |
| `-l` | — | list named tables and exit |
| `-p` | — | printable characters mode (`format=print`) |
| `-r` | — | rescue mode (ignore errors, dump corrupted DB) |
| `-a` | — | dump main DB and all named tables |
| `-s table` | yes | dump only the named table |
| `-u` / `-U` | — | warmup / warmup+lock |
| `-n` | — | legacy `MDBX_NOSUBDIR` flag, accepted as a silent no-op (`case 'n': break;`, dump.c:287) |

Default: dump only the main DB. `-a` and `-s` are mutually exclusive.

## Format (BDB-compatible, `format=bytevalue` default)

Header (`dump.c:105-147`):
```
VERSION=3
geometry=l<lower>,u<upper>,s<shrink>,g<grow>      (only with -a, when upper != lower)
mapsize=<bytes>                                   (only with -a)
canary=v<x>,x<x>,y<x>,z<x>                        (only with -a, if canary.v)
format=print|bytevalue
database=<name>                                   (named tables only)
type=btree
db_pagesize=<bytes>
duplicates=0|1
<dbflag>=1                                        (reversekey/dupsort/integerkey/dupfix/integerdup/reversedup)
sequence=<uint64>                                 (if non-zero)
HEADER=END
```
Items (`dump.c:56-82`, `162-182`):
```
 <key-hex> <data-hex>\n                          (bytevalue: hex per byte)
 <key> <data>\n                                  (print: literal char if isprint() && != '\\', else `\` + 2 hex digits)
```
- In `-c` concise mode with DUPSORT, duplicates share one line:
  ` <key> <data1> <data2> ...` (space-separated).
- Footer: `DATA=END`.

Encoding (`dumpval`, `dump.c:56-75`): each byte → 2 hex digits, or in print
mode a literal char if `isprint && != '\\'`; any other byte (including `\`,
0x5C) is emitted as `\` + its 2 hex digits (e.g. backslash → `\5C`, never a
doubled `\\`); a single leading space before each value; newline after each
value/line.

## Exit codes

- 0 success; 1 (`EXIT_FAILURE`) on error/usage (`dump.c:504`).
- Errors to stderr; `-q` silences.

## Behavior notes

- Opens read-only; rescue adds `MDBX_VALIDATION | MDBX_EXCLUSIVE` and uses
  `mdbx_cursor_ignord()` (ignore wrong order) + `mdbx_txn_reset/renew` restart
  hack on corrupt tables (`dump.c:449-458`).
- `-a` enumerates sub-DBs via cursor over MAIN_DBI keys
  (`mdbx_cursor_get MDBX_NEXT_NODUP`), skipping keys containing NUL
  (`dump.c:401-421`), opening each with `mdbx_dbi_open_ex(..., ACCEDE)`.
- `-l` lists table names and exits.
- Warmup support.

## Rewrite notes

- The format must stay byte-identical for BDB/LMDB interop and `tests/dump-load.sh`
  round-trips.
- I/O currently raw `printf/putchar` — a buffered writer + `std::ostream` must
  preserve exact bytes (esp. print-mode escaping).