## v0.9.3 at 2021-02-02

Acknowledgments:

 - [Mahlon E. Smith](http://www.martini.nu/) for [FreeBSD port of libmdbx](https://svnweb.freebsd.org/ports/head/databases/mdbx/).
 - [장세연](http://www.castis.com) for bug fixing and PR.
 - [Clément Renault](https://github.com/Kerollmops/heed) for [Heed](https://github.com/Kerollmops/heed) fully typed Rust wrapper.
 - [Alex Sharov](https://github.com/AskAlexSharov) for bug reporting.
 - [Noel Kuntze](https://github.com/Thermi) for bug reporting.

Removed options and features:

 - Drop `MDBX_HUGE_TRANSACTIONS` build-option (now no longer required).

New features:

 - Package for FreeBSD is available now by Mahlon E. Smith.
 - New API functions to get/set various options (https://libmdbx.dqdkfa.ru/dead-github/issues/128):
    - the maximum number of named databases for the environment;
    - the maximum number of threads/reader slots;
    - threshold (since the last unsteady commit) to force flush the data buffers to disk;
    - relative period (since the last unsteady commit) to force flush the data buffers to disk;
    - limit to grow a list of reclaimed/recycled page's numbers for finding a sequence of contiguous pages for large data items;
    - limit to grow a cache of dirty pages for reuse in the current transaction;
    - limit of a pre-allocated memory items for dirty pages;
    - limit of dirty pages for a write transaction;
    - initial allocation size for dirty pages list of a write transaction;
    - maximal part of the dirty pages may be spilled when necessary;
    - minimal part of the dirty pages should be spilled when necessary;
    - how much of the parent transaction dirty pages will be spilled while start each child transaction;
 - Unlimited/Dynamic size of retired and dirty page lists (https://libmdbx.dqdkfa.ru/dead-github/issues/123).
 - Added `-p` option (purge subDB before loading) to `mdbx_load` tool.
 - Reworked spilling of large transaction and committing of nested transactions:
    - page spilling code reworked to avoid the flaws and bugs inherited from LMDB;
    - limit for number of dirty pages now is controllable at runtime;
    - a spilled pages, including overflow/large pages, now can be reused and refunded/compactified in nested transactions;
    - more effective refunding/compactification especially for the loosed page cache.
 - Added `MDBX_ENABLE_REFUND` and `MDBX_PNL_ASCENDING` internal/advanced build options.
 - Added `mdbx_default_pagesize()` function.
 - Better support architectures with a weak/relaxed memory consistency model (ARM, AARCH64, PPC, MIPS, RISC-V, etc) by means [C11 atomics](https://en.cppreference.com/w/c/atomic).
 - Speed up page number lists and dirty page lists (https://libmdbx.dqdkfa.ru/dead-github/issues/132).
 - Added `LIBMDBX_NO_EXPORTS_LEGACY_API` build option.

Fixes:

 - Fixed missing cleanup (null assigned) in the C++ commit/abort (https://libmdbx.dqdkfa.ru/dead-github/pull/143).
 - Fixed `mdbx_realloc()` for case of nullptr and `MDBX_WITHOUT_MSVC_CRT=ON` for Windows.
 - Fixed the possibility to use invalid and renewed (closed & re-opened, dropped & re-created) DBI-handles (https://libmdbx.dqdkfa.ru/dead-github/issues/146).
 - Fixed 4-byte aligned access to 64-bit integers, including access to the `bootid` meta-page's field (https://libmdbx.dqdkfa.ru/dead-github/issues/153).
 - Fixed minor/potential memory leak during page flushing and unspilling.
 - Fixed handling states of cursors's and subDBs's for nested transactions.
 - Fixed page leak in extra rare case the list of retired pages changed during update GC on transaction commit.
 - Fixed assertions to avoid false-positive UB detection by CLANG/LLVM (https://libmdbx.dqdkfa.ru/dead-github/issues/153).
 - Fixed `MDBX_TXN_FULL` and regressive `MDBX_KEYEXIST` during large transaction commit with `MDBX_LIFORECLAIM` (https://libmdbx.dqdkfa.ru/dead-github/issues/123).
 - Fixed auto-recovery (`weak->steady` with the same boot-id) when Database size at last weak checkpoint is large than at last steady checkpoint.
 - Fixed operation on systems with unusual small/large page size, including PowerPC (https://libmdbx.dqdkfa.ru/dead-github/issues/157).


--------------------------------------------------------------------------------


## v0.9.2 at 2020-11-27

Acknowledgments:

 - Jens Alfke (Mobile Architect at [Couchbase](https://www.couchbase.com/)) for [NimDBX](https://github.com/snej/nimdbx).
 - Clément Renault (CTO at [MeiliSearch](https://www.meilisearch.com/)) for [mdbx-rs](https://github.com/Kerollmops/mdbx-rs).
 - Alex Sharov (Go-Lang Teach Lead at [TurboGeth/Ethereum](https://ethereum.org/)) for an extreme test cases and bug reporting.
 - George Hazan (CTO at [Miranda NG](https://www.miranda-ng.org/)) for bug reporting.
 - [Positive Technologies](https://www.ptsecurity.com/) for funding and [The Standoff](https://standoff365.com/).

Added features:

 - Provided package for [buildroot](https://buildroot.org/).
 - Binding for Nim is [available](https://github.com/snej/nimdbx) now by Jens Alfke.
 - Added `mdbx_env_delete()` for deletion an environment files in a proper and multiprocess-safe way.
 - Added `mdbx_txn_commit_ex()` with collecting latency information.
 - Fast completion pure nested transactions.
 - Added `LIBMDBX_INLINE_API` macro and inline versions of some API functions.
 - Added `mdbx_cursor_copy()` function.
 - Extended tests for checking cursor tracking.
 - Added `MDBX_SET_LOWERBOUND` operation for `mdbx_cursor_get()`.

Fixes:

 - Fixed missing installation of `mdbx.h++`.
 - Fixed use of obsolete `__noreturn`.
 - Fixed use of `yield` instruction on ARM if unsupported.
 - Added pthread workaround for buggy toolchain/cmake/buildroot.
 - Fixed use of `pthread_yield()` for non-GLIBC.
 - Fixed use of `RegGetValueA()` on Windows 2000/XP.
 - Fixed use of `GetTickCount64()` on Windows 2000/XP.
 - Fixed opening DB on a network shares (in the exclusive mode).
 - Fixed copy&paste typos.
 - Fixed minor false-positive GCC warning.
 - Added workaround for broken `DEFINE_ENUM_FLAG_OPERATORS` from Windows SDK.
 - Fixed cursor state after multimap/dupsort repeated deletes (https://libmdbx.dqdkfa.ru/dead-github/issues/121).
 - Added `SIGPIPE` suppression for internal thread during `mdbx_env_copy()`.
 - Fixed extra-rare `MDBX_KEY_EXIST` error during `mdbx_commit()` (https://libmdbx.dqdkfa.ru/dead-github/issues/131).
 - Fixed spilled pages checking (https://libmdbx.dqdkfa.ru/dead-github/issues/126).
 - Fixed `mdbx_load` for 'plain text' and without `-s name` cases (https://libmdbx.dqdkfa.ru/dead-github/issues/136).
 - Fixed save/restore/commit of cursors for nested transactions.
 - Fixed cursors state in rare/special cases (move next beyond end-of-data, after deletion and so on).
 - Added workaround for MSVC 19.28 (Visual Studio 16.8) (but may still hang during compilation).
 - Fixed paranoidal Clang C++ UB for bitwise operations with flags defined by enums.
 - Fixed large pages checking (for compatibility and to avoid false-positive errors from `mdbx_chk`).
 - Added workaround for Wine (https://github.com/miranda-ng/miranda-ng/issues/1209).
 - Fixed `ERROR_NOT_SUPPORTED` while opening DB by UNC pathnames (https://github.com/miranda-ng/miranda-ng/issues/2627).
 - Added handling `EXCEPTION_POSSIBLE_DEADLOCK` condition for Windows.


--------------------------------------------------------------------------------


## v0.9.1 2020-09-30

Added features:

 - Preliminary C++ API with support for C++17 polymorphic allocators.
 - [Online C++ API reference](https://libmdbx.dqdkfa.ru/) by Doxygen.
 - Quick reference for Insert/Update/Delete operations.
 - Explicit `MDBX_SYNC_DURABLE` to sync modes for API clarity.
 - Explicit `MDBX_ALLDUPS` and `MDBX_UPSERT` for API clarity.
 - Support for read transactions preparation (`MDBX_TXN_RDONLY_PREPARE` flag).
 - Support for cursor preparation/(pre)allocation and reusing (`mdbx_cursor_create()` and `mdbx_cursor_bind()` functions).
 - Support for checking database using specified meta-page (see `mdbx_chk -h`).
 - Support for turn to the specific meta-page after checking (see `mdbx_chk -h`).
 - Support for explicit reader threads (de)registration.
 - The `mdbx_txn_break()` function to explicitly mark a transaction as broken.
 - Improved handling of corrupted databases by `mdbx_chk` utility and `mdbx_walk_tree()` function.
 - Improved DB corruption detection by checking parent-page-txnid.
 - Improved opening large DB (> 4Gb) from 32-bit code.
 - Provided `pure-function` and `const-function` attributes to C API.
 - Support for user-settable context for transactions & cursors.
 - Revised API and documentation related to Handle-Slow-Readers callback feature.

Deprecated functions and flags:

 - For clarity and API simplification the `MDBX_MAPASYNC` flag is deprecated.
   Just use `MDBX_SAFE_NOSYNC` or `MDBX_UTTERLY_NOSYNC` instead of it.
 - `MDBX_oom_func`, `mdbx_env_set_oomfunc()` and `mdbx_env_get_oomfunc()`
   replaced with `MDBX_hsr_func`, `mdbx_env_get_hsr` and `mdbx_env_get_hsr()`.

Fixes:

 - Fix `mdbx_strerror()` for `MDBX_BUSY` error (no error description is returned).
 - Fix update internal meta-geo information in read-only mode (`EACCESS` or `EBADFD` error).
 - Fix `mdbx_page_get()` null-defer when DB corrupted (crash by `SIGSEGV`).
 - Fix `mdbx_env_open()` for re-opening after non-fatal errors (`mdbx_chk` unexpected failures).
 - Workaround for MSVC 19.27 `static_assert()` bug.
 - Doxygen descriptions and refinement.
 - Update Valgrind's suppressions.
 - Workaround to avoid infinite loop of 'nested' testcase on MIPS under QEMU.
 - Fix a lot of typos & spelling (Thanks to Josh Soref for PR).
 - Fix `getopt()` messages for Windows (Thanks to Andrey Sporaw for reporting).
 - Fix MSVC compiler version requirements (Thanks to Andrey Sporaw for reporting).
 - Workarounds for QEMU's bugs to run tests for cross-built[A library under QEMU.
 - Now C++ compiler optional for building by CMake.


--------------------------------------------------------------------------------


## v0.9.0 2020-07-31 (not a release, but API changes)

Added features:

 - [Online C API reference](https://libmdbx.dqdkfa.ru/) by Doxygen.
 - Separated enums for environment, sub-databases, transactions, copying and data-update flags.

Deprecated functions and flags:

 - Usage of custom comparators and the `mdbx_dbi_open_ex()` are deprecated, since such databases couldn't be checked by the `mdbx_chk` utility.
   Please use the value-to-key functions to provide keys that are compatible with the built-in libmdbx comparators.


********************************************************************************

For early releases and changes see the ChangeLog-Old the git commit history.
