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
 - Fixed `mdbx_cursor_scroll()` and `mdbx_cursor_distance()` indexing the cursor stack out of bounds when `deepness` was above `INT_MAX` (e.g. `UINT_MAX`); the value is now clamped like in `mdbx_cursor_distribute()` ([#85](https://github.com/Mithril-mine/libmdbx-devel/issues/85)).
 - Fixed `mdbx_estimate_distance()` returning a negated result when both cursors were on different duplicates of one key ([#86](https://github.com/Mithril-mine/libmdbx-devel/issues/86)).
 - Fixed `mdbx_jsonInteger_from_key()` decoding the stored integer `0` as `INT64_MAX`; zero now round-trips correctly ([#87](https://github.com/Mithril-mine/libmdbx-devel/issues/87)).




--------------------------------------------------------------------------------


## v0.15.0 at 2026-09-22

Not a release, but a technical tag marking the start of development for a new version branch.

The engineering of subsequent releases of the v0.14.x version line will continue in the `stable` branch.
In the `master` branch, development of version v0.15 will begin, with the technical version tag set to v0.15.0.


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
