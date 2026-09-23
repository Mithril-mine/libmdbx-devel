# AGENTS-GUIDE — working safely with the amalgamated libmdbx

> This guide is part of the **amalgamated distribution** of libmdbx and is
> intended for **external agents** (AI assistants, bots, code-generators) that
> will embed, build, use or modify the library. It complements `README.md`
> (project overview) and the Doxygen reference. Everything here is derived
> from public facts about the library: its functional architecture, the public
> headers `mdbx.h` / `mdbx.h++`, the shipped examples and the man-pages.
> No internal development-process data is included.

---

## 1. What libmdbx is (short architecture)

libmdbx is an **embedded transactional key-value database**, a deeply reworked
descendant of LMDB. It is a library linked into your application — there is no
server process and no network protocol.

Architecture-shaping properties:

| Property | Consequence |
| --- | --- |
| **B+tree storage** | Data lives in balanced pages; lookup is a descent from root to leaf, `O(log N)`. |
| **MVCC + Copy-on-Write** | Every page is stamped with the transaction that created it; a writer never modifies a page visible to readers, it creates a new version. |
| **Memory-mapped data file** | Reading is memory access (`mmap`); there is **no buffer cache inside the library**. |
| **No WAL, no crash recovery** | Atomicity comes from two-phase updates of a trio of meta-pages; after a crash the last consistent meta is picked. |
| **Wait-free readers, one writer** | Readers take no locks on the read path; all write transactions are serialized by a single global mutex. |
| **Built-in page reuse (GC)** | Freed pages are recycled only when no reader can see them anymore (detent-based). |

The database is a single file (by default `mdbx.dat`) plus a lock file next to
it. Inside the file there is a **main table** holding descriptors of all named
sub-tables (`dbi`s). Each table is its own B+tree.

### 1.1. Data-model essentials

- **Record** = `key → value` pair (`MDBX_val` in C, `mdbx::slice` in C++).
- Keys in a table are sorted by the table's comparator.
- **Multimaps** (`MDBX_DUPSORT`): one key may map to several values, sorted and
  searchable; keys are stored without duplication.
- **Integer keys** (`MDBX_INTEGERKEY`) are natively supported — fast and compact.
- Values larger than ~½ page move to **overflow pages**.

### 1.2. Hard limits (see README "Limitations")

| Entity | Limit (default 4K page size) |
| --- | --- |
| Page size | power of 2, 256…65536 bytes, default 4096 |
| Key size | 0 … ≈½ page (2022 bytes at 4K) |
| Value size | 0 … `0x7fff0000` (~2 GiB); for dupsort values ≈½ page |
| Database size | up to 2^31 pages (≈8 TiB at 4K, ≈128 TiB at 64K) |
| Named tables | up to `MDBX_MAX_DBI` = 32765 |

---

## 2. How to use correctly

### 2.1. Basic lifecycle (C API)

```c
#include "mdbx.h"

int rc;
MDBX_env *env = NULL;
rc = mdbx_env_create(&env);
if (rc != MDBX_SUCCESS) /* handle */;

/* Tune before open: geometry (size/growth), max named tables, options. */
rc = mdbx_env_set_geometry(env, -1, -1, -1, 64*1024, -1, -1);
rc = mdbx_env_set_maxdbs(env, 16);

rc = mdbx_env_open(env, "/path/to/db", MDBX_CREATE, 0664);
if (rc != MDBX_SUCCESS) /* handle */;

/* ... work with transactions ... */

mdbx_env_close(env);
```

Rule of thumb: **create → configure → open → use → close**. Do not call
configure functions after `mdbx_env_open()`.

### 2.2. Transactions — the core discipline

- Exactly **one write transaction** exists at any time process-wide per
  environment. Writers do not deadlock — they serialize.
- Read transactions are **wait-free**: they see a consistent snapshot and never
  block a writer; a writer never blocks them.
- Always end a transaction: `mdbx_txn_commit()` or `mdbx_txn_abort()`. An
  aborted/rolled-back write txn discards all its changes.

