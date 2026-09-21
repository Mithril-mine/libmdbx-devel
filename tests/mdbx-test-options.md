# mdbx_test — stochastic test driver options

Research notes for TASK-22 (tiered smoke profiles). `mdbx_test` is the
multi-actor stochastic driver built from `tests/framework/`. It exercises the
library with pseudo-random workloads; it is **not** a unit-test runner (the
unit tests live in `tests/ut/` on top of googletest).

## Quick reference

```sh
mdbx_test --help                  # full option list (this file mirrors it)
mdbx_test --dump-config           # print the resolved configuration before run
mdbx_test --nops=1K --prng-seed=42 --mode=+nosync-safe --hill
```

## Stochasticity and reproducibility

`mdbx_test` derives a pseudo-random sequence from a PRNG seeded with
`--prng-seed=N`. Properties (verified in `tests/framework/`):

- `prng_seed` is stored in `config.params.prng_seed`
  (`config.c++`), and `prng_seed()` seeds the global PRNG state
  (`utils.c++:145`).
- For every table/actor space, the per-space seed is derived deterministically:
  `params.prng_seed += bleach64(space_id)` (`cases.c++:93`). So a fixed base
  seed yields the **same per-actor operation sequence** across runs.
- In **single-process** actor runs (`--hill`, `--nested`, `--ttl`, ...) the whole
  log is reproducible. In the multi-process `basic` case the per-actor sequences
  are still deterministic, but the interleaving of concurrent children is not
  (ordering of child messages in the log may differ between runs).
- A seed of `0` is replaced by entropy (`config::entropy`); always pass an
  explicit non-zero seed to get reproducibility.
- Practical rule: design fast profiles with a **fixed seed and bounded length**
  so a regression can be re-run with identical input. Attach the seed to the
  bug report together with the failing scenario.

## Termination control

Termination is decided per actor in `testcase::should_continue()`
(`test.c++:576`):

- `--duration=N[s|m|h|d]` — stop after N seconds of wall time (default `0` = off).
- `--nops=N[K|M|G|T]` — stop after the actor completed N operations
  (default `1000`).
- If **both** are zero the actor runs indefinitely and must be killed by the
  outer `--timeout` or the CTest TIMEOUT.
- `--timeout=N[s|m|h|d]` — overall run guard (equivalent to a watchdog).
- `--repeat=N` — repeat the whole run N times (default `1`).

Keep at least one bound set. For tiered smoke use `--nops` (bounded work,
predictable wall-clock) rather than relying on `--timeout`.

## Common parameters

| Option | Meaning |
| --- | --- |
| `--help` / `-h` | Show usage |
| `--loglevel=[0-7]` or `fatal..extra` | Verbosity; `notice` for CI logs |
| `--pathname=...` | Path/name of the database files |
| `--repeat=N` | Repeat the run N times |
| `--threads=N` | Number of threads (**unsupported for now**) |
| `--timeout=N[s|m|h|d]` | Overall timeout |
| `--failfast[=YES/no]` | Kill all actors on first failure |
| `--max-readers=N` | `mdbx_env_set_maxreaders()` value |
| `--max-tables=N` | `mdbx_env_set_maxdbs()` value |
| `--dump-config[=YES/no]` | Print resolved config before the run |
| `--progress[=YES/no]` | Progress canary output |
| `--console[=yes/no]` | Console-like output |
| `--cleanup-before[=YES/no]` | Remove and re-create the database first |
| `--cleanup-after[=YES/no]` | Remove the database on completion |
| `--prng-seed=N` | Seed the PRNG (see reproducibility above) |

## Database size

| Option | Meaning |
| --- | --- |
| `--pagesize=min,max,N` | Page size in 256..65536 |
| `--size-lower=N[K|M|G|T]` | Lower bound of the size |
| `--size-upper=N[K|M|G|T]` | Upper bound |
| `--size=N[K|M|G|T]` | Initial size |
| `--shrink-threshold=N[K|M|G|T]` | Shrink threshold |
| `--growth-step=N[K|M|G|T]` | Growth step |

Defaults: `size_now` is 256 MB for dupsort tables, 1 GB otherwise
(`main.c++`). Bound sizes for fast smoke to keep setup cheap.

## Scenarios and actors

