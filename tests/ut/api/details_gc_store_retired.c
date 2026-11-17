/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* V-003 regression (issue #495, the PR description): the gc_store_retired
 * store loop must tolerate the retired-pages growth that happens inside the
 * RESERVE window of a GC write - the chunk accounting must use the
 * post-growth state and never underflow. The hook-replacement harness (the
 * crafted lists, the cursor_put growth stub and the full case matrix) lives
 * in the testing API (mdbx_tst_gc_store_retired_cases(), tests/api.h) -
 * this unit drives it and asserts the verdict. */

#include "mdbx.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  const int failed = mdbx_tst_gc_store_retired_cases();
  if (failed) {
    fprintf(stderr, "FAILED %d checks\n", failed);
    return 1;
  }
  return 0;
}