```c
MDBX_txn *txn = NULL;
rc = mdbx_txn_begin(env, NULL, 0, &txn);          /* write */
rc = mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT);
rc = mdbx_txn_commit(txn);                         /* or mdbx_txn_abort(txn) */
```

Read-only txns use `MDBX_TXN_RDONLY`; they are cheap and may be nested into a
write txn for consistent multi-table reads.

### 2.3. Tables (DBI handles)

- `mdbx_dbi_open(txn, "name", MDBX_CREATE, &dbi)` creates/opens a named table.
  Use `MDBX_DUPSORT`, `MDBX_DUPFIXED`, `MDBX_INTEGERKEY`, `MDBX_INTEGERDUP`
  deliberately at creation; most flags cannot be changed later.
- Reuse the `dbi` handle across transactions; handle lifecycle is tied to the
  environment, but the mapping is validated per transaction.

### 2.4. Cursors

Cursors traverse/seek within a table and survive writes in the same txn.

```c
MDBX_cursor *cur = NULL;
rc = mdbx_cursor_open(txn, dbi, &cur);
rc = mdbx_cursor_get(cur, &key, &value, MDBX_SET_KEY);
while ((rc = mdbx_cursor_get(cur, &key, &value, MDBX_NEXT)) == MDBX_SUCCESS)
    /* process pair */;
mdbx_cursor_close(cur);
```

Cursors do not block readers or writers; long-lived cursors inside a long-lived
read txn still count as readers (see §3).

### 2.5. Error codes

- Functions return `MDBX_SUCCESS` (0) on success; errors are negative.
- `MDBX_RESULT_TRUE` (-1) is a **success-like** sentinel for "not found /
  no more data / greater key found" depending on the call — treat it per API
  contract, not as an error.
- Common codes: `MDBX_NOTFOUND`, `MDBX_MAP_FULL` (DB grew to its geometry upper
  bound), `MDBX_BAD_TXN`, `MDBX_OUSTED` (a parked read txn was ousted),
  `MDBX_KEYEXIST`, `MDBX_BAD_VALSIZE`.

Do **not** rely on errno; libmdbx uses its own mdbx-specific values even when
they collide numerically with platform error numbers (`MDBX_ENOSYS`, for
example, is `ERROR_NOT_SUPPORTED` on Windows and `ENOSYS` on POSIX). Read the
*mdbx* meaning, not the errno name.

### 2.6. Durability modes (choose consciously)

| Mode | Semantics |
| --- | --- |
| `MDBX_SYNC_DURABLE` (default) | Flush data, then two-phase meta flush. Full ACID. |
| `MDBX_NOMETASYNC` | Data flushed, meta deferred. May lose last commits on crash. |
| `MDBX_SAFE_NOSYNC` | Delayed flushes, previous steady commit kept. Fast, but grows the file between steady points. |
| `MDBX_UTTERLY_NOSYNC` | No flushes. Highest speed, worst crash safety. |
| `MDBX_WRITEMAP` | Write through mmap (+msync). Can be combined with the above. |

Pick the weakest mode that still satisfies your durability contract; document
the choice.

### 2.7. C++ API (recommended for new code)

Include `mdbx.h++`. Key RAII types: `mdbx::env_managed`, `mdbx::txn_managed`,
`mdbx::map_handle`, `mdbx::cursor_managed`, `mdbx::slice`, `mdbx::buffer`.
Errors become exceptions derived from `mdbx::exception`; `error::success_or_throw()`
wraps C return codes.

```c++
#include "mdbx.h++"
mdbx::env_managed env("/tmp/db", mdbx::env_managed::create_parameters{},
                      mdbx::env::operate_parameters{}, /* accede = */ true);
auto txn = env.start_write();
auto dbi = txn.open_map("hello", mdbx::key_mode::usual, mdbx::value_mode::single);
txn.put(dbi, mdbx::slice("key"), mdbx::slice("value"), mdbx::put_mode::upsert);
txn.commit();
```