`--case=basic` is the predefined complex scenario: simultaneous multi-process
execution of `nested`, `hill`, `ttl`, `copy`, `append`, `jitter`, `try`,
`jitter`, `try`, and (on POSIX) `fork.reader`, `fork.writer`.

Actors selectable directly:

| Actor | Behavior |
| --- | --- |
| `--hill` | Fill-up and empty-down by CRUD quads |
| `--ttl` | Stochastic time-to-live simulation |
| `--nested` | Nested transactions with stochastic-size bellows |
| `--jitter` | Jitter/delays simulation |
| `--try` | Try write-transaction, no more |
| `--copy` | Online copy/backup |
| `--append` | Append-mode insertions |
| `--dead.reader` / `--dead.writer` | Dead reader/writer simulator |
| `--fork.reader` / `--fork.writer` | After-fork reader/writer |

Actor-specific options: `--batch.read=N`, `--batch.write=N`, `--delay=N`,
`--wait4ops=N`, `--duration=N`, `--nops=N`, `--inject-writefault=N`,
`--drop[=yes|NO]`, `--ignore-dbfull[=yes|NO]`, `--speculum[=yes|NO]`,
`--geometry-jitter[=YES|no]`, `--defrag-jitter[=YES|no]`.

## Keys and values

`--keylen.min/--keylen.max/--keylen=N`, `--datalen.min/--datalen.max/
--datalen=N` (plain numbers or `min`/`max`), plus keygen controls
(`--keygen.width`, `--keygen.mesh`, `--keygen.zerofill`, `--keygen.split`,
`--keygen.rotate`, `--keygen.offset`, `--keygen.case=random`).

## Database mode (`--mode={[+-]FLAG},...`)

`+`/`-` add/remove flags relative to the defaults
(`NOSUBDIR | WRITEMAP | SYNC_DURABLE | ACCEDE`):

| Flag | Maps to |
| --- | --- |
| `nosubdir` | `MDBX_NOSUBDIR` |
| `rdonly` | `MDBX_RDONLY` |
| `exclusive` | `MDBX_EXCLUSIVE` |
| `accede` | `MDBX_ACCEDE` |
| `nometasync` | `MDBX_NOMETASYNC` |
| `lifo` | `MDBX_LIFORECLAIM` |
| `nosync-safe` | `MDBX_SAFE_NOSYNC` |
| `writemap` | `MDBX_WRITEMAP` |
| `nosync-utterly` | `MDBX_UTTERLY_NOSYNC` |
| `perturb` | `MDBX_PAGEPERTURB` |
| `nostickythreads` | `MDBX_NOSTICKYTHREADS` |
| `nordahead` | `MDBX_NORDAHEAD` |
| `nomeminit` | `MDBX_NOMEMINIT` |

`--random-writemap[=YES|no]` toggles `MDBX_WRITEMAP` randomly;
`--random-treeopts[=YES|no]` picks tree-related `MDBX_options_t` randomly.

Per the test-performance principles (BACKLOG B27), pick the sync mode per
scenario: `+nosync-safe` (or `nosync-utterly`) when durability is not the point.

## Table options (`--table={[+-]FLAG},...`)

| Flag | Maps to |
| --- | --- |
| `key.reverse` | `MDBX_REVERSEKEY` |
| `key.integer` | `MDBX_INTEGERKEY` |
| `data.multi` | `MDBX_DUPSORT` |
| `data.integer` | `MDBX_INTEGERDUP \| MDBX_DUPFIXED \| MDBX_DUPSORT` |
| `data.fixed` | `MDBX_DUPFIXED \| MDBX_DUPSORT` |
| `data.reverse` | `MDBX_REVERSEDUP \| MDBX_DUPSORT` |

Default is `MDBX_DUPSORT`.

## Practical patterns

- **Fast launch check** (verify build + that mdbx_test starts, per TASK-22 key
  fact): a single tiny run, e.g. `--nops=1 --hill` (tens of ms). Do **not** run
  long stochastic runs when merely adding unit tests.
- **T1 fast smoke**: bounded `--nops`, fixed `--prng-seed`, `+nosync-safe`,
  small bounded size. See `tests/CMakeLists.txt` (`smoke-t1` label) and
  `tests/select-tests.sh`.
- **Reproducibility**: log `--dump-config` output with any failure.