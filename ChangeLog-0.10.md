## v0.10.5 at 2021-10-13 (obsolete, please use v0.11.1)

Unfortunately, the `v0.10.5` accidentally comes not full-compatible with previous releases:

 - `v0.10.5` can read/processing DBs created by previous releases, i.e. the backward-compatibility is provided;
 - however, previous releases may lead to false-corrupted state with DB that was touched by `v0.10.5`, i.e. the forward-compatibility is broken for `v0.10.4` and earlier.

This cannot be fixed, as it requires fixing past versions, which as a result we will just get a current version.
Therefore, it is recommended to use `v0.11.1` instead of `v0.10.5`.

Acknowledgments:

 - [Noel Kuntze](https://github.com/Thermi) for immediately bug reporting.

Fixes:

 - Fixed unaligned access regression after the `#pragma pack` fix for modern compilers.
 - Added UBSAN-test to CI to avoid a regression(s) similar to lately fixed.
 - Fixed possibility of meta-pages clashing after manually turn to a particular meta-page using `mdbx_chk` utility.

Minors:

 - Refined handling of weak or invalid meta-pages while a DB opening.
 - Refined providing information for the `@MAIN` and `@GC` sub-databases of a last committed modification transaction's ID.


--------------------------------------------------------------------------------


## v0.10.4 at 2021-10-10

Acknowledgments:

 - [Artem Vorotnikov](https://github.com/vorot93) for support [Rust wrapper](https://github.com/vorot93/libmdbx-rs).
 - [Andrew Ashikhmin](https://github.com/yperbasis) for contributing to C++ API.

Fixes:

 - Fixed possibility of looping update GC during transaction commit (no public issue since the problem was discovered inside [Positive Technologies](https://www.ptsecurity.ru)).
 - Fixed `#pragma pack` to avoid provoking some compilers to generate code with [unaligned access](https://libmdbx.dqdkfa.ru/dead-github/issues/235).
 - Fixed `noexcept` for potentially throwing `txn::put()` of C++ API.

Minors:

 - Added stochastic test script for checking small transactions cases.
 - Removed extra transaction commit/restart inside test framework.
 - In debugging builds fixed a too small (single page) by default DB shrink threshold.


--------------------------------------------------------------------------------


## v0.10.3 at 2021-08-27

Acknowledgments:

 - [Francisco Vallarino](https://github.com/fjvallarino) for [Haskell bindings for libmdbx](https://hackage.haskell.org/package/libmdbx).
 - [Alex Sharov](https://github.com/AskAlexSharov) for reporting and testing.
 - [Andrea Lanfranchi](https://github.com/AndreaLanfranchi) for contributing.

Extensions and improvements:

 - Added `cursor::erase()` overloads for `key` and for `key-value`.
 - Resolve minor Coverity Scan issues (no fixes but some hint/comment were added).
 - Resolve minor UndefinedBehaviorSanitizer issues (no fixes but some workaround were added).

Fixes:

 - Always setup `madvise` while opening DB (fixes https://libmdbx.dqdkfa.ru/dead-github/issues/231).
 - Fixed checking legacy `P_DIRTY` flag (`0x10`) for nested/sub-pages.

Minors:

 - Fixed getting revision number from middle of history during amalgamation (GNU Makefile).
 - Fixed search GCC tools for LTO (CMake scripts).
 - Fixed/reorder dirs list for search CLANG tools for LTO (CMake scripts).
 - Fixed/workarounds for CLANG < 9.x
 - Fixed CMake warning about compatibility with 3.8.2


--------------------------------------------------------------------------------


## v0.10.2 at 2021-07-26

Acknowledgments:

 - [Alex Sharov](https://github.com/AskAlexSharov) for reporting and testing.
 - [Andrea Lanfranchi](https://github.com/AndreaLanfranchi) for reporting bugs.
 - [Lionel Debroux](https://github.com/debrouxl) for fuzzing tests and reporting bugs.
 - [Sergey Fedotov](https://github.com/SergeyFromHell/) for [`node-mdbx` NodeJS bindings](https://www.npmjs.com/package/node-mdbx).
 - [Kris Zyp](https://github.com/kriszyp) for [`lmdbx-store` NodeJS bindings](https://github.com/kriszyp/lmdbx-store).
 - [Noel Kuntze](https://github.com/Thermi) for [draft Python bindings](https://libmdbx.dqdkfa.ru/dead-github/commits/python-bindings).

New features, extensions and improvements:

 - [Allow to predefine/override `MDBX_BUILD_TIMESTAMP` for builds reproducibility](https://libmdbx.dqdkfa.ru/dead-github/issues/201).
 - Added options support for `long-stochastic` script.
 - Avoided `MDBX_TXN_FULL` error for large transactions when possible.
 - The `MDBX_READERS_LIMIT` increased to `32767`.
 - Raise `MDBX_TOO_LARGE` under Valgrind/ASAN if being opened DB is 100 larger than RAM (to avoid hangs and OOM).
 - Minimized the size of poisoned/unpoisoned regions to avoid Valgrind/ASAN stuck.
 - Added more workarounds for QEMU for testing builds for 32-bit platforms, Alpha and Sparc architectures.
 - `mdbx_chk` now skips iteration & checking of DB' records if corresponding page-tree is corrupted (to avoid `SIGSEGV`, ASAN failures, etc).
 - Added more checks for [rare/fuzzing corruption cases](https://libmdbx.dqdkfa.ru/dead-github/issues/217).

Backward compatibility break:

 - Use file `VERSION.txt` for version information instead of `VERSION` to avoid collision with `#include <version>`.
 - Rename `slice::from/to_FOO_bytes()` to `slice::envisage_from/to_FOO_length()'.
 - Rename `MDBX_TEST_EXTRA` make's variable to `MDBX_SMOKE_EXTRA`.
 - Some details of the C++ API have been changed for subsequent freezing.

Fixes:

 - Fixed excess meta-pages checks in case `mdbx_chk` is called to check the DB for a specific meta page and thus could prevent switching to the selected meta page, even if the check passed without errors.
 - Fixed [recursive use of SRW-lock on Windows cause by `MDBX_NOTLS` option](https://libmdbx.dqdkfa.ru/dead-github/issues/203).
 - Fixed [log a warning during a new DB creation](https://libmdbx.dqdkfa.ru/dead-github/issues/205).
 - Fixed [false-negative `mdbx_cursor_eof()` result](https://libmdbx.dqdkfa.ru/dead-github/issues/207).
 - Fixed [`make install` with non-GNU `install` utility (OSX, BSD)](https://libmdbx.dqdkfa.ru/dead-github/issues/208).
 - Fixed [installation by `CMake` in special cases by complete use `GNUInstallDirs`'s variables](https://libmdbx.dqdkfa.ru/dead-github/issues/209).
 - Fixed [C++ Buffer issue with `std::string` and alignment](https://libmdbx.dqdkfa.ru/dead-github/issues/191).
 - Fixed `safe64_reset()` for platforms without atomic 64-bit compare-and-swap.
 - Fixed hang/shutdown on big-endian platforms without `__cxa_thread_atexit()`.
 - Fixed [using bad meta-pages if DB was partially/recoverable corrupted](https://libmdbx.dqdkfa.ru/dead-github/issues/217).
 - Fixed extra `noexcept` for `buffer::&assign_reference()`.
 - Fixed `bootid` generation on Windows for case of change system' time.
 - Fixed [test framework keygen-related issue](https://libmdbx.dqdkfa.ru/dead-github/issues/127).


--------------------------------------------------------------------------------


## v0.10.1 at 2021-06-01

Acknowledgments:

 - [Alexey Akhunov](https://github.com/AlexeyAkhunov) and [Alex Sharov](https://github.com/AskAlexSharov) for bug reporting and testing.
 - [Andrea Lanfranchi](https://github.com/AndreaLanfranchi) for bug reporting and testing related to WSL2.

New features:

 - Added `-p` option to `mdbx_stat` utility for printing page operations statistic.
 - Added explicit checking for and warning about using unfit github's archives.
 - Added fallback from [OFD locking](https://bit.ly/3yFRtYC) to legacy non-OFD POSIX file locks on an `EINVAL` error.
 - Added [Plan 9](https://en.wikipedia.org/wiki/9P_(protocol)) network file system to the whitelist for an ability to open a DB in exclusive mode.
 - Support for opening from WSL2 environment a DB hosted on Windows drive and mounted via [DrvFs](https://docs.microsoft.com/it-it/archive/blogs/wsl/wsl-file-system-support#drvfs) (i.e by Plan 9 noted above).

Fixes:

 - Fixed minor "foo not used" warnings from modern C++ compilers when building the C++ part of the library.
 - Fixed confusing/messy errors when build library from unfit github's archives (https://libmdbx.dqdkfa.ru/dead-github/issues/197).
 - Fixed `#​e​l​s​i​f` typo.
 - Fixed rare unexpected `MDBX_PROBLEM` error during altering data in huge transactions due to wrong spilling/oust of dirty pages (https://libmdbx.dqdkfa.ru/dead-github/issues/195).
 - Re-Fixed WSL1/WSL2 detection with distinguishing (https://libmdbx.dqdkfa.ru/dead-github/issues/97).


--------------------------------------------------------------------------------


## v0.10.0 at 2021-05-09

Acknowledgments:

 - [Mahlon E. Smith](https://github.com/mahlonsmith) for [Ruby bindings](https://rubygems.org/gems/mdbx/).
 - [Alex Sharov](https://github.com/AskAlexSharov) for [mdbx-go](https://github.com/torquem-ch/mdbx-go), bug reporting and testing.
 - [Artem Vorotnikov](https://github.com/vorot93) for bug reporting and PR.
 - [Paolo Rebuffo](https://www.linkedin.com/in/paolo-rebuffo-8255766/), [Alexey Akhunov](https://github.com/AlexeyAkhunov) and Mark Grosberg for donations.
 - [Noel Kuntze](https://github.com/Thermi) for preliminary [Python bindings](https://github.com/Thermi/libmdbx/tree/python-bindings)

New features:

 - Added `mdbx_env_set_option()` and `mdbx_env_get_option()` for controls
   various runtime options for an environment (announce of this feature  was missed in a previous news).
 - Added `MDBX_DISABLE_PAGECHECKS` build option to disable some checks to reduce an overhead
   and detection probability of database corruption to a values closer to the LMDB.
   The `MDBX_DISABLE_PAGECHECKS=1` provides a performance boost of about 10% in CRUD scenarios,
   and conjointly with the `MDBX_ENV_CHECKPID=0` and `MDBX_TXN_CHECKOWNER=0` options can yield
   up to 30% more performance compared to LMDB.
 - Using float point (exponential quantized) representation for internal 16-bit values
   of grow step and shrink threshold when huge ones (https://libmdbx.dqdkfa.ru/dead-github/issues/166).
   To minimize the impact on compatibility, only the odd values inside the upper half
   of the range (i.e. 32769..65533) are used for the new representation.
 - Added the `mdbx_drop` similar to LMDB command-line tool to purge or delete (sub)database(s).
 - [Ruby bindings](https://rubygems.org/gems/mdbx/) is available now by [Mahlon E. Smith](https://github.com/mahlonsmith).
 - Added `MDBX_ENABLE_MADVISE` build option which controls the use of POSIX `madvise()` hints and friends.
 - The internal node sizes were refined, resulting in a reduction in large/overflow pages in some use cases
   and a slight increase in limits for a keys size to ≈½ of page size.
 - Added to `mdbx_chk` output number of keys/items on pages.
 - Added explicit `install-strip` and `install-no-strip` targets to the `Makefile` (https://libmdbx.dqdkfa.ru/dead-github/pull/180).
 - Major rework page splitting (af9b7b560505684249b76730997f9e00614b8113) for
     - An "auto-appending" feature upon insertion for both ascending and
       descending key sequences. As a result, the optimality of page filling
       increases significantly (more densely, less slackness) while
       inserting ordered sequences of keys,
     - A "splitting at middle" to make page tree more balanced on average.
 - Added `mdbx_get_sysraminfo()` to the API.
 - Added guessing a reasonable maximum DB size for the default upper limit of geometry (https://libmdbx.dqdkfa.ru/dead-github/issues/183).
 - Major rework internal labeling of a dirty pages (958fd5b9479f52f2124ab7e83c6b18b04b0e7dda) for
   a "transparent spilling" feature with the gist to make a dirty pages
   be ready to spilling (writing to a disk) without further altering ones.
   Thus in the `MDBX_WRITEMAP` mode the OS kernel able to oust dirty pages
   to DB file without further penalty during transaction commit.
   As a result, page swapping and I/O could be significantly reduced during extra large transactions and/or lack of memory.
 - Minimized reading leaf-pages during dropping subDB(s) and nested trees.
 - Major rework a spilling of dirty pages to support [LRU](https://en.wikipedia.org/wiki/Cache_replacement_policies#Least_recently_used_(LRU))
   policy and prioritization for a large/overflow pages.
 - Statistics of page operations (split, merge, copy, spill, etc) now available through `mdbx_env_info_ex()`.
 - Auto-setup limit for length of dirty pages list (`MDBX_opt_txn_dp_limit` option).
 - Support `make options` to list available build options.
 - Support `make help` to list available make targets.
 - Silently `make`'s build by default.
 - Preliminary [Python bindings](https://github.com/Thermi/libmdbx/tree/python-bindings) is available now
   by [Noel Kuntze](https://github.com/Thermi) (https://libmdbx.dqdkfa.ru/dead-github/issues/147).

Backward compatibility break:

 - The `MDBX_AVOID_CRT` build option was renamed to `MDBX_WITHOUT_MSVC_CRT`.
   This option is only relevant when building for Windows.
 - The `mdbx_env_stat()` always, and `mdbx_env_stat_ex()` when called with the zeroed transaction parameter,
   now internally start temporary read transaction and thus may returns `MDBX_BAD_RSLOT` error.
   So, just never use deprecated `mdbx_env_stat()' and call `mdbx_env_stat_ex()` with transaction parameter.
 - The build option `MDBX_CONFIG_MANUAL_TLS_CALLBACK` was removed and now just a non-zero value of
   the `MDBX_MANUAL_MODULE_HANDLER` macro indicates the requirement to manually call `mdbx_module_handler()`
   when loading libraries and applications uses statically linked libmdbx on an obsolete Windows versions.

Fixes:

 - Fixed performance regression due non-optimal C11 atomics usage (https://libmdbx.dqdkfa.ru/dead-github/issues/160).
 - Fixed "reincarnation" of subDB after it deletion (https://libmdbx.dqdkfa.ru/dead-github/issues/168).
 - Fixed (disallowing) implicit subDB deletion via operations on `@MAIN`'s DBI-handle.
 - Fixed a crash of `mdbx_env_info_ex()` in case of a call for a non-open environment (https://libmdbx.dqdkfa.ru/dead-github/issues/171).
 - Fixed the selecting/adjustment values inside `mdbx_env_set_geometry()` for implicit out-of-range cases (https://libmdbx.dqdkfa.ru/dead-github/issues/170).
 - Fixed `mdbx_env_set_option()` for set initial and limit size of dirty page list ((https://libmdbx.dqdkfa.ru/dead-github/issues/179).
 - Fixed an unreasonably huge default upper limit for DB geometry (https://libmdbx.dqdkfa.ru/dead-github/issues/183).
 - Fixed `constexpr` specifier for the `slice::invalid()`.
 - Fixed (no)readahead auto-handling (https://libmdbx.dqdkfa.ru/dead-github/issues/164).
 - Fixed non-alloy build for Windows.
 - Switched to using Heap-functions instead of LocalAlloc/LocalFree on Windows.
 - Fixed `mdbx_env_stat_ex()` to returning statistics of the whole environment instead of MainDB only (https://libmdbx.dqdkfa.ru/dead-github/issues/190).
 - Fixed building by GCC 4.8.5 (added workaround for a preprocessor's bug).
 - Fixed building C++ part for iOS <= 13.0 (unavailability of  `std::filesystem::path`).
 - Fixed building for Windows target versions prior to Windows Vista (`WIN32_WINNT < 0x0600`).
 - Fixed building by MinGW for Windows (https://libmdbx.dqdkfa.ru/dead-github/issues/155).


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
