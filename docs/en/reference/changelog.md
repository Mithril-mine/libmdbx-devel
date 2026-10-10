ChangeLog
=========

The source code is available on [SourceCraft](https://sourcecraft.dev/dqdkfa/libmdbx) and mirror on [GitHub](https://github.com/Mithril-mine/libmdbx).
Please use the `stable` branch or the latest release for production environments through staging, and the `master` branch for development of derivative projects.
Всё будет хорошо!


## v0.15.1 at a deep development stage

### Improvements:

 - Minor clarification/refining README and doxygen API descriptons.

### Fixes:

 - Fixed missed propagation of a mid-commit flush error in `iov_page()` — a failed write could be silently reported as success.
 - Fixed Windows ioring corruption when committing large write-mapped durable transactions — could crash in `ior_wocr()` under a live APC.
 - Fixed `mdbx_chk` crash when `-i`/`-s` filtering out named sub-tables (issue #49).
 - Fixed `mdbx_load` reading a dump from standard input by default — `-f` no longer consumes the database path argument (issue #50).
 - Fixed a wrong `env->incore` assertion in `gc_alloc_ex()` (issue #52).
 - Fixed `MDBX_TXN_TRY` leakage into transaction state flags.
 - Fixed typos in the ChangeLog.


--------------------------------------------------------------------------------


## v0.15.0 at 2026-09-22

Not a release, but a technical tag marking the start of development for a new version branch.

### Important:

The development of subsequent releases in the v0.14.x version line will continue in the `stable` branch.
In the `master` branch, development of version v0.15 will begin, with the technical version tag set to v0.15.0.


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
