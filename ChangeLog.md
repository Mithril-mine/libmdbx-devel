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

### Fixes:

 - Fixed a rare durability bug: a failed disk write during a commit could be silently reported as success, and on Windows a large write-mapped durable transaction could crash the engine while committing (issue #47).
 - Fixed `mdbx_chk` crashing on databases with named sub-tables when using `-i`/`-s` (issue #49).
 - Fixed `mdbx_load` failing with `ENOENT` when reading a dump from standard input (issue #50).
 - Fixed a spurious internal assertion failure during page allocation on Windows (issue #52).
 - Fixed an arithmetic overflow that miscomputed the maximum database size on 32-bit builds with large RAM.
 - Fixed a rare internal structure corruption during rollback of a duplicate insertion (unreachable in normal use, triggerable only by crafted internal sequences).
 - Fixed the `MDBX_TXN_TRY` flag leaking into transaction state flags.
 - Fixed `mdbx_dump -p`/`mdbx_load` silently losing data on round-trip when a key or value starts with a space; such a space is now properly escaped (issue #61).
 - Fixed `mdbx_replace_ex()` with overlapping buffers possibly hanging or crashing the next operation on the same sub-table (issue #56).
 - Fixed a crafted database file that passed validation and could corrupt data or crash when deleted (issue #57).
 - Fixed MSVC+AddressSanitizer builds failing to compile (issue #62).


--------------------------------------------------------------------------------


## v0.15.0 at 2026-09-22

Not a release, but a technical tag marking the start of development for a new version branch.

The engineering of subsequent releases of the v0.14.x version line will continue in the `stable` branch.
In the `master` branch, development of version v0.15 will begin, with the technical version tag set to v0.15.0.


********************************************************************************

For early releases and changes see the ChangeLog-NN the git commit history.
