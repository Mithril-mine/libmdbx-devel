## v0.11.14 "Sergey Kapitsa" at 2023-02-14

The stable bugfix release in memory of [Sergey Kapitsa](https://en.wikipedia.org/wiki/Sergey_Kapitsa) on his 95th birthday.

```
22 files changed, 250 insertions(+), 174 deletions(-)
Signed-off-by: Леонид Юрьев (Leonid Yuriev) <leo@yuriev.ru>
```

Fixes:
 - backport: Fixed insignificant typo of `||` inside `#if` byte-order condition.
 - backport: Fixed `SIGSEGV` or an erroneous call to `free()` in situations where
   errors occur when reopening by `mdbx_env_open()` of a previously used
   environment.
 - backport: Fixed `cursor_put_nochecklen()` internals for case when dupsort'ed named subDb
   contains a single key with multiple values (aka duplicates), which are replaced
   with a single value by put-operation with the `MDBX_UPSERT+MDBX_ALLDUPS` flags.
   In this case, the database becomes completely empty, without any pages.
   However exactly this condition was not considered and thus wasn't handled correctly.
   See [issue#8](https://gitflic.ru/project/erthink/libmdbx/issue/8) for more information.
 - backport: Fixed extra assertion inside `override_meta()`, which could
   lead to false-positive failing of the assertion in a debug builds during
   DB recovery and auto-rollback.
 - backport: Refined the `__cold`/`__hot` macros to avoid the
   `error: inlining failed in call to ‘always_inline FOO(...)’: target specific option mismatch`
   issue during build using GCC >10.x for SH4 arch.

Minors:

 - backport: Using the https://libmdbx.dqdkfa.ru/dead-github
   for resources deleted by the GitHub' administration.
 - backport: Fixed English typos.
 - backport: Fixed proto of `__asan_default_options()`.
 - backport: Fixed doxygen-description of C++ API, especially of C++20 concepts.
 - backport: Refined `const` and `noexcept` for few C++ API methods.
 - backport: Fixed copy&paste typo of "Getting started".
 - backport: Update MithrilDB status.
 - backport: Resolve false-posirive `used uninitialized` warning from GCC >10.x
   while build for SH4 arch.


--------------------------------------------------------------------------------


## v0.11.13 at "Swashplate" 2022-11-10

The stable bugfix release in memory of [Boris Yuryev](https://ru.wikipedia.org/wiki/Юрьев,_Борис_Николаевич) on his 133rd birthday.

```
30 files changed, 405 insertions(+), 136 deletions(-)
Signed-off-by: Леонид Юрьев (Leonid Yuriev) <leo@yuriev.ru>
```

Fixes:

 - Fixed builds with older libc versions after using `fcntl64()` (backport).
 - Fixed builds with  older `stdatomic.h` versions,
   where the `ATOMIC_*_LOCK_FREE` macros mistakenly redefined using functions (backport).
 - Added workaround for `mremap()` defect to avoid assertion failure (backport).
 - Workaround for `encryptfs` bug(s) in the `copy_file_range` implementation  (backport).
 - Fixed unexpected `MDBX_BUSY` from `mdbx_env_set_option()`, `mdbx_env_set_syncbytes()`
   and `mdbx_env_set_syncperiod()` (backport).
 - CMake requirements lowered to version 3.0.2 (backport).

Minors:

 - Minor clarification output of `--help` for `mdbx_test` (backport).
 - Added admonition of insecure for RISC-V (backport).
 - Stochastic scripts and CMake files synchronized with the `devel` branch.
 - Use `--dont-check-ram-size` for small-tests make-targets (backport).


--------------------------------------------------------------------------------


## v0.11.12 "Эребуни" at 2022-10-12

The stable bugfix release.

```
11 files changed, 96 insertions(+), 49 deletions(-)
Signed-off-by: Леонид Юрьев (Leonid Yuriev) <leo@yuriev.ru>
```

Fixes:

 - Fixed static assertion failure on platforms where the `off_t` type is wider
   than corresponding fields of `struct flock` used for file locking (backport).
   Now _libmdbx_ will use `fcntl64(F_GETLK64/F_SETLK64/F_SETLKW64)` if available.
 - Fixed assertion check inside `page_retire_ex()` (backport).

Minors:

 - Fixed `-Wint-to-pointer-cast` warnings while casting to `mdbx_tid_t` (backport).
 - Removed needless `LockFileEx()` inside `mdbx_env_copy()` (backport).


--------------------------------------------------------------------------------


## v0.11.11 "Тендра-1790" at 2022-09-11

The stable bugfix release.

```
10 files changed, 38 insertions(+), 21 deletions(-)
Signed-off-by: Леонид Юрьев (Leonid Yuriev) <leo@yuriev.ru>
```

Fixes:

 - Fixed an extra check for `MDBX_APPENDDUP` inside `mdbx_cursor_put()` which could result in returning `MDBX_EKEYMISMATCH` for valid cases.
 - Fixed an extra ensure/assertion check of `oldest_reader` inside `mdbx_txn_end()`.
 - Fixed derived C++ builds by removing `MDBX_INTERNAL_FUNC` for `mdbx_w2mb()` and `mdbx_mb2w()`.


--------------------------------------------------------------------------------


## v0.11.10 "the TriColor" at 2022-08-22

The stable bugfix release.

```
14 files changed, 263 insertions(+), 252 deletions(-)
Signed-off-by: Леонид Юрьев (Leonid Yuriev) <leo@yuriev.ru>
```

New:

 - The C++ API has been refined to simplify support for `wchar_t` in path names.
 - Added explicit error message for Buildroot's Microblaze toolchain maintainers.

Fixes:

 - Never use modern `__cxa_thread_atexit()` on Apple's OSes.
 - Use `MultiByteToWideChar(CP_THREAD_ACP)` instead of `mbstowcs()`.
 - Don't check owner for finished transactions.
 - Fixed typo in `MDBX_EINVAL` which breaks MingGW builds with CLANG.

Minors:

 - Fixed variable name typo.
 - Using `ldd` to check used dso.
 - Added `MDBX_WEAK_IMPORT_ATTRIBUTE` macro.
 - Use current transaction geometry for untouched parameters when `env_set_geometry()` called within a write transaction.
 - Minor clarified `iov_page()` failure case.


--------------------------------------------------------------------------------


## v0.11.9 "Чирчик-1992" at 2022-08-02

The stable bugfix release.

```
18 files changed, 318 insertions(+), 178 deletions(-)
Signed-off-by: Леонид Юрьев (Leonid Yuriev) <leo@yuriev.ru>
```

Acknowledgments:

 - [Alex Sharov](https://github.com/AskAlexSharov) and Erigon team for reporting and testing.
 - [Andrew Ashikhmin](https://gitflic.ru/user/yperbasis) for contributing.

New:

 - Ability to customise `MDBX_LOCK_SUFFIX`, `MDBX_DATANAME`, `MDBX_LOCKNAME` just by predefine ones during build.
 - Added to [`mdbx::env_managed`](https://libmdbx.dqdkfa.ru/group__cxx__api.html#classmdbx_1_1env__managed)'s methods a few overloads with `const char* pathname` parameter (C++ API).

Fixes:

 - Fixed hang copy-with-compactification of a corrupted DB
   or in case the volume of output pages is a multiple of `MDBX_ENVCOPY_WRITEBUF`.
 - Fixed standalone non-CMake build on MacOS (`#include AvailabilityMacros.h>`).
 - Fixed unexpected `MDBX_PAGE_FULL` error in rare cases with large database page sizes.

Minors:

 - Minor fixes Doxygen references, comments, descriptions, etc.
 - Fixed copy&paste typo inside `meta_checktxnid()`.
 - Minor fix `meta_checktxnid()` to avoid assertion in debug mode.
 - Minor fix `mdbx_env_set_geometry()` to avoid returning `EINVAL` in particular rare cases.
 - Minor refine/fix batch-get testcase for large page size.
 - Added `--pagesize NN` option to long-stotastic test script.
 - Updated Valgrind-suppressions file for modern GCC.
 - Fixed `has no symbols` warning from Apple's ranlib.


--------------------------------------------------------------------------------


## v0.11.8 "Baked Apple" at 2022-06-12

The stable release with an important fixes and workaround for the critical macOS thread-local-storage issue.

Acknowledgments:

 - [Masatoshi Fukunaga](https://github.com/mah0x211) for [Lua bindings](https://github.com/mah0x211/lua-libmdbx).

New:

 - Added most of transactions flags to the public API.
 - Added `MDBX_NOSUCCESS_EMPTY_COMMIT` build option to return non-success result (`MDBX_RESULT_TRUE`) on empty commit.
 - Reworked validation and import of DBI-handles into a transaction.
   Assumes  these changes will be invisible to most users, but will cause fewer surprises in complex DBI cases.
 - Added ability to open DB in without-LCK (exclusive read-only) mode in case no permissions to create/write LCK-file.

Fixes:

 - A series of fixes and improvements for automatically generated documentation (Doxygen).
 - Fixed copy&paste bug with could lead to `SIGSEGV` (nullptr dereference) in the exclusive/no-lck mode.
 - Fixed minor warnings from modern Apple's CLANG 13.
 - Fixed minor warnings from CLANG 14 and in-development CLANG 15.
 - Fixed `SIGSEGV` regression in without-LCK (exclusive read-only) mode.
 - Fixed `mdbx_check_fs_local()` for CDROM case on Windows.
 - Fixed nasty typo of typename which caused false `MDBX_CORRUPTED` error in a rare execution path,
   when the size of the thread-ID type not equal to 8.
 - Fixed Elbrus/E2K LCC 1.26 compiler warnings (memory model for atomic operations, etc).
 - Fixed write-after-free memory corruption on latest `macOS` during finalization/cleanup of thread(s) that executed read transaction(s).
   > The issue was suddenly discovered by a [CI](https://en.wikipedia.org/wiki/Continuous_integration)
   > after adding an iteration with macOS 11 "Big Sur", and then reproduced on recent release of macOS 12 "Monterey".
   > The issue was never noticed nor reported on macOS 10 "Catalina" nor others.
   > Analysis shown that the problem caused by a change in the behavior of the system library (internals of dyld and pthread)
   > during thread finalization/cleanup: now a memory allocated for a `__thread` variable(s) is released
   > before execution of the registered Thread-Local-Storage destructor(s),
   > thus a TLS-destructor will write-after-free just by legitime dereference any `__thread` variable.
   > This is unexpected crazy-like behavior since the order of resources releasing/destroying
   > is not the reverse of ones acquiring/construction order. Nonetheless such surprise
   > is now workarounded by using atomic compare-and-swap operations on a 64-bit signatures/cookies.

Minors:

 - Refined `release-assets` GNU Make target.
 - Added logging to `mdbx_fetch_sdb()` to help debugging complex DBI-handels use cases.
 - Added explicit error message from probe of no-support for `std::filesystem`.
 - Added contributors "score" table by `git fame` to generated docs.
 - Added `mdbx_assert_fail()` to public API (mostly for backtracing).
 - Now C++20 concepts used/enabled only when `__cpp_lib_concepts >= 202002`.
 - Don't provide nor report package information if used as a CMake subproject.


--------------------------------------------------------------------------------


## v0.11.7 "Resurrected Sarmat" at 2022-04-22

The stable risen release after the GitHub's intentional malicious disaster.

#### We have migrated to a reliable trusted infrastructure
The origin for now is at [GitFlic](https://gitflic.ru/project/erthink/libmdbx)
since on 2022-04-15 the GitHub administration, without any warning nor
explanation, deleted _libmdbx_ along with a lot of other projects,
simultaneously blocking access for many developers.
For the same reason ~~GitHub~~ is blacklisted forever.

GitFlic already support Russian and English languages, plan to support more,
including 和 中文. You are welcome!

New:

 - Added the `tools-static` make target to build statically linked MDBX tools.
 - Support for Microsoft Visual Studio 2022.
 - Support build by MinGW' make from command line without CMake.
 - Added `mdbx::filesystem` C++ API namespace that corresponds to `std::filesystem` or `std::experimental::filesystem`.
 - Created [website](https://libmdbx.dqdkfa.ru/) for online auto-generated documentation.
 - Used `https://web.archive.org/web/https://github.com/erthink/libmdbx` for dead (or temporarily lost) resources deleted by ~~GitHub~~.
 - Added `--loglevel=` command-line option to the `mdbx_test` tool.
 - Added few fast smoke-like tests into CMake builds.

Fixes:

 - Fixed a race between starting a transaction and creating a DBI descriptor that could lead to `SIGSEGV` in the cursor tracking code.
 - Clarified description of `MDBX_EPERM` error returned from `mdbx_env_set_geometry()`.
 - Fixed non-promoting the parent transaction to be dirty in case the undo of the geometry update failed during abortion of a nested transaction.
 - Resolved linking issues with `libstdc++fs`/`libc++fs`/`libc++experimental` for C++ `std::filesystem` or `std::experimental::filesystem` for legacy compilers.
 - Added workaround for GNU Make 3.81 and earlier.
 - Added workaround for Elbrus/LCC 1.25 compiler bug of class inline `static constexpr` member field.
 - [Fixed](https://github.com/ledgerwatch/erigon/issues/3874) minor assertion regression (only debug builds were affected).
 - Fixed detection of `C++20` concepts accessibility.
 - Fixed detection of Clang's LTO availability for Android.
 - Fixed extra definition of `_FILE_OFFSET_BITS=64` for Android that is problematic for 32-bit Bionic.
 - Fixed build for ARM/ARM64 by MSVC.
 - Fixed non-x86 Windows builds with `MDBX_WITHOUT_MSVC_CRT=ON` and `MDBX_BUILD_SHARED_LIBRARY=ON`.

Minors:

 - Resolve minor MSVC warnings: avoid `/INCREMENTAL[:YES]` with `/LTCG`, `/W4` with `/W3`, the `C5105` warning.
 - Switched to using `MDBX_EPERM` instead of `MDBX_RESULT_TRUE` to indicate that the geometry cannot be updated.
 - Added `NULL` checking during memory allocation inside `mdbx_chk`.
 - Resolved all warnings from MinGW while used without CMake.
 - Added inheritable `target_include_directories()` to `CMakeLists.txt` for easy integration.
 - Added build-time checks and paranoid runtime assertions for the `off_t` arguments of `fcntl()` which are used for locking.
 - Added `-Wno-lto-type-mismatch` to avoid false-positive warnings from old GCC during LTO-enabled builds.
 - Added checking for TID (system thread id) to avoid hang on 32-bit Bionic/Android within `pthread_mutex_lock()`.
 - Reworked `MDBX_BUILD_TARGET` of CMake builds.
 - Added `CMAKE_HOST_ARCH` and `CMAKE_HOST_CAN_RUN_EXECUTABLES_BUILT_FOR_TARGET`.


--------------------------------------------------------------------------------


## v0.11.6 at 2022-03-24

The stable release with the complete workaround for an incoherence flaw of Linux unified page/buffer cache.
Nonetheless the cause for this trouble may be an issue of Intel CPU cache/MESI.
See [issue#269](https://libmdbx.dqdkfa.ru/dead-github/issues/269) for more information.

Acknowledgments:

 - [David Bouyssié](https://github.com/david-bouyssie) for [Scala bindings](https://github.com/david-bouyssie/mdbx4s).
 - [Michelangelo Riccobene](https://github.com/mriccobene) for reporting and testing.

Fixes:

 - [Added complete workaround](https://libmdbx.dqdkfa.ru/dead-github/issues/269) for an incoherence flaw of Linux unified page/buffer cache.
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/issues/272) cursor reusing for read-only transactions.
 - Fixed copy&paste typo inside `mdbx::cursor::find_multivalue()`.

Minors:

 - Minor refine C++ API for convenience.
 - Minor internals refines.
 - Added `lib-static` and `lib-shared` targets for make.
 - Added minor workaround for AppleClang 13.3 bug.
 - Clarified error messages of a signature/version mismatch.


--------------------------------------------------------------------------------


## v0.11.5 at 2022-02-23

The release with the temporary hotfix for a flaw of Linux unified page/buffer cache.
See [issue#269](https://libmdbx.dqdkfa.ru/dead-github/issues/269) for more information.

Acknowledgments:

 - [Simon Leier](https://github.com/leisim) for reporting and testing.
 - [Kai Wetlesen](https://github.com/kaiwetlesen) for [RPMs](http://copr.fedorainfracloud.org/coprs/kwetlesen/libmdbx/).
 - [Tullio Canepa](https://github.com/canepat) for reporting C++ API issue and contributing.

Fixes:

 - [Added hotfix](https://libmdbx.dqdkfa.ru/dead-github/issues/269) for a flaw of Linux unified page/buffer cache.
 - [Fixed/Reworked](https://libmdbx.dqdkfa.ru/dead-github/pull/270) move-assignment operators for "managed" classes of C++ API.
 - Fixed potential `SIGSEGV` while open DB with overrided non-default page size.
 - [Made](https://libmdbx.dqdkfa.ru/dead-github/issues/267) `mdbx_env_open()` idempotence in failure cases.
 - Refined/Fixed pages reservation inside `mdbx_update_gc()` to avoid non-reclamation in a rare cases.
 - Fixed typo in a retained space calculation for the hsr-callback.

Minors:

 - Reworked functions for meta-pages, split-off non-volatile.
 - Disentangled C11-atomic fences/barriers and pure-functions (with `__attribute__((__pure__))`) to avoid compiler misoptimization.
 - Fixed hypotetic unaligned access to 64-bit dwords on ARM with `__ARM_FEATURE_UNALIGNED` defined.
 - Reasonable paranoia that makes clarity for code readers.
 - Minor fixes Doxygen references, comments, descriptions, etc.


--------------------------------------------------------------------------------


## v0.11.4 at 2022-02-02

The stable release with fixes for large and huge databases sized of 4..128 TiB.

Acknowledgments:

 - [Ledgerwatch](https://github.com/ledgerwatch), [Binance](https://github.com/binance-chain) and [Positive Technologies](https://www.ptsecurity.com/) teams for reporting, assistance in investigation and testing.
 - [Alex Sharov](https://github.com/AskAlexSharov) for reporting, testing and provide resources for remote debugging/investigation.
 - [Kris Zyp](https://github.com/kriszyp) for [Deno](https://deno.land/) support.

New features, extensions and improvements:

 - Added treating the `UINT64_MAX` value as maximum for given option inside `mdbx_env_set_option()`.
 - Added `to_hex/to_base58/to_base64::output(std::ostream&)` overloads without using temporary string objects as buffers.
 - Added `--geometry-jitter=YES|no` option to the test framework.
 - Added support for [Deno](https://deno.land/) support by [Kris Zyp](https://github.com/kriszyp).

Fixes:

 - Fixed handling `MDBX_opt_rp_augment_limit` for GC's records from huge transactions (Erigon/Akula/Ethereum).
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/issues/258) build on Android (avoid including `sys/sem.h`).
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/pull/261) missing copy assignment operator for `mdbx::move_result`.
 - Fixed missing `&` for `std::ostream &operator<<()` overloads.
 - Fixed unexpected `EXDEV` (Cross-device link) error from `mdbx_env_copy()`.
 - Fixed base64 encoding/decoding bugs in auxillary C++ API.
 - Fixed overflow of `pgno_t` during checking PNL on 64-bit platforms.
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/issues/260) excessive PNL checking after sort for spilling.
 - Reworked checking `MAX_PAGENO` and DB upper-size geometry limit.
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/issues/265) build for some combinations of versions of  MSVC and Windows SDK.

Minors:

 - Added workaround for CLANG bug [D79919/PR42445](https://reviews.llvm.org/D79919).
 - Fixed build test on Android (using `pthread_barrier_t` stub).
 - Disabled C++20 concepts for CLANG < 14 on Android.
 - Fixed minor `unused parameter` warning.
 - Added CI for Android.
 - Refine/cleanup internal logging.
 - Refined line splitting inside hex/base58/base64 encoding to avoid `\n` at the end.
 - Added workaround for modern libstdc++ with CLANG < 4.x
 - Relaxed txn-check rules for auxiliary functions.
 - Clarified a comments and descriptions, etc.
 - Using the `-fno-semantic interposition` option to reduce the overhead to calling self own public functions.


--------------------------------------------------------------------------------


## v0.11.3 at 2021-12-31

Acknowledgments:

 - [gcxfd <i@rmw.link>](https://github.com/gcxfd) for reporting, contributing and testing.
 - [장세연 (Чан Се Ен)](https://github.com/sasgas) for reporting and testing.
 - [Alex Sharov](https://github.com/AskAlexSharov) for reporting, testing and provide resources for remote debugging/investigation.

New features, extensions and improvements:

 - [Added](https://libmdbx.dqdkfa.ru/dead-github/issues/236) `mdbx_cursor_get_batch()`.
 - [Added](https://libmdbx.dqdkfa.ru/dead-github/issues/250) `MDBX_SET_UPPERBOUND`.
 - C++ API is finalized now.
 - The GC update stage has been [significantly speeded](https://libmdbx.dqdkfa.ru/dead-github/issues/254) when fixing huge Erigon's transactions (Ethereum ecosystem).

Fixes:

 - Disabled C++20 concepts for stupid AppleClang 13.x
 - Fixed internal collision of `MDBX_SHRINK_ALLOWED` with `MDBX_ACCEDE`.

Minors:

 - Fixed returning `MDBX_RESULT_TRUE` (unexpected -1) from `mdbx_env_set_option()`.
 - Added `mdbx_env_get_syncbytes()` and `mdbx_env_get_syncperiod()`.
 - [Clarified](https://libmdbx.dqdkfa.ru/dead-github/pull/249) description of `MDBX_INTEGERKEY`.
 - Reworked/simplified `mdbx_env_sync_internal()`.
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/issues/248) extra assertion inside `mdbx_cursor_put()` for `MDBX_DUPFIXED` cases.
 - Avoiding extra looping inside `mdbx_env_info_ex()`.
 - Explicitly enabled core dumps from stochastic tests scripts on Linux.
 - [Fixed](https://libmdbx.dqdkfa.ru/dead-github/issues/253) `mdbx_override_meta()` to avoid false-positive assertions.
 - For compatibility reverted returning `MDBX_ENODATA`for some cases.


--------------------------------------------------------------------------------


## v0.11.2 at 2021-12-02

Acknowledgments:

 - [장세연 (Чан Се Ен)](https://github.com/sasgas) for contributing to C++ API.
 - [Alain Picard](https://github.com/castortech) for [Java bindings](https://github.com/castortech/mdbxjni).
 - [Alex Sharov](https://github.com/AskAlexSharov) for reporting and testing.
 - [Kris Zyp](https://github.com/kriszyp) for reporting and testing.
 - [Artem Vorotnikov](https://github.com/vorot93) for support [Rust wrapper](https://github.com/vorot93/libmdbx-rs).

Fixes:

 - [Fixed compilation](https://libmdbx.dqdkfa.ru/dead-github/pull/239) with `devtoolset-9` on CentOS/RHEL 7.
 - [Fixed unexpected `MDBX_PROBLEM` error](https://libmdbx.dqdkfa.ru/dead-github/issues/242) because of update an obsolete meta-page.
 - [Fixed returning `MDBX_NOTFOUND` error](https://libmdbx.dqdkfa.ru/dead-github/issues/243) in case an inexact value found for `MDBX_GET_BOTH` operation.
 - [Fixed compilation](https://libmdbx.dqdkfa.ru/dead-github/issues/245) without kernel/libc-devel headers.

Minors:

 - Fixed `constexpr`-related macros for legacy compilers.
 - Allowed to define 'CMAKE_CXX_STANDARD` using an environment variable.
 - Simplified collection statistics of page operation .
 - Added `MDBX_FORCE_BUILD_AS_MAIN_PROJECT` cmake option.
 - Remove unneeded `#undef P_DIRTY`.


--------------------------------------------------------------------------------


## v0.11.1 at 2021-10-23

### Backward compatibility break:

The database format signature has been changed to prevent
forward-interoperability with an previous releases, which may lead to a
[false positive diagnosis of database corruption](https://libmdbx.dqdkfa.ru/dead-github/issues/238)
due to flaws of an old library versions.

This change is mostly invisible:

 - previously versions are unable to read/write a new DBs;
 - but the new release is able to handle an old DBs and will silently upgrade ones.

Acknowledgments:

 - [Alex Sharov](https://github.com/AskAlexSharov) for reporting and testing.


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
