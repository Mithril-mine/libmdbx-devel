# Spec: mdbx_drop — empty/delete a database table

Source: `src/tools/drop.c` (196 lines). Man page: `src/man1/mdbx_drop.1`.

## CLI

```
usage: mdbx_drop [-V] [-q] [-d] [-s name] dbpath
```

getopt optstring (`drop.c:86`): `"ds:nqV"`

| Opt | Arg | Meaning |
|---|---|---|
| `-V` | — | version banner, exit 0 |
| `-q` | — | quiet |
| `-d` | — | delete the DB entirely (not just empty it) |
| `-s name` | yes | drop the specified named table; default = empty MAIN_DBI |
| `-n` | — | legacy `MDBX_NOSUBDIR` flag, accepted as a silent no-op (`case 'n': break;`, drop.c:110) |

## Exit codes

- 0 success, 1 (`EXIT_FAILURE`) on error/usage (`drop.c:103,147,195`).

## stdout/stderr contract

- Minimal; errors to stderr `"%s: %s() error %d %s\n"`.
- `-q` suppresses non-error messages.

## Behavior notes

- Default action without `-d`: empty the main DB (`mdbx_drop(..., del=false)`).
- With `-s name`: open the named table with `mdbx_dbi_open(txn, name,
  MDBX_DB_ACCEDE)`; `-d` deletes it, else empties it.
- `mdbx_env_set_maxdbs(env, 2)` when a named table is used.
- Wrap in txn begin → drop → commit; abort on error.

## Rewrite notes

- Trivial tool; small surface. Keep exit-code semantics (empty vs delete
  must remain distinguishable for scripts).