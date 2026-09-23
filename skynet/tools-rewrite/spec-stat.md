# Spec: mdbx_stat — print database statistics

Source: `src/tools/stat.c` (459 lines). Man page: `src/man1/mdbx_stat.1`.

## CLI

```
usage: mdbx_stat [-V] [-q] [-e] [-f[f[f]]] [-r[r]] [-a|-s table] dbpath
```

getopt optstring (`stat.c:166`): `"Vqpaefnrs:"`

| Opt | Arg | Meaning |
|---|---|---|
| `-V` | — | version banner, exit 0 |
| `-q` | — | quiet |
| `-p` | — | show page-operation statistics for current session |
| `-e` | — | show whole-DB (environment) info |
| `-f` | repeatable | show GC info (repeat for more detail) |
| `-r` | repeatable | show reader table |
| `-a` | — | stat of main DB and all tables |
| `-s table` | yes | stat of only the named table |
| `-n` | — | legacy `MDBX_NOSUBDIR` flag, accepted as a silent no-op (`case 'n': break;`, stat.c:205) |

Default: stat of the main DB only.

## Output sections (`stat.c:39-44, 67-89, 139-148, 282-318`)

- Per-table block:
  ```
  Status of <name>
    Pagesize: <u>
    Tree depth: <u>
    Branch pages: <u64>
    Leaf pages: <u64>
    Large/Overflow pages: <u64>
    Entries: <u64>
  ```
- `-p` → "Page Operations (for current session):" with New/CoW/Clone/Split/
  Merge/Spill/Unspill/WOP/PreFault/mInCore/mSync/fSync counters
  (`MDBX_pageop_stat`, `stat.c:282-303`).
- `-e` → "Environment Info": pagesize, dynamic datafile range, current
  mapsize/datafile, max readers/tables, etc. (`stat.c:308-318`).
- `-f` → GC info via `mdbx_gc_info()` + `mdbx_ratio2percents()`.
- `-r` → "Reader Table" with pid/thread/txnid/lag/used/retained
  (`mdbx_reader_list` / `mdbx_reader_check`, `stat.c:67-89`).
- `-a` enumerates tables via `mdbx_enumerate_tables()`.

## Exit codes

- 0 success; 1 (`EXIT_FAILURE`) on error/usage (`stat.c:187,247`).

## stdout/stderr contract

- All stat blocks to **stdout**; errors to stderr; `-q` silences chatter.

## Rewrite notes

- Pure read-only tool; mostly formatting. The many `printf`s map cleanly to a
  small formatting helper (column alignment must be preserved for scripts/tests).