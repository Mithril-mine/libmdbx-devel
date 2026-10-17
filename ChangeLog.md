ChangeLog
=========

The source code is available on [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx) and mirror on [GitHub](https://github.com/Mithril-mine/libmdbx).
Please use the `stable` branch or the latest release for production environments through staging, and the `master` branch for development of derivative projects.
Всё будет хорошо!


## v0.15.1 at a deep development stage

Cutting-edge release with new core features and internal refactoring.

### Important:

  - The operation of the [project website](https://libmdbx.dqdkfa.ru) has been restored after an attack by ukrainian/european bandera terrorists.
    Along with the website’s revival, the documentation has been significantly expanded, and the layout of the API reference guide is now more readable. The documentation now includes:
       - an extensive [textbook](https://libmdbx.dqdkfa.ru/docs/en/textbook/index.html) on libmdbx with about 50 examples;
       - the "deep dive" section with a description of the [architecture](https://libmdbx.dqdkfa.ru/docs/en/deep-dive/architecture.html) and [internal mechanisms](https://libmdbx.dqdkfa.ru/docs/en/deep-dive/internals.html);
       - the ["first steps"](https://libmdbx.dqdkfa.ru/docs/en/getting-started/first-steps.html) sections and the description of [improvements](https://libmdbx.dqdkfa.ru/docs/en/deep-dive/improvements.html) in comparison with LMDB.

    As a result of the attack, no source code was damaged, but the working project site, configured work environments and the infrastructure necessary for their operation were lost.
    Therefore, we had to deploy a new website from working drafts without completing the proofreading of the textbook and other new content. So work on updating the website and proofreading the documentation will continue.

  - In the `master` branch, development of version v0.15 will begin. The engineering of subsequent releases of the v0.14.x version line will continue in the `stable` branch.

### Appreciations:

 - [Andrea Lanfranchi](https://github.com/AndreaLanfranchi) for bug reporting and assistance with fixing ones.

### Improvements:

 - Clarification/refining README and doxygen API descriptions.
 - Added the `-h`/`--help` options to all the command-line utilities and deduplicated their common helpers into a shared header-only module.
 - Added the man page for the `mdbx_defrag` utility and updated the other man pages.
 - Wrote the "Tooling: command-line utilities" document with full option reference of every utility.
 - Added a new bilingual textbook and integrated it into the documentation site, with the accompanying example programs ported and restructured.
 - Introduced a unified bilingual documentation site, six bilingual guides and consolidated engineering documentation.
 - Conan recipe: added the `mdbx.lto` and `mdbx.sanitize` options and documented the rules of the package binary identity — see README "Packaging / Conan".
 - When the locking is auto-selected, POSIX shared anonymous semaphores are now preferred over the legacy System V IPC on platforms that provide them, e.g. macOS ([#80](https://github.com/Mithril-mine/libmdbx-devel/issues/80)).

### Fixes:

 - Fixed a rare durability bug: a failed disk write could be silently reported as success, and a large write-mapped durable commit on Windows could crash ([#47](https://github.com/Mithril-mine/libmdbx-devel/issues/47)).
 - Fixed `mdbx_chk` crashing on databases with named sub-tables when using the `-i`/`-s` options ([#49](https://github.com/Mithril-mine/libmdbx-devel/issues/49)).
 - Fixed `mdbx_load` failing with a missing-file error when reading a dump from standard input ([#50](https://github.com/Mithril-mine/libmdbx-devel/issues/50)).
 - Fixed a spurious internal assertion failure during page allocation on Windows ([#52](https://github.com/Mithril-mine/libmdbx-devel/issues/52)).
 - Fixed an arithmetic overflow that miscomputed the maximum database size on 32-bit builds with large RAM.
 - Fixed a rare internal structure corruption during rollback of a duplicate insertion.
 - Fixed the `MDBX_TXN_TRY` flag leaking into transaction state flags.
 - Fixed `mdbx_dump -p`/`mdbx_load` silently losing data on round-trip when a key or value starts with a space ([#61](https://github.com/Mithril-mine/libmdbx-devel/issues/61)).
 - Fixed `mdbx_replace_ex()` possibly hanging or crashing the next operation on the same sub-table when called with overlapping buffers ([#56](https://github.com/Mithril-mine/libmdbx-devel/issues/56)).
 - Fixed a crafted database file that passed validation and could later corrupt data or crash when deleted ([#57](https://github.com/Mithril-mine/libmdbx-devel/issues/57)).
 - Fixed MSVC+AddressSanitizer builds failing to compile ([#62](https://github.com/Mithril-mine/libmdbx-devel/issues/62)).
 - Fixed `mdbx_env_copy()`/`mdbx_copy -f` possibly deleting a pre-existing destination database when it could not be locked ([#58](https://github.com/Mithril-mine/libmdbx-devel/issues/58)).
 - Fixed a failed second in-process open releasing the first handle's file locks, letting another process take exclusive access ([#59](https://github.com/Mithril-mine/libmdbx-devel/issues/59)).
 - Fixed the C++ `*_managed` wrappers possibly terminating the process on a failed `close()`/`abort()`, and an `env_managed::close()` use-after-free ([#60](https://github.com/Mithril-mine/libmdbx-devel/issues/60)).
 - Fixed a crafted database file driving the engine out of bounds (crash or data corruption) when deleting records ([#63](https://github.com/Mithril-mine/libmdbx-devel/issues/63)).
 - Fixed a performance regression: empty durable-mode commits still flushed the device, adding several milliseconds to every no-op commit ([#64](https://github.com/Mithril-mine/libmdbx-devel/issues/64)).
 - Fixed a Windows performance regression: every no-sync commit in write-map mode flushed the whole mapping ([#65](https://github.com/Mithril-mine/libmdbx-devel/issues/65)).
 - Fixed a nested-transaction begin failing out-of-memory leaving the parent transaction in an inconsistent state ([#67](https://github.com/Mithril-mine/libmdbx-devel/issues/67)).
 - Fixed `mdbx_cursor_count_ex()` writing past the caller's buffer when called with the legacy structure size ([#68](https://github.com/Mithril-mine/libmdbx-devel/issues/68)).
 - Fixed `mdbx_dump -f` truncating the output file on a mistyped command line before the arguments were validated ([#71](https://github.com/Mithril-mine/libmdbx-devel/issues/71)).
 - Fixed `mdbx_dump` reporting success even when writing the dump to a file failed, and `mdbx_load` accepting a truncated dump as a complete database ([#70](https://github.com/Mithril-mine/libmdbx-devel/issues/70)).
 - Fixed `mdbx_dump`/`mdbx_load`/`mdbx_stat` mishandling table names containing control characters: dumping was refused, listing and stat output are sanitized, and such a name in a `database=` header is rejected ([#72](https://github.com/Mithril-mine/libmdbx-devel/issues/72)).
 - Fixed `mdbx_drop()` accepting the internal free-list (GC) handle and destroying the garbage-collection tree, permanently leaking every tracked page and corrupting page accounting; the call is now rejected ([#69](https://github.com/Mithril-mine/libmdbx-devel/issues/69)).
 - Fixed a hardware divide-by-zero in `mdbx_cursor_put(MDBX_MULTIPLE)` when a `MDBX_DUPFIXED` element size of zero was supplied together with a huge element count; the put is now rejected with `MDBX_BAD_VALSIZE` ([#73](https://github.com/Mithril-mine/libmdbx-devel/issues/73)).
 - Fixed `tree_cutoff_twig()` recursing without bound on a crafted database whose branch page forms a cycle, overflowing the C stack when a table is dropped under `MDBX_VALIDATION`; the recursion depth is now bounded ([#74](https://github.com/Mithril-mine/libmdbx-devel/issues/74)).
 - Fixed a hardware divide-by-zero in the range-estimation API (`mdbx_estimate_*`) when a crafted database had a valid tree root but zero leaf/branch page counts; such a tree is now rejected on open with `MDBX_VALIDATION` ([#75](https://github.com/Mithril-mine/libmdbx-devel/issues/75)).
 - Fixed `mdbx_chk` reading out of bounds and crashing on a corrupted GC record whose page count exceeded the page-list limit; the count is now clamped to what the record can hold ([#76](https://github.com/Mithril-mine/libmdbx-devel/issues/76)).
 - Fixed the shared reader-table occupancy counter being used unclamped as a loop/allocation bound over the fixed-size reader table, so a too-large value from a corrupt lock file drove out-of-bounds reads (and a heap overflow on 32-bit); it is now clamped to `max_readers` ([#79](https://github.com/Mithril-mine/libmdbx-devel/issues/79)).
 - Fixed the writer trusting a forged `cached_oldest_txnid` from the external lock mapping, reusing pages still referenced by live readers; it is now rejected (with a forced reader-table rescan) whenever it exceeds the steady snapshot, and the hung-reader recovery no longer passes a non-positive reader pid to the possibly kill-capable HSR callback ([#81](https://github.com/Mithril-mine/libmdbx-devel/issues/81)).
- Fixed the defragmentation walk becoming exponential on a crafted/corrupt tree whose branch pages reference shared sub-trees (a reconverging DAG): the walk was bounded only by the depth of a single path, so it re-descended shared pages once per root-to-leaf path (a denial of service); the walk now rejects any page referenced more than once (a valid copy-on-write tree never shares a page) and is bounded by the file's page count ([#82](https://github.com/Mithril-mine/libmdbx-devel/issues/82)).
 - Fixed `mdbx_cursor_scroll()` and `mdbx_cursor_distribute()` returning wrong positions or counts on DUPSORT tables when `deepness` was at or above the tree height: scroll was off by one step for each duplicate set it entered or left, and distribute used (and corrupted) one of the caller's cursors as its working iterator. Distances measured from inside a single-level duplicate set are now also discounted by the start position; the functions account the duplicates consistently and distribute clones a private iterator ([#84](https://github.com/Mithril-mine/libmdbx-devel/issues/84)).
 - Fixed `mdbx::buffer` assigning `std::string_view` being declared `noexcept` although it allocates owned storage, so an out-of-memory terminated the process via `std::terminate()` instead of propagating a catchable exception; the operator now throws normally. Also fixed `is_freestanding()` comparing the payload offset with `<` instead of `<=`, misclassifying an owned buffer whose data sits at the very end of the silo as a non-owning reference ([#78](https://github.com/Mithril-mine/libmdbx-devel/issues/78)).
 - Fixed `mdbx::txn` being silently constructed from an owning `mdbx::txn_managed`: `mdbx::txn t = env.start_write();` sliced the managed temporary, dropping ownership so the write transaction (and the environment's writer lock) was never released — every later writer blocked or failed with `MDBX_BUSY`; the derived-to-base value conversions are now deleted, turning the slice into a compile error while the base handle stays a copyable non-owning view ([#77](https://github.com/Mithril-mine/libmdbx-devel/issues/77)).
 - Fixed `mdbx_dbi_rename()` storing a stale table record under the new name when the handle begins a write transaction in the `DBI_STALE` state (e.g. after an aborted transaction), silently corrupting the database; the handle is now refreshed before the rename, as in `mdbx_dbi_sequence()` ([#106](https://github.com/Mithril-mine/libmdbx-devel/issues/106)).
 - Fixed table open/rename mishandling the documented pseudo-names: `MDBX_CHK_GC`/`MDBX_CHK_META` were dereferenced before the pointer comparison and crashed the process, re-opening a non-empty table through an open handle with `MDBX_CREATE` and different flags returned success with the old flags instead of `MDBX_INCOMPATIBLE`, and opening a table that another process dropped returned success instead of `MDBX_NOTFOUND` ([#107](https://github.com/Mithril-mine/libmdbx-devel/issues/107)).
 - Fixed `mdbx_env_stat_ex()` under-counting the totals once a named table was used in the transaction, because the MainDB table-record scan skipped the rest of the leaf page; `gc_reserve4return()` now checks the result of `gc_prepare_stockpile4update()`, and the multi-page prefault in `gc_alloc_ex()` writes each page run at its real file offset ([#108](https://github.com/Mithril-mine/libmdbx-devel/issues/108)).
 - Fixed Windows `osal_getenv_singlethreaded()` reporting an environment variable set to an empty value as not set, so `MDBX_DBG_*` switches given as an empty string were ignored; an empty value is now returned as-is again. Also, a failed growth of the static value buffer no longer reports success or loses the previous buffer ([#109](https://github.com/Mithril-mine/libmdbx-devel/issues/109)).
 - Fixed Windows `flock_ex()` handling of a lock request that ends after a wait: on timeout the previously granted lock stayed held (untracked, blocking later opens) and the stack `OVERLAPPED` could be written by the kernel after return; a failed `GetOverlappedResult()` also reported success because the byte count was stored into the error slot. The cancelled request is now awaited and its lock released if it was granted meanwhile, and the real error is returned ([#110](https://github.com/Mithril-mine/libmdbx-devel/issues/110)).
 - Fixed `osal_ipclock_unlock()` in a forked child treating a failed unlock as fatal only when NOTICE logging was enabled, so whether the child panicked depended on the log level; the failure is now always tolerated in a child. Also fixed a Windows I/O ring missing the `last_bytes` update when extending a `WriteFile` item, so only two contiguous chunks were merged instead of all of them ([#111](https://github.com/Mithril-mine/libmdbx-devel/issues/111)).
 - Fixed `mdbx_env_info_ex()` (and `mdbx_env_chk()`) reading the shared reader-table occupancy counter without clamping it to `max_readers`, so a corrupt or hostile lock file drove an out-of-bounds scan over the fixed-size reader table and crashed the process, as the other reader-table scans already did after the #79 fix; the count is now clamped like everywhere else ([#113](https://github.com/Mithril-mine/libmdbx-devel/issues/113)).
 - Fixed `mdbx_env_warmup()` accepting an environment that was created but not opened and dereferencing its unmapped database with a crash; it now returns `MDBX_EPERM` like the other map-using calls. On Windows the `MDBX_opt_writethrough_threshold` getter now returns the value the setter accepts, so save/restore round-trips work. And `default_rp_augment_limit()` now converts the stored GC time limit from monotonic units before comparing it with the 16-second scale, so setting a sensible `MDBX_opt_gc_time_limit` no longer drops the automatic `rp_augment_limit` to its minimum ([#114](https://github.com/Mithril-mine/libmdbx-devel/issues/114)).
 - Fixed `mdbx_env_copy()`/`mdbx_txn_copy2pathname()` mishandling the destination file: with `MDBX_CP_OVERWRITE` a copy whose file lock failed could unlink a destination owned by a concurrent copy (which then reported success with no file left); an exclusive-create failure could leave an orphan file blocking later copies; and an overwritten destination kept the old file's tail. The destination is now truncated once locked, survives lock conflicts and un-owned create failures, and is marked damaged before the stub meta write ([#112](https://github.com/Mithril-mine/libmdbx-devel/issues/112)).
 - Fixed `mdbx_gc_info()` looping forever on a write transaction that has already reclaimed a GC record: the skip used `continue`, which bypassed the cursor advance, so the scan stuck on the same record whenever the transaction could not clean the reclaimed records (e.g. near the size limit), spinning with the writer lock held. The record iterator also honors the documented stop now: a non-zero visitor result ends the enumeration immediately and is returned, instead of the scan continuing and the error being overwritten ([#88](https://github.com/Mithril-mine/libmdbx-devel/issues/88)).
 - Fixed C++ `txn_managed::checkpoint()` dropping the transaction handle when `mdbx_txn_checkpoint()` rejected the transaction with `MDBX_BAD_TXN` (e.g. a broken write transaction or one with a live nested transaction): the state check leaves the transaction live, so the reset lost ownership, the destructor no longer aborted, and the writer lock was leaked until the environment was closed. The handle is now kept for `MDBX_BAD_TXN` as well, so the destructor still aborts ([#89](https://github.com/Mithril-mine/libmdbx-devel/issues/89)).
 - Fixed C++ `cursor::erase()` binding string literals to the `bool` overloads: `c.erase("key")` deleted all values at the cursor position and `c.erase(key, "value")` deleted every value of the key, because a literal converts to `bool` by a standard conversion and to `slice` only by a user-defined one. String-literal overloads now win, so the natural forms erase exactly the named key or pair, with silent data loss ([#90](https://github.com/Mithril-mine/libmdbx-devel/issues/90)).
 - Fixed C++ `buffer::append_hex()/append_base58()/append_base64()` reading freed storage when the source was the buffer itself and the storage had to grow: `reserve_tailroom()` relocated the storage first, and the transcoder then read the freed block, appending garbage. The producer now runs into a temporary buffer before the relocation ([#94](https://github.com/Mithril-mine/libmdbx-devel/issues/94)).
 - Fixed C++ `buffer` move-assignment from a reference-modality source pointing into the target's own storage: the move released the storage first and then copied from the freed block. Such a self-referencing source is now copied before the release ([#95](https://github.com/Mithril-mine/libmdbx-devel/issues/95)).
 - Fixed `mdbx_liberr2str()` having no message for `MDBX_BACKLOG_DEPLETED`, so `mdbx_strerror()` fell back to the (unrelated) system message table and C++ `mdbx::error::what()` returned a null pointer, crashing typical error handlers; the code now has its own message ([#96](https://github.com/Mithril-mine/libmdbx-devel/issues/96)).
 - Fixed `mdbx_cursor_scroll()` and `mdbx_cursor_distance()` indexing the cursor stack out of bounds when `deepness` was above `INT_MAX` (e.g. `UINT_MAX`); the value is now clamped like in `mdbx_cursor_distribute()` ([#85](https://github.com/Mithril-mine/libmdbx-devel/issues/85)).
 - Fixed `mdbx_estimate_distance()` returning a negated result when both cursors were on different duplicates of one key ([#86](https://github.com/Mithril-mine/libmdbx-devel/issues/86)).
 - Fixed `mdbx_jsonInteger_from_key()` decoding the stored integer `0` as `INT64_MAX`; zero now round-trips correctly ([#87](https://github.com/Mithril-mine/libmdbx-devel/issues/87)).
 - Excluded OpenBSD from the POSIX-semaphore locking auto-selection, since its `sem_init()` always fails with `EPERM` for process-shared semaphores and thus a default build could not open any environment; also corrected the misleading comment naming macOS, which uses the POSIX-2001 path anyway ([#115](https://github.com/Mithril-mine/libmdbx-devel/issues/115)).




--------------------------------------------------------------------------------


## v0.15.0 at 2026-09-22

Not a release, but a technical tag marking the start of development for a new version branch.

The engineering of subsequent releases of the v0.14.x version line will continue in the `stable` branch.
In the `master` branch, development of version v0.15 will begin, with the technical version tag set to v0.15.0.


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
