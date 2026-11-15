/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Repro for F-V12-1 (tranche V12, docs/verification/doc-additions-v12.md §2):
/// mdbx_load silently swallows a dump line at an embedded NUL byte.
///
/// Self-contained: writes the crafted dump `v12-nul-swallow.dump` into the
/// current directory. Run against a build of the tools:
///
///     mdbx_load fresh.db < v12-nul-swallow.dump ; echo "exit=$?"
///     mdbx_dump fresh.db
///
/// Expected (buggy) behavior: load exits 0 and the dump contains the record
///   "" -> "555"   (hex lines: " " and " 353535")
/// while the bytes "ZZJUNKTAIL" after the NUL are consumed from stdin and
/// silently discarded. Expected (fixed) behavior: the NUL line trips badend()
/// and the load fails (nonzero exit, nothing committed).
///
/// Root cause: src/tools/load.c readline() stores the byte after the leading
/// space into line[0]; for c == 0 the subsequent strlen() is 0, so the
/// growth/strip/decode stages are all skipped and out->iov_len = 0, while
/// fgets() already consumed the rest of the physical line. Any other invalid
/// byte reaches badend(); NUL is the only silent-swallow case. The same
/// strlen()-based pattern in readhdr() truncates `database=na\0me` to `na`.

#include <stdio.h>

int main(void) {
  static const char body[] = "VERSION=3\n"        /* line 1: global header */
                             "format=bytevalue\n" /* line 2 */
                             "type=btree\n"       /* line 3 */
                             "HEADER=END\n"       /* line 4 */
                             " 616263\n"          /* line 5: key "abc" */
                             " 313233\n"          /* line 6: value "123" */
                             " \0ZZJUNKTAIL\n"    /* line 7: NUL at data position -- THE BUG TRIGGER */
                             " 353535\n"          /* line 8: becomes the value of the empty key */
                             "DATA=END\n";        /* line 9 */
  FILE *f = fopen("v12-nul-swallow.dump", "wb");
  if (!f)
    return 1;
  /* fwrite with an explicit length: sizeof(body) would stop at the NUL. */
  fwrite(body, 1, (size_t)(sizeof(body) - 1), f);
  return fclose(f) != 0;
}
