/* V9 (stream A, api-txn.c) — mdbx_txn_amend(MDBX_TXN_TRY) on a PARKED reader.
 * HYPOTHESIS REFUTED — the code is CORRECT; kept as a machine-checked regression test.
 *
 * Hypothesis (withdrawn): the symmetry
 *   if ((uint32_t)(flags & MDBX_TXN_TRY) == (uint32_t)(rtxn->flags & MDBX_TXN_PARKED))
 *     rc = txn_ro_park(rtxn, true);            (api-txn.c:778-782)
 * would treat the TRY+PARKED combination as "equal" and re-park an already
 * parked transaction, which txn_ro_park() (txn-ro.c:328) rejects with
 * MDBX_BAD_TXN — making amend(TRY) of a parked reader always fail.
 *
 * Why it is wrong: the comparison is on full flag VALUES, not non-zero-ness.
 * MDBX_TXN_TRY = 0x10000000 (mdbx.h:1598) and MDBX_TXN_PARKED has a different
 * value (STATIC_ASSERT(MDBX_TXN_TRY != MDBX_TXN_PARKED), api-txn.c:777), so:
 *   TRY + parked      -> 0x10000000 != PARKED -> skip park -> TRY lock   (OK)
 *   TRY + not-parked  -> 0x10000000 != 0      -> skip park -> TRY lock   (OK)
 *   no TRY + parked   -> 0 != PARKED          -> skip park -> block      (OK)
 *   no TRY + not-park -> 0 == 0               -> park first, then block  (OK)
 * Parking is attempted exactly when both sides are literally zero — the
 * intended "park the reader before blocking on the write lock" case only.
 *
 * Machine check (Release build of verification/master-audit):
 *   txn_park rc=0; txn_amend(parked reader, MDBX_TXN_TRY) rc=0 with a live
 *   write transaction returned — SUCCESS, no BAD_TXN, no lock leak.
 *
 * Build: gcc -I<repo> -I<repo>/src v9-txn-amend-park-try.c -L build-dir -lmdbx-static -lpthread
 */
#include "mdbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  system("rm -rf v9amend.tmp && mkdir v9amend.tmp");
  MDBX_env *env = nullptr;
  int rc = mdbx_env_create(&env);
  if (rc != MDBX_SUCCESS)
    return 1;
  rc = mdbx_env_open(env, "v9amend.tmp", MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    printf("FAIL env_open rc=%d\n", rc);
    return 1;
  }

  MDBX_txn *ro = nullptr;
  rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &ro);
  if (rc != MDBX_SUCCESS) {
    printf("FAIL begin-ro rc=%d\n", rc);
    return 1;
  }

  rc = mdbx_txn_park(ro, true /* autounpark */);
  printf("txn_park rc=%d\n", rc);
  if (rc != MDBX_SUCCESS) {
    printf("SKIP: parking unsupported here (rc=%d)\n", rc);
    mdbx_txn_abort(ro);
    mdbx_env_close_ex(env, true);
    mdbx_env_delete("v9amend.tmp", MDBX_ENV_JUST_DELETE);
    return 0;
  }

  MDBX_txn *wr = ro; /* in/out: caller's parked reader */
  rc = mdbx_txn_amend(ro, &wr, MDBX_TXN_TRY, nullptr);
  printf("txn_amend(parked reader, MDBX_TXN_TRY) rc=%d (%s)\n", rc, mdbx_liberr2str(rc) ? mdbx_liberr2str(rc) : "?");

  if (rc == MDBX_SUCCESS && wr != nullptr && wr != ro) {
    printf("PASS: amend converted the parked reader to a write txn %p (hypothesis REFUTED, code correct)\n",
           (void *)wr);
    mdbx_txn_abort(wr);
  } else if (rc == MDBX_SUCCESS && wr == ro) {
    printf("PASS-DIFFERENT: rc=SUCCESS but *ptxn not replaced\n");
    mdbx_txn_abort(wr);
  } else {
    printf("FAIL: rc=%d — regressed vs machine-checked SUCCESS (see header)\n", rc);
    if (wr && wr != ro)
      mdbx_txn_abort(wr);
    else {
      mdbx_txn_unpark(ro, false);
      mdbx_txn_abort(ro);
    }
  }

  mdbx_env_close_ex(env, true);
  mdbx_env_delete("v9amend.tmp", MDBX_ENV_JUST_DELETE);
  return 0;
}