Prefer the C++ API for new code: RAII prevents the classic resource-leak bugs
(see §3).

---

## 3. Safe-work SKILLS: risks, patterns, anti-patterns

Every rule is given with its **why** so an agent can reason, not just pattern-match.

### 3.1. Long-lived read transactions (the #1 growth risk)

- **Why:** MVCC + CoW keep every page alive that a reader's snapshot can still
  see. While a read txn lives, pages retired by writers **cannot be recycled**,
  so the database file grows and can exhaust its geometry upper bound
  (`MDBX_MAP_FULL`).
- **Pattern:** short, scoped read txns; use `mdbx_txn_park()`/unpark for long
  interactive sessions; register a Handle-Slow-Readers callback
  (`MDBX_hsr_func`) to resolve "reader is blocking the world".
- **Anti-pattern:** opening a read txn and keeping it while debugging,
  streaming to a slow sink, or sleeping.
- **Check:** `txn_space_retired` tells how much space writers retired after your
  snapshot — if it grows, your reader is holding it hostage.

### 3.2. Transaction hygiene (leaks and state)

- **Why:** an unfinished write txn holds the writer lock and pins dirty state;
  an unfinished read txn pins a snapshot. Both degrade or stall the system.
- **Pattern:** always pair begin/commit-or-abort; use RAII (C++) or a cleanup
  path in C; prefer read-only txns for reads.
- **Anti-pattern:** early `return` between `mdbx_txn_begin()` and `commit`.
- **Check:** commit or abort on **every** exit path of the function.

### 3.3. Cursor lifetime

- **Why:** cursors hold page pointers and a position; using them after txn end,
  or closing the txn while a cursor is still referenced, is undefined.
- **Pattern:** open cursors after the txn, close them before the txn ends; in
  C++ prefer `cursor_managed` scoped to the txn.
- **Anti-pattern:** storing a cursor and reusing it across transactions.
- **Check:** close order = cursors → txn.

### 3.4. Value/record sizing assumptions

- **Why:** keys are limited to ≈½ page, dupsort values to ≈½ page, ordinary
  values to ~2 GiB. Overflow pages exist but have cost.
- **Pattern:** treat the documented limits as hard constants fetched from
  `mdbx_env_get_maxkeysize()`; test with maximum-size records.
- **Anti-pattern:** assuming 4 KiB values fit in any table, or assuming large
  keys work.
- **Check:** `MDBX_BAD_VALSIZE` / `MDBX_BAD_PARAM` on put — usually a size bug.

### 3.5. Multi-process and containers

- **Why:** the DB is shared between processes through mmap + a lock file;
  correctness depends on coherent mapping and PID/handle visibility.
- **Pattern:** run DB-aware processes with a shared PID namespace
  (`--pid=host` in Docker, or `--pid=container:<id>`); keep one physical copy
  of each mapped page in system memory.
- **Anti-pattern:** multiple containers with isolated PIDs opening the same DB.
- **Check:** the `options:` line in `mdbx_chk -V` (the build-options string)
  must be identical across processes sharing a DB.

### 3.6. DSO/DLL unloading and TLS destructors

- **Why:** libmdbx tracks per-thread reader slots via thread-local storage;
  unloading a shared library that used it can crash or leak if the platform
  does not run TLS destructors properly.
- **Pattern:** avoid unloading a DSO that contains libmdbx after use, or ensure
  the platform supports `__cxa_thread_atexit_impl()` (glibc ≥ 2.18, modern
  Windows).
- **Anti-pattern:** dlclose()'ing a library that opened environments from other
  threads.
- **Check:** unload/destructor paths are rarely tested — add an explicit test.

### 3.7. Error handling completeness

- **Why:** libmdbx returns rich, specific codes; swallowing them hides
  corruption, size or locking problems until they become unrecoverable.
- **Pattern:** check every API return; translate mdbx codes to your own error
  domain explicitly; treat `MDBX_RESULT_TRUE` per the specific API contract.
- **Anti-pattern:** `(void)rc;` or comparing everything to `!= 0`.
- **Check:** run `mdbx_chk` after abnormal termination to catch corruption early.

### 3.8. Performance expectations

- **Why:** because reads are direct mmap accesses, naive linear scans can beat
  "smart" indexing at small scale, but suboptimal code only shows its cost on
  large data — and random page access favors SSDs.
- **Pattern:** measure with realistic data sizes; use `MDBX_APPEND` for bulk
  pre-sorted inserts; batch writes; consider `MDBX_LIFORECLAIM` for
  many-readers-heavy workloads.
- **Anti-pattern:** micro-benchmarking on a 100-row DB and extrapolating.
- **Check:** fullscan vs indexed lookup on the real workload.

---

## 4. Safety checklist (self-contained)

Before reporting work on libmdbx correct, verify:

- [ ] Every transaction is committed or aborted on all exit paths (§3.2).
- [ ] No read transaction outlives its logical operation (§3.1).
- [ ] Cursors are closed before their transaction (§3.3).
- [ ] Keys/values respect the documented limits (fetch via API, not by hand) (§3.4).
- [ ] All error codes are handled; `MDBX_RESULT_TRUE` semantics respected (§3.5/§2.5).
- [ ] Durability mode chosen deliberately and documented (§2.6).
- [ ] Multi-process/container PID & mmap coherence verified (§3.5).
- [ ] The environment is closed on shutdown; no DSO-unload-after-use (§3.6).
- [ ] `mdbx_chk` passes on any DB you report as consistent.
- [ ] Examples compile with the shipped headers on the target platform.

---

## 5. Resource catalog

### 5.1. Official sources

- Home & documentation: <https://libmdbx.dqdkfa.ru> (verified)
- Doxygen reference: <https://libmdbx.dqdkfa.ru/doxygen/> (verified)
- Amalgamated sources: <https://sourcecraft.dev/dqdkfa/libmdbx> (verified);
  mirror <https://github.com/Mithril-mine/libmdbx> (verified)

### 5.2. Console tools (shipped in the amalgamation, man1/)

| Tool | Purpose |
| --- | --- |
| `mdbx_chk` | integrity check / repair (`mdbx_chk -vvn` recommended) |
| `mdbx_dump` / `mdbx_load` | export / import of data |
| `mdbx_stat` | statistics |
| `mdbx_copy` | hot backup |
| `mdbx_drop` | remove databases/tables |
| `mdbx_defrag` | online compaction / defragmentation |

### 5.3. Examples (shipped)

- `examples/example-mdbx.c` — C API walkthrough (line-by-line comparable with
  the old Berkeley-DB example in `examples/sample-bdb.txt`).
- `examples/example-mdbx.c++` — modern C++ API example.
- `examples/pcrf/pcrf_simulator.c` — a larger simulation workload.

### 5.4. Bindings (community, verified links)

The full list (several dozen) lives in the "Bindings and Projects" section of
the libmdbx homesite. High-demand ones (links verified in `README.md`):

| Runtime | Repo |
| --- | --- |
| Rust | <https://github.com/vorot93/libmdbx-rs> |
| Go | <https://github.com/torquem-ch/mdbx-go> |
| .NET | <https://public.git.amsoft.spb.ru/libmdbx/libmdbx-dotnet> |
| Python | <https://pypi.org/project/libmdbx/> |
| CPython | <https://pypi.org/project/clibmdbx/> |
| NodeJS | <https://github.com/ikonopistsev/mdbxmou> |
| Zig | <https://github.com/theseyan/lmdbx-zig> |

Always confirm the latest URL on <https://libmdbx.dqdkfa.ru/#sec-projects>
before linking; third-party repos may move.

### 5.5. Where things are NOT

The amalgamated tarball deliberately ships **no tests** and **no internal
development docs**. Do not promise the user `tests/`, `skynet/`, or `AGENTS.md`
from the development repository — point them to the dev repo or the examples.

---

*End of AGENTS-GUIDE. Keep this file in sync with README.md and the public
headers when the API or limits change.*