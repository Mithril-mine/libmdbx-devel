# Doxygen semantic review of the public C API (mdbx.h) — phase 2 record

> Working artifact of TASK-11 phase 2 (`docs` agent). Scope: semantic accuracy of the public
> doxygen documentation in `mdbx.h`. Companion to the mechanical audit (structure/`\param`/`\return`,
> which found no defects). Blocks reviewed this session; each spot-check compares the doc text
> against actual behavior/values known from the source.

## Method

- Compare doc claims (semantics, defaults, guarantees, error codes) with the implementation and with
  verified values (options defaults, format constants, recovery/durability semantics).
- Doc-only fixes are applied directly on a feature branch (coordinator rule v2.2); functional
  changes would go through review (none were needed).

## Groups/blocks reviewed

| Block | Result |
| --- | --- |
| SYNC MODES group (`sync_modes`): DURABLE/NOMETASYNC/SAFE_NOSYNC/UTTERLY_NOSYNC, two classes of failures, steady semantics | accurate |
| `mdbx_env_set_geometry()`: param semantics (size_lower/now/upper, growth_step, shrink_threshold default 2×growth_step), Windows specifics (SRWL, single-process shrink, Native API), UNABLE_EXTEND_MAPSIZE handling | accurate |
| `mdbx_env_set_warmup` flags: `MDBX_warmup_flags_t` = default(0)/lock/touchlimit/release | accurate (flag names corrected earlier per REVIEW) |
| value2key / key2value groups: ordering semantics, invertibility, key lengths (8/4 bytes), JSON-integer RFC-7159 range, decode clamping | accurate (per-function docs rewritten and verified vs `api-key-transform.c`) |
| Options block "Default is …" values: max_readers (~100 per 4K), loose_limit=64, dp_reserve_limit=1024, txn_dp_initial=1024, txn_dp_limit=1/42 RAM, spill_max/min_denominator=8, spill_parent4child=0 | accurate |
| `MDBX_opt_merge_threshold` (12.5%..50% ↔ 8192..32768 16.16 units) and `prefer_waf_insteadof_balance` merge-choice semantics | accurate |
| `MDBX_opt_writethrough_threshold` (write-through vs write+fdatasync, only SYNC_DURABLE w/o WRITEMAP, not on Windows) | accurate |
| `mdbx_txn_info` space fields (dirty/leftover/retired/limit), `scan_rlt` | accurate |
| `mdbx_cache_get` statuses (HIT/CONFIRMED/REFRESHED/DIRTY/BEHIND/UNABLE/RACE) and entry model {trunk_txnid, last_confirmed_txnid, offset, length} | accurate |
| put/del "Quick Reference" matrix (NOOVERWRITE/UPSERT/CURRENT/ALLDUPS, EMULTIVAL, mdbx_replace semantics) | accurate |
| Error-code docs sampled (MDBX_THREAD_MISMATCH, MDBX_TXN_OVERLAPPING, MDBX_BAD_RSLOT, MDBX_BUSY, MDBX_EMULTIVAL, MDBX_DANGLING_DBI, MDBX_READERS_FULL=-30790) | accurate |

## Findings

No semantic inaccuracies found in the reviewed blocks of `mdbx.h`. The header's documentation is in
good shape after prior typo/copy-paste fixes and the value2key/key2value rewrite.

## Not covered (follow-up)

- Line-by-line audit of all remaining `mdbx.h` groups (c_opening, c_transactions, c_dbi, c_crud,
  c_cursors, c_statinfo, c_settings, c_debug, c_rqest, c_extra, chk) — sampled only here.
- `mdbx.h++` and `mdbx++/*` semantic review — deferred; recommend a dedicated pass reusing this
  method (doc claim vs implementation) and, where useful, an explore-agent mechanical layer for
  undocumented members.
- Recommendation: run `doxygen -Werror` on the full docs build to catch residual dangling refs;
  current docs build has EXTRACT_ALL=YES and includes options.h, so build-option `\ref`s resolve.