ChangeLog
=========

The source code is available on [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx) and mirror on [GitHub](https://github.com/Mithril-mine/libmdbx).
Please use the `stable` branch or the latest release for production environments through staging, and the `master` branch for development of derivative projects.
Всё будет хорошо!


## v0.15.1 at a deep development stage

Cutting-edge release with new core features and internal refactoring.

### Important:

 - The [project website](https://libmdbx.dqdkfa.ru) is back online and greatly expanded:
    - a textbook with about 50 examples, "deep dive" articles and "first steps" guides;
    - a more readable API reference, completed and refined for the C/C++ API;
    - man pages for every tool, including the new `mdbx_defrag`.
 - The `master` branch develops v0.15; the v0.14.x line continues in the `stable` branch.

### Appreciations:

 - [Andrea Lanfranchi](https://github.com/AndreaLanfranchi) for extensive bug reporting and assistance with fixing most of the issues below.
 - [Omar Gylyjov](https://github.com/dahiAbi) for reporting and fixing the Android database-open failure.
 - [wangkaiyi](https://github.com/wangkaiyi) for reporting the compacting-copy hang.
 - [Mohsin Ismail](https://github.com/mibaloch) for reporting the preopen-inspection defect.

### Backward compatibility breaks:

 - Removed the `MDBX_CPU_WRITEBACK_INCOHERENT` build option; weakly-ordered CPUs are always assumed ([#361](https://github.com/Mithril-mine/libmdbx-devel/issues/361)).
 - Removed the `MDBX_ENABLE_REFUND` build option; online auto-compactification is always enabled ([#193](https://github.com/Mithril-mine/libmdbx-devel/issues/193)).
 - Removed the C++ `env::get_preopen_snapinfo()` overloads without an explicit mode argument ([#182](https://github.com/Mithril-mine/libmdbx-devel/issues/182)).
 - The C++ `env::mode`/`env::durability` enums are scoped now; the old names remain deprecated aliases.

### Improvements:

 - Introduced a unified bilingual documentation site, guides and engineering documentation.
 - Added a bilingual textbook with accompanying example programs.
 - Added `mdbx_preopen_snapinfo_ex()` with an explicit mode of processing weak checkpoints ([#182](https://github.com/Mithril-mine/libmdbx-devel/issues/182)).
 - Added the man page for `mdbx_defrag`, updated the other man pages and every `-h` option.
 - `mdbx_defrag` now names every stopping reason and suggests `mdbx_copy -c` for full compaction.
 - Added the `MDBX_AVOID_FTOK` build option to avoid System V IPC key collisions ([#80](https://github.com/Mithril-mine/libmdbx-devel/issues/80)).
 - On Windows, doomed datafile-shrink attempts are skipped and rate-limited, speeding up commits ([#170](https://github.com/Mithril-mine/libmdbx-devel/issues/170)).
 - Added the `MDBX_opt_shrink_retry_period` option to rate-limit futile datafile-shrink attempts ([#170](https://github.com/Mithril-mine/libmdbx-devel/issues/170)).

### Fixes:

 - Data loss and durability:
    - A failed disk write could be reported as success; a large durable commit crashed on Windows ([#47](https://github.com/Mithril-mine/libmdbx-devel/issues/47)).
    - Auxiliary page-write errors were silently ignored, masking a failing disk ([#203](https://github.com/Mithril-mine/libmdbx-devel/issues/203)).
    - A forged snapshot-age hint could make the writer reuse pages still read by live transactions ([#81](https://github.com/Mithril-mine/libmdbx-devel/issues/81)).
    - `mdbx_drop()` on the internal free-list handle corrupted the page accounting; now rejected ([#69](https://github.com/Mithril-mine/libmdbx-devel/issues/69)).
    - A failed second in-process open released the first handle's file locks ([#59](https://github.com/Mithril-mine/libmdbx-devel/issues/59)).
    - A compacting copy to a pipe reported success but wrote a wrong root page, losing records ([#309](https://github.com/Mithril-mine/libmdbx-devel/issues/309)).
    - On Linux, a non-compacting copy could be corrupt on some network or FUSE filesystems ([#327](https://github.com/Mithril-mine/libmdbx-devel/issues/327)).

 - Resistance to corrupted or crafted "poisoned" data:
    - A crafted garbage-collection record could hand one page to two b-tree nodes; now rejected ([#83](https://github.com/Mithril-mine/libmdbx-devel/issues/83)).
    - A crafted database could crash or corrupt data on delete, drop, estimate or check; now rejected ([#57](https://github.com/Mithril-mine/libmdbx-devel/issues/57), [#63](https://github.com/Mithril-mine/libmdbx-devel/issues/63), [#74](https://github.com/Mithril-mine/libmdbx-devel/issues/74), [#75](https://github.com/Mithril-mine/libmdbx-devel/issues/75)).
    - A crafted tree could make the defragmentation walk exponential, a denial of service ([#82](https://github.com/Mithril-mine/libmdbx-devel/issues/82)).
    - A crafted set of duplicate values could crash or corrupt memory; now rejected ([#176](https://github.com/Mithril-mine/libmdbx-devel/issues/176)).
    - A corrupt reader-table counter drove out-of-bounds scans; the count is now clamped ([#79](https://github.com/Mithril-mine/libmdbx-devel/issues/79), [#113](https://github.com/Mithril-mine/libmdbx-devel/issues/113)).
    - `mdbx_gc_info()` could loop forever holding the writer lock ([#88](https://github.com/Mithril-mine/libmdbx-devel/issues/88)).
    - `mdbx_cursor_scroll()`/`mdbx_cursor_distance()` read out of bounds on a huge `deepness` ([#85](https://github.com/Mithril-mine/libmdbx-devel/issues/85)).

 - Crashes, hangs and undefined behavior in the core engine:
    - `mdbx_enumerate_tables()` could crash or read freed memory when racing with table operations ([#341](https://github.com/Mithril-mine/libmdbx-devel/issues/341)).
    - Deleting a key's last duplicate and repeating the delete could corrupt data or crash; now a no-op ([#301](https://github.com/Mithril-mine/libmdbx-devel/issues/301), [#305](https://github.com/Mithril-mine/libmdbx-devel/issues/305)).
    - After a failed seek among duplicates, a delete corrupted memory; now safely refused ([#305](https://github.com/Mithril-mine/libmdbx-devel/issues/305)).
    - A write through one cursor left others stale, so a later delete could lose committed values ([#304](https://github.com/Mithril-mine/libmdbx-devel/issues/304)).
    - A racing writer could trip a checking-build assertion for every writer sharing the lock file ([#347](https://github.com/Mithril-mine/libmdbx-devel/issues/347)).
    - Without C11 atomics, loads and stores could lose their ordering on ARM-like CPUs ([#340](https://github.com/Mithril-mine/libmdbx-devel/issues/340), [#361](https://github.com/Mithril-mine/libmdbx-devel/issues/361)).
    - `mdbx_replace()` with `MDBX_RESERVE` could crash; now rejected on tables with duplicates ([#166](https://github.com/Mithril-mine/libmdbx-devel/issues/166)).
    - `mdbx_replace_ex()` with overlapping buffers could hang or crash the next operation ([#56](https://github.com/Mithril-mine/libmdbx-devel/issues/56)).
    - A compacting copy hung forever when its writer thread hit a write error; now reported ([#171](https://github.com/Mithril-mine/libmdbx-devel/issues/171)).
    - `mdbx_txn_rollback()` of a nested transaction could corrupt the parent ([#103](https://github.com/Mithril-mine/libmdbx-devel/issues/103), [#124](https://github.com/Mithril-mine/libmdbx-devel/issues/124)).
    - Cloning a read transaction could crash on cleanup or read recycled pages; now fails cleanly ([#128](https://github.com/Mithril-mine/libmdbx-devel/issues/128), [#148](https://github.com/Mithril-mine/libmdbx-devel/issues/148)).
    - Parking a failed transaction crashed on abort; cloning a parked one was unusable ([#104](https://github.com/Mithril-mine/libmdbx-devel/issues/104)).
    - A nested-transaction begin failing out-of-memory left the parent inconsistent ([#67](https://github.com/Mithril-mine/libmdbx-devel/issues/67)).
    - `mdbx_cursor_count_ex()` wrote past the caller's buffer with the legacy structure size ([#68](https://github.com/Mithril-mine/libmdbx-devel/issues/68)).
    - `mdbx_cursor_put(MDBX_MULTIPLE)` with a zero element size divided by zero; now rejected ([#73](https://github.com/Mithril-mine/libmdbx-devel/issues/73)).
    - Undefined behavior in the JSON-integer key encoding for huge magnitudes.
    - An arithmetic overflow miscomputed the maximum database size on 32-bit builds with large RAM.
    - The `MDBX_TXN_TRY` flag leaked into the transaction state flags.
    - A forked child could treat a failed unlock of an inherited lock as fatal at some log levels ([#111](https://github.com/Mithril-mine/libmdbx-devel/issues/111)).
    - A child forked during another thread's database call could hang at exit; `fork()` now waits ([#324](https://github.com/Mithril-mine/libmdbx-devel/issues/324), [#329](https://github.com/Mithril-mine/libmdbx-devel/issues/329)).
    - `mdbx_env_chk()` corrupted the heap or crashed when a callback stopped the check early ([#314](https://github.com/Mithril-mine/libmdbx-devel/issues/314)).
    - `mdbx_txn_park()`/`unpark()` crashed on a transaction without a reader slot; now `MDBX_EPERM` ([#316](https://github.com/Mithril-mine/libmdbx-devel/issues/316)).
    - `mdbx_cursor_renew()` crashed on a wrong cursor or transaction handle ([1/15 of #319](https://github.com/Mithril-mine/libmdbx-devel/issues/319)).

 - Wrong results and API behavior:
    - `mdbx_env_info_ex()` on a not-open environment left the output unset; now zeroed ([2/15 of #319](https://github.com/Mithril-mine/libmdbx-devel/issues/319)).
    - `MDBX_NEXT/PREV_MULTIPLE` without `MDBX_DUPFIXED` gave empty data past the cursor end; now rejected ([3/15 of #319](https://github.com/Mithril-mine/libmdbx-devel/issues/319)).
    - `mdbx_estimate_range()` ignored `begin_data`/`end_data`; `mdbx_estimate_move()` erred on success ([4/15 of #319](https://github.com/Mithril-mine/libmdbx-devel/issues/319)).
    - `mdbx_cursor_scan()` returned `MDBX_ENODATA` instead of `MDBX_RESULT_FALSE` on single-value keys ([5/15 of #319](https://github.com/Mithril-mine/libmdbx-devel/issues/319)).
    - `MDBX_TO_PAIR_*` returned the key's first multi-value instead of the positioned one ([#313](https://github.com/Mithril-mine/libmdbx-devel/issues/313)).
    - `mdbx_put(MDBX_CURRENT)` deleted all multi-values before checking the new value's size ([#313](https://github.com/Mithril-mine/libmdbx-devel/issues/313)).
    - On Linux, a 32-bit process got a far too small total RAM on hosts with 4 GiB or more ([#339](https://github.com/Mithril-mine/libmdbx-devel/issues/339)).
    - `mdbx_env_delete()` left an orphan lock file and reported success with the database in use ([#345](https://github.com/Mithril-mine/libmdbx-devel/issues/345)).
    - `mdbx_env_info_ex()` reported the first writer's open-time mode instead of the current flags ([#338](https://github.com/Mithril-mine/libmdbx-devel/issues/338)).
    - `mdbx_env_set_flags()` could create a forbidden `MDBX_WRITEMAP` and lazy-mode mix; now refused ([#338](https://github.com/Mithril-mine/libmdbx-devel/issues/338)).
    - `mdbx_cursor_get_batch(MDBX_FIRST)` returned only the tail of an already positioned cursor ([#151](https://github.com/Mithril-mine/libmdbx-devel/issues/151)).
    - `mdbx_cursor_scroll()`/`distribute()` returned wrong results near the tree height ([#84](https://github.com/Mithril-mine/libmdbx-devel/issues/84), [#123](https://github.com/Mithril-mine/libmdbx-devel/issues/123)).
    - `mdbx_env_stat_ex()` under-counted the totals once a named table was used ([#108](https://github.com/Mithril-mine/libmdbx-devel/issues/108)).
    - `mdbx_estimate_distance()` returned a negated result for cursors on one key's duplicates ([#86](https://github.com/Mithril-mine/libmdbx-devel/issues/86)).
    - `mdbx_jsonInteger_from_key()` decoded the stored integer `0` as `INT64_MAX` ([#87](https://github.com/Mithril-mine/libmdbx-devel/issues/87)).
    - `mdbx_dbi_rename()` on a stale handle stored a stale record, corrupting the database ([#106](https://github.com/Mithril-mine/libmdbx-devel/issues/106)).
    - Table open/rename mishandled pseudo-names and cross-process drops, up to a crash ([#107](https://github.com/Mithril-mine/libmdbx-devel/issues/107), [#308](https://github.com/Mithril-mine/libmdbx-devel/issues/308)).
    - `mdbx_env_chk()` left `result.tables` dangling; the C++ `env::chk()` threw on found problems ([#93](https://github.com/Mithril-mine/libmdbx-devel/issues/93)).
    - `mdbx_env_warmup()` crashed on a created-but-not-opened environment; now `MDBX_EPERM` ([#114](https://github.com/Mithril-mine/libmdbx-devel/issues/114)).
    - With `MDBX_WRITEMAP`, `mdbx_txn_info()` reported doubled dirty space and lost spilled pages ([#117](https://github.com/Mithril-mine/libmdbx-devel/issues/117)).
    - `MDBX_CP_RENEW_TXN` had no effect since v0.14.2, so a throttled copy could fail spuriously ([#337](https://github.com/Mithril-mine/libmdbx-devel/issues/337)).
    - With a custom spill option, a doubly-nested transaction could fail with `MDBX_PROBLEM` ([#342](https://github.com/Mithril-mine/libmdbx-devel/issues/342)).
    - `mdbx_drop()` on an already empty table now resets its sequence, as for a non-empty one ([#312](https://github.com/Mithril-mine/libmdbx-devel/issues/312)).

 - Windows:
    - A long-lived read transaction could deadlock a writer growing the file; now grown in place ([#154](https://github.com/Mithril-mine/libmdbx-devel/issues/154)).
    - Exclusive-mode commits used the slow flush path; they are now as fast as usual ([#179](https://github.com/Mithril-mine/libmdbx-devel/issues/179)).
    - `MDBX_NOMETASYNC`/`MDBX_SAFE_NOSYNC` degraded to full durable syncs on every commit ([#154](https://github.com/Mithril-mine/libmdbx-devel/issues/154)).
    - The shrink decision used bookkeeping instead of the real file size, skipping shrinks ([#154](https://github.com/Mithril-mine/libmdbx-devel/issues/154)).
    - A file-lock wait could leave the lock held, blocking later opens, or report false success ([#110](https://github.com/Mithril-mine/libmdbx-devel/issues/110)).
    - Empty `MDBX_DBG_*` switches were ignored ([#109](https://github.com/Mithril-mine/libmdbx-devel/issues/109)).
    - A spurious internal assertion failure during page allocation ([#52](https://github.com/Mithril-mine/libmdbx-devel/issues/52)).
    - The I/O ring merged only two contiguous write chunks instead of all of them ([#111](https://github.com/Mithril-mine/libmdbx-devel/issues/111)).
    - After a failed lock-file request, other threads hung forever starting a read transaction ([#332](https://github.com/Mithril-mine/libmdbx-devel/issues/332)).
    - A rare failure while resizing the database could close an unrelated application handle ([#333](https://github.com/Mithril-mine/libmdbx-devel/issues/333)).
    - `mdbx_env_delete()` with `MDBX_ENV_ENSURE_UNUSED`/`WAIT_FOR_UNUSED` never deleted anything ([#336](https://github.com/Mithril-mine/libmdbx-devel/issues/336)).

 - Tools:
    - `mdbx_chk` crashed with `-i`/`-s` on named tables and over-read a corrupt GC record ([#49](https://github.com/Mithril-mine/libmdbx-devel/issues/49), [#76](https://github.com/Mithril-mine/libmdbx-devel/issues/76)).
    - `mdbx_dump`/`mdbx_load` lost data silently on round-trip; a truncated dump passed as complete ([#61](https://github.com/Mithril-mine/libmdbx-devel/issues/61), [#70](https://github.com/Mithril-mine/libmdbx-devel/issues/70)).
    - `mdbx_dump -f` truncated the output file before the command line was validated ([#71](https://github.com/Mithril-mine/libmdbx-devel/issues/71)).
    - `mdbx_load` failed when reading a dump from standard input ([#50](https://github.com/Mithril-mine/libmdbx-devel/issues/50)).
    - `mdbx_load -p -s` deleted every named table; `-s` after a header and `-N`/`-p` misbehaved ([#312](https://github.com/Mithril-mine/libmdbx-devel/issues/312)).
    - `mdbx_load -b` failed on an unchanged batch; empty and long table names restored wrongly ([#312](https://github.com/Mithril-mine/libmdbx-devel/issues/312)).
    - `mdbx_stat -f` failed on an empty GC; `mdbx_dump` closed a failed table as complete ([#312](https://github.com/Mithril-mine/libmdbx-devel/issues/312)).
    - Table names with control characters are now rejected in dumps and sanitized in listings ([#72](https://github.com/Mithril-mine/libmdbx-devel/issues/72)).

 - C++ API:
    - Binding a non-owning handle from an owning temporary leaked it; now a compile error ([#77](https://github.com/Mithril-mine/libmdbx-devel/issues/77), [#121](https://github.com/Mithril-mine/libmdbx-devel/issues/121)).
    - `txn_managed::checkpoint()` dropped the handle on a core rejection, leaking the writer lock ([#89](https://github.com/Mithril-mine/libmdbx-devel/issues/89)).
    - The `*_managed` wrappers could terminate the process on a failed close, with a use-after-free ([#60](https://github.com/Mithril-mine/libmdbx-devel/issues/60)).
    - Committing a parent with a live nested child double-freed; now fails with `MDBX_BAD_TXN` ([#92](https://github.com/Mithril-mine/libmdbx-devel/issues/92)).
    - Assigning `std::string_view` to `mdbx::buffer` allocated despite `noexcept`; could terminate ([#78](https://github.com/Mithril-mine/libmdbx-devel/issues/78)).
    - `cursor::erase("key")` erased every value at the cursor instead of the named key ([#90](https://github.com/Mithril-mine/libmdbx-devel/issues/90)).
    - `buffer` wrote wrong bytes on overlapping sources that forced a capacity grow ([#94](https://github.com/Mithril-mine/libmdbx-devel/issues/94), [#95](https://github.com/Mithril-mine/libmdbx-devel/issues/95), [#125](https://github.com/Mithril-mine/libmdbx-devel/issues/125), [#152](https://github.com/Mithril-mine/libmdbx-devel/issues/152)).
    - `buffer::reserve()` shrank capacity; `buffer` rejected user memory and lost the allocator ([#99](https://github.com/Mithril-mine/libmdbx-devel/issues/99)).
    - A `0`/`NULL` pointer with a length built a reference to address zero; now a compile error ([#137](https://github.com/Mithril-mine/libmdbx-devel/issues/137), [#142](https://github.com/Mithril-mine/libmdbx-devel/issues/142), [#162](https://github.com/Mithril-mine/libmdbx-devel/issues/162)).
    - The lookup/scan wrappers threw or gave wrong results on multimaps, empty tables, small limits ([#98](https://github.com/Mithril-mine/libmdbx-devel/issues/98), [#127](https://github.com/Mithril-mine/libmdbx-devel/issues/127)).
    - Several wrappers threw on documented non-error results or returned wrong values ([#97](https://github.com/Mithril-mine/libmdbx-devel/issues/97), [#102](https://github.com/Mithril-mine/libmdbx-devel/issues/102)).
    - The API did not compile in several documented configurations ([#100](https://github.com/Mithril-mine/libmdbx-devel/issues/100)).
    - Overload resolution could silently pick the wrong function (`if (slice)`, `move()`, `hex()`) ([#101](https://github.com/Mithril-mine/libmdbx-devel/issues/101)).
    - Added the non-throwing `slice::try_as_pod<T>()`, empty on a size mismatch ([#144](https://github.com/Mithril-mine/libmdbx-devel/issues/144)).
    - Documentation refined for cursor unbinding, `chk()` tables lifetime and `append_producer()` ([#91](https://github.com/Mithril-mine/libmdbx-devel/issues/91), [#143](https://github.com/Mithril-mine/libmdbx-devel/issues/143), [#145](https://github.com/Mithril-mine/libmdbx-devel/issues/145), [#146](https://github.com/Mithril-mine/libmdbx-devel/issues/146)).
    - Flags conversion could silently drop `MDBX_NOSTICKYTHREADS` used with `MDBX_EXCLUSIVE` ([#318](https://github.com/Mithril-mine/libmdbx-devel/issues/318)).

 - Performance:
    - Ending a read-only transaction called `getpid()` on Linux, slowing short reads about 8x ([#348](https://github.com/Mithril-mine/libmdbx-devel/issues/348)).
    - Empty durable-mode commits still flushed the device, adding milliseconds to every no-op ([#64](https://github.com/Mithril-mine/libmdbx-devel/issues/64)).
    - Every no-sync write-map commit on Windows flushed the whole mapping ([#65](https://github.com/Mithril-mine/libmdbx-devel/issues/65)).

 - Platform support:
    - `mdbx_env_open()` failed with `EINVAL` on Android f2fs ([#197](https://github.com/Mithril-mine/libmdbx-devel/issues/197)).
    - OpenBSD is excluded from POSIX-semaphore locking, where `sem_init()` always fails ([#115](https://github.com/Mithril-mine/libmdbx-devel/issues/115)).
    - A System V IPC key collision could destroy another database's semaphores; now refused ([#80](https://github.com/Mithril-mine/libmdbx-devel/issues/80), [#159](https://github.com/Mithril-mine/libmdbx-devel/issues/159)).
    - `mdbx_preopen_snapinfo()` reported corruption for a weak checkpoint after an unclean shutdown ([#182](https://github.com/Mithril-mine/libmdbx-devel/issues/182)).
    - With recent Linux headers, a failed file preallocation was ignored until write time ([#320](https://github.com/Mithril-mine/libmdbx-devel/issues/320)).
    - With older Linux headers, a copy without `fallocate()` failed with `EBADF` ([#330](https://github.com/Mithril-mine/libmdbx-devel/issues/330)).
    - `mdbx_env_copy()` to a new file on a filesystem without direct I/O failed with `EEXIST` ([#331](https://github.com/Mithril-mine/libmdbx-devel/issues/331)).
    - With the POSIX-1988 semaphore locking (the default on FreeBSD, NetBSD and Solaris) or the System V one, a signal during a lock wait made the environment fatal, and the lock holder's commit failed with `MDB
X_PANIC` ([#323](https://github.com/Mithril-mine/libmdbx-devel/issues/323)).

 - Build:
    - Fixed MSVC+AddressSanitizer builds failing to compile ([#62](https://github.com/Mithril-mine/libmdbx-devel/issues/62)).
    - Fixed linking with compiler-rt for `__builtin_cpu_supports()` by Clang.
    - Conan recipe: added the `mdbx.lto` and `mdbx.sanitize` options.

 - Leaks:
    - Four transaction error paths leaked table handles or promoted wrong geometry values ([#105](https://github.com/Mithril-mine/libmdbx-devel/issues/105)).
    - Fixed a memory leak on a zero-length table name.

 - Minors:
    - `mdbx_strerror()` had no message for `MDBX_BACKLOG_DEPLETED`, crashing typical error handlers ([#96](https://github.com/Mithril-mine/libmdbx-devel/issues/96)).
    - The non-printf logger passed the full length on truncation; the message is now terminated ([#116](https://github.com/Mithril-mine/libmdbx-devel/issues/116)).
    - On POSIX a copy of a private database was readable by its group ([#331](https://github.com/Mithril-mine/libmdbx-devel/issues/331)).
    - A copy could delete a pre-existing destination on a lock failure or keep its old tail ([#58](https://github.com/Mithril-mine/libmdbx-devel/issues/58), [#112](https://github.com/Mithril-mine/libmdbx-devel/issues/112)).
    - `mdbx_env_copy()` deleted an existing destination on an unrelated open failure ([#310](https://github.com/Mithril-mine/libmdbx-devel/issues/310)).


--------------------------------------------------------------------------------


## v0.15.0 at 2026-09-22

Not a release, but a technical tag marking the start of development for a new version branch.

The engineering of subsequent releases of the v0.14.x version line will continue in the `stable` branch.
In the `master` branch, development of version v0.15 will begin, with the technical version tag set to v0.15.0.


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
