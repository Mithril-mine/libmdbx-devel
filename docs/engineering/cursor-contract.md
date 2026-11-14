# Cursor contract: states and the full list of cases

## Summary: marks for the `MDBX_SET` family

The `MDBX_SET` family has 40 rows: SET-1..6, SETK-1..6, SR-1..8, LB-1..11, UB-1..8, and C-1 for this family.
Marks: **20 keep, 11 change, 9 ask**. Rows, rules and reasons are in 4.3.1; the questions are in section 5.

| Row | Mark | Why, in one line |
|---|---|---|
| SR-3 | change | Past the last key from N or P: Xb (plain) or Xv (DUPSORT), while SR-4 gives Xa; proposed: always Xa. |
| SR-5 | change | Past the last key from an anchor on another leaf: Xb or Xv, while SR-4 gives Xa. |
| SR-8 | change | Over-long key: the state follows SR-3 and SR-5; the `MDBX_NOTFOUND` code is kept. |
| LB-4 | change | As SR-3. |
| LB-5 | change | As SR-5 for the other-leaf case; the rightmost-leaf case is already Xa. |
| LB-6 | change | Value above all values of the last key: reads as Xa, but `GET_CURRENT` without data returns the last key. |
| LB-11 | change | As SR-8. |
| UB-3 | change | Exact match on the last record: Xb, `GET_CURRENT` returns the bound itself; proposed: Xa. |
| UB-4 | change | As SR-3. |
| UB-5 | change | As LB-5. |
| UB-8 | change | As SR-8. |
| SET-2 | ask | A10: after a failed exact search `on_first`/`on_last` read a stale index; `mdbx.h` says TRUE. |
| SET-3 | ask | A10, as SET-2. |
| SET-6 | ask | A10 for the state; the `MDBX_NOTFOUND` code for an over-long key is a keep (M-2). |
| SETK-2 | ask | A10, as SET-2. |
| SETK-3 | ask | A10, as SET-2. |
| SETK-6 | ask | A10, as SET-6. |
| LB-9 | ask | A9: plain table, NULL data: `mdbx.h` says "as `MDBX_SET_RANGE`", which accepts it; the code returns `MDBX_EINVAL`. |
| UB-7 | ask | A9, as LB-9, for NULL data on a plain table. |
| LB-10 | ask | A11: DUPSORT, NULL value pointer: always `MDBX_RESULT_TRUE`, even for an exact key; or `MDBX_SUCCESS`. |

All eleven change rows get one rule: when nothing is found at or after the key, the cursor is past the end (Xa).
Today the same outcome leaves Xa, Xb or Xv, by the starting state, the table kind and the code path.
Xv looks like a defect: the cursor is past the end and `PREV` works, but `GET_CURRENT` says `MDBX_ENODATA`.

Corrections to the state model from the measurement:

- **New state Xv** (2.1, M-1): past the end on a DUPSORT table whose last key has several values, reached by a
  path that resets the values cursor first. It is the Xb encoding with the values cursor reset, so
  `GET_CURRENT` returns `MDBX_ENODATA` instead of the last record.
- **2.4 point 4 corrected:** deleting the last key of a DUPSORT table gives Xv, not Xb, when the new last key has
  several values.
- **2.2 corrected (M-5):** after a value delete (Vd, and Vt on a key that is not the last key) the outer probes
  read like D: `GET_CURRENT` and `NEXT` return the same record. The Vt row itself holds where measured; my first note misread its `NEXT_DUP` column.
- **Probe limits, not one state (2.4 point 8):** P and H on a one-key page (M-3), and N on an empty table against
  D0. Other calls separate them for the user.
- **M-2:** on tables with variable-length keys an over-long key gives `MDBX_NOTFOUND`, never `MDBX_BAD_VALSIZE`,
  for every read in this family. I read it as deliberate; I mark the return code keep.
- **M-11:** the program labels a few DUPSORT lines by intent, not by result; their states fit other rows, no
  mark changes.

Working draft for Leonid. It holds sections 4 to 6 of the approved proposal
("The cursor contract: how I propose to write it", 2026-11-13) for every call in scope:
the user-visible cursor states, and one row per call, starting state and outcome.

Status of this draft:

- Only the `MDBX_SET` family (4.3.1) and the SET part of C-1 are measured, on Windows and on Linux.
  Every other row of the column "State today (measured)" says "not measured".
- The columns "Proposed rule" and "Mark" are filled for the `MDBX_SET` family and the SET part of C-1.
  Elsewhere "Proposed rule" is empty, and "Mark" is empty except "ask?" where `mdbx.h` and the code
  visibly disagree. Each "ask?" has a note with the code reference. Section 5 lists all of them.
- Notes marked "inferred" come from reading the code. They are not findings.
  The measuring program confirms or refutes each of them.

All source references are to `master` @ `c26d8d4b`, written as `file:line @ c26d8d4b`.
I read every cited line at that commit. The proposal itself cited `ddedc7e3`.

Related issues: [#301](https://github.com/Mithril-mine/libmdbx-devel/issues/301),
[#304](https://github.com/Mithril-mine/libmdbx-devel/issues/304),
[#305](https://github.com/Mithril-mine/libmdbx-devel/issues/305),
[#313](https://github.com/Mithril-mine/libmdbx-devel/issues/313),
[#326](https://github.com/Mithril-mine/libmdbx-devel/issues/326),
and the pointer-lifetime contract [#388](https://github.com/Mithril-mine/libmdbx-devel/issues/388) as the pattern.

## 1. How I observe a state

A state is what the user can observe through the API. I observe it with a fixed probe set.
Each probe runs on a copy of the cursor under test, made with `mdbx_cursor_copy()`:

- `mdbx_cursor_eof()`, `mdbx_cursor_on_first()`, `mdbx_cursor_on_last()`,
  and for `MDBX_DUPSORT` tables `mdbx_cursor_on_first_dup()`, `mdbx_cursor_on_last_dup()`;
- `mdbx_cursor_get(MDBX_GET_CURRENT)`: which record, or which error;
- `mdbx_cursor_get(MDBX_NEXT)` and `mdbx_cursor_get(MDBX_PREV)`, each on its own fresh copy:
  where they go, or which error.

The copy is faithful. `mdbx_cursor_copy()` copies the whole internal state word and the page stack,
for the outer and the inner (duplicates) cursor
(`src/api-cursor.c:234-239 @ c26d8d4b`, `src/cursor-setup.c:449-463 @ c26d8d4b`).
So the probes do not move the cursor under test.

Two probes change the copy, and that is fine: `MDBX_NEXT`, `MDBX_PREV`, and also `MDBX_GET_CURRENT`
in the "after a delete" state (it moves the copy to the record it returns,
`src/cursor-seek.c:285-288 @ c26d8d4b`).

## 2. States, as the user sees them

### 2.1 Main states

The probe columns show what I expect from reading the code. The measuring program checks every cell.
"by index" means the result follows the stored position and is not fixed by the state.

| Code | State | `eof` | `GET_CURRENT` | `NEXT` | `PREV` | Internal encoding | Reached by (examples) |
|---|---|---|---|---|---|---|---|
| U | unbound | `MDBX_EINVAL` | `MDBX_EINVAL` | `MDBX_EINVAL` | `MDBX_EINVAL` | signature "ready for dispose" | `mdbx_cursor_create()`, `mdbx_cursor_unbind()`, end of the transaction, `mdbx_txn_release_all_cursors_ex(unbind=true)` |
| N | new | TRUE | `MDBX_ENODATA` | acts as `MDBX_FIRST` | acts as `MDBX_LAST` | stack empty, `z_fresh` set | `mdbx_cursor_open()`, `mdbx_cursor_bind()`, `mdbx_cursor_renew()`, `mdbx_cursor_reset()` |
| P | unpositioned | TRUE | `MDBX_ENODATA` | `MDBX_ENODATA` | `MDBX_ENODATA` | stack empty, no `z_fresh` | `mdbx_drop()`, `MDBX_DELETE_WHOLE`, left-over cursors of `mdbx_cursor_distribute()`, a move from D0 |
| H | unpositioned after a failed exact search | TRUE | `MDBX_ENODATA` | `MDBX_ENODATA` | `MDBX_ENODATA` | stack kept, `z_hollow` set | `MDBX_NOTFOUND` from `MDBX_SET`, `MDBX_SET_KEY`, `MDBX_GET_BOTH`, `MDBX_GET_BOTH_RANGE`, `MDBX_TO_EXACT_KEY_VALUE_*`, `MDBX_SEEK_AND_GET_MULTIPLE` |
| R | on a record | FALSE | that record | the next record | the previous record | no state flag | any successful positioning |
| E | at the end, still readable | TRUE | the last record | `MDBX_NOTFOUND` | the record before the last | `z_eof_soft` | `MDBX_LAST` (plain table), `MDBX_NEXT` that returns `MDBX_NOTFOUND` at the end |
| Xa | past the end, nothing readable | TRUE | `MDBX_NOTFOUND` | `MDBX_NOTFOUND` | the last record | `z_eof_soft` + `z_eof_hard`, index = number of keys | `MDBX_SET_RANGE` miss past the last key, from a cursor already on the rightmost leaf page |
| Xb | past the end, last record still readable | TRUE | the last record | `MDBX_NOTFOUND` | the last record | `z_eof_soft` + `z_eof_hard`, index = last key | `MDBX_SET_RANGE` miss past the last key through a tree search; delete of the last record; see notes |
| Xv | past the end, the last key has several values (`MDBX_DUPSORT` only) | TRUE | `MDBX_ENODATA` | `MDBX_NOTFOUND` | the last value of the last key | the Xb encoding, but the last key has several values and the inner (values) cursor is reset | a path to Xb that resets the values cursor first, on a last key with several values: `MDBX_SET_RANGE`, `MDBX_SET_LOWERBOUND`, `MDBX_SET_UPPERBOUND` past the last key through a tree search; delete of the last key (M-1) |
| D | after a delete | FALSE | the record after the deleted one | the same record | the record before the deleted one | `z_after_delete` | `mdbx_cursor_del()`; a delete of the record under the cursor through another cursor or `mdbx_del()` |
| D0 | after a delete that emptied the table | TRUE | `MDBX_ENODATA` | `MDBX_NOTFOUND` once, then P | `MDBX_NOTFOUND` once, then P | stack empty, `z_after_delete` | delete of the only record |

`mdbx_cursor_on_first()` and `mdbx_cursor_on_last()` return TRUE for N, P and D0, because the stack is empty
(`src/cursor-nav.c:331-346 @ c26d8d4b`). For H they are computed from a stale index (see 2.5).
For the other states they follow the index. For Xv (measured): `on_first` FALSE, `on_last` TRUE,
`on_first_dup` and `on_last_dup` TRUE.

Code references for the table:

- U: `src/cursor-setup.c:533-536 @ c26d8d4b` (calls that need a transaction return `MDBX_EINVAL`),
  `src/cursor-setup.c:264-271 @ c26d8d4b` (end of transaction makes the cursor unbound).
- N and P: `src/cursor.h:160-164 @ c26d8d4b`, `src/cursor.h:543-547 @ c26d8d4b`,
  `src/cursor-nav.c:161-170 @ c26d8d4b`.
- H: `src/cursor-seek.c:101-113 @ c26d8d4b`, `src/cursor-nav.c:199-201 @ c26d8d4b`.
- E, Xa, Xb: `src/cursor.h:55-86 @ c26d8d4b`, `src/cursor-nav.c:207-217 @ c26d8d4b`,
  `src/cursor-nav.c:266-283 @ c26d8d4b`, `src/cursor-seek.c:99-117 @ c26d8d4b`,
  `src/cursor-seek.c:279-284 @ c26d8d4b`.
- Xv: `src/cursor-seek.c:42 @ c26d8d4b`, `src/cursor-seek.c:150-154 @ c26d8d4b`, `src/cursor-nav.c:274-281 @ c26d8d4b`,
  `src/cursor-seek.c:300-306 @ c26d8d4b`, `src/cursor-mut.c:1112-1117 @ c26d8d4b`; the path is in M-1 (4.3.1).
- D, D0: `src/cursor-mut.c:1067-1097 @ c26d8d4b`, `src/cursor-nav.c:207-210 @ c26d8d4b`.

### 2.2 Position among the values of a key (`MDBX_DUPSORT` tables only)

On a `MDBX_DUPSORT` table the cursor has a second position: which value of the current key.
I write it after the main state, for example `R·Vm`. The main state is R or E, with one exception
(measured, M-5): after a value delete (Vd, and Vt on a key that is not the last key) the main probes read
like D. `GET_CURRENT` and `NEXT` return the same record, as the `mdbx.h` delete note promises
(`mdbx.h:6769-6771 @ c26d8d4b`), and `eof` is FALSE. So I write these positions as `D·Vd` and `D·Vt`;
rows below that say `R·Vd` or `R·Vt` mean the same position.

| Code | Position | `on_first_dup` / `on_last_dup` | `GET_CURRENT` | `NEXT_DUP` | `PREV_DUP` | Internal encoding |
|---|---|---|---|---|---|---|
| Vs | the key has a single value | TRUE / TRUE | the record | `MDBX_NOTFOUND` | `MDBX_NOTFOUND` | inner cursor not used |
| V1 | first of several values | TRUE / FALSE | that value | the next value | `MDBX_NOTFOUND` | inner index 0 |
| Vm | a middle value | FALSE / FALSE | that value | the next value | the previous value | inner on a value |
| Vn | last of several values | FALSE / TRUE | that value | `MDBX_NOTFOUND` | the previous value | inner on the last value |
| Vd | after deleting a value; more values follow | by index | the value after the deleted one | that same value | the value before the deleted one | inner `z_after_delete` |
| Vt | after deleting the last value; other values remain | FALSE / TRUE | the first value of the next key | `MDBX_NOTFOUND` | the last remaining value (inferred) | inner `z_inner_tail_deleted` + `z_eof_soft` + `z_eof_hard` |
| Vx | past the last value after a failed pair search | FALSE / TRUE | `MDBX_NOTFOUND` | `MDBX_NOTFOUND` | the last value (inferred) | inner `z_eof_soft` + `z_eof_hard`, index = number of values |

Measured for Vt (M-5): on a key that is not the last key, `on_first_dup` FALSE, `on_last_dup` TRUE,
`GET_CURRENT` and `NEXT` the first value of the next key, `PREV` the last remaining value. On the last key,
`GET_CURRENT` and `NEXT` return `MDBX_NOTFOUND`, `PREV` the last remaining value, `eof` FALSE.
The `NEXT_DUP` and `PREV_DUP` columns are not in the probe set; they stay inferred.

Vt and Vx look alike, but two calls tell them apart.
`MDBX_GET_CURRENT` returns the next record from Vt and `MDBX_NOTFOUND` from Vx
(`src/cursor-seek.c:289-293 @ c26d8d4b`, `src/cursor-seek.c:304-309 @ c26d8d4b`).
`mdbx_cursor_del()` is a no-op success from Vt and `MDBX_ENODATA` from Vx
(`src/cursor-mut.c:958-965 @ c26d8d4b`). This is the subject of [#301](https://github.com/Mithril-mine/libmdbx-devel/issues/301)
and [#305](https://github.com/Mithril-mine/libmdbx-devel/issues/305).
Vx is set at `src/cursor-seek.c:548-555 @ c26d8d4b` and `src/cursor-seek.c:615-625 @ c26d8d4b`.

### 2.3 States outside this contract

- **Transaction unusable.** The transaction is broken (`mdbx_txn_break()`), failed, parked without
  auto-unpark, or ousted. The cursor stays bound. Calls fail with `MDBX_BAD_TXN` or `MDBX_OUSTED`.
  This belongs to the failure-model contract.
- **Closed.** `mdbx_cursor_close()` frees the handle. Any later call is a use after free.
  One exception: a parent's cursor closed inside a nested transaction is kept until the nested
  transaction ends (`src/api-cursor.c:195-199 @ c26d8d4b`). Until then every call on it returns
  `MDBX_EBADSIGN` (`src/cursor-setup.c:533-535 @ c26d8d4b`). The user must not touch it anyway.

### 2.4 How this list differs from the proposal's candidates

1. **"new" is not reached by `mdbx_cursor_create()`.** A created cursor is unbound (U) until
   `mdbx_cursor_bind()`. Every probe returns `MDBX_EINVAL` on it (`src/api-cursor.c:10-27 @ c26d8d4b`,
   `src/cursor-setup.c:533-536 @ c26d8d4b`). N comes from open, bind, renew and reset
   (`src/cursor-setup.c:291 @ c26d8d4b`, `src/api-cursor.c:79 @ c26d8d4b`).
2. **"unpositioned" is two states, P and H.** The proposal mapped it to `z_hollow` only.
   P is the empty-stack encoding without `z_fresh` (`z_poor_mark`). H is `z_hollow` with the stack kept.
   Through `eof`, `GET_CURRENT`, `NEXT` and `PREV` they look identical.
   They differ in two visible ways. `on_first`/`on_last` read the stale index of H.
   And `mdbx_cursor_scroll()`, `mdbx_cursor_delete_range()`, `mdbx_cursor_distribute()` and the range
   actions of `mdbx_cursor_bunch_delete()` accept H but reject P with `MDBX_ENODATA` (section 5, A6).
3. **"past the end" is three states for the user: Xa, Xb and Xv.** All have the same flags (`z_eof_soft` +
   `z_eof_hard`). Xa and Xb differ only in the stored index. `GET_CURRENT` returns `MDBX_NOTFOUND` in Xa and
   the last record in Xb (`src/cursor-seek.c:279-284 @ c26d8d4b`). Xv is the Xb encoding on a last key with
   several values whose values cursor was reset on the way: `GET_CURRENT` returns `MDBX_ENODATA` (M-1). Which one the user gets depends on the
   starting state, the tree shape and the number of values of the last key (rows SR-3 to SR-5). Measured.
4. **"after a delete" is D and D0, plus Vd and Vt.** Deleting the last record of a non-empty table
   does not give D. It gives Xb, because `cursor_sibling_right()` overwrites `z_after_delete`
   (`src/cursor-mut.c:1112-1117 @ c26d8d4b`, `src/cursor-nav.c:280 @ c26d8d4b`). On a `MDBX_DUPSORT` table
   whose new last key has several values it gives Xv, not Xb: the same code then resets the inner cursor
   (`src/cursor-mut.c:1115 @ c26d8d4b`). Measured: Xb on the plain tables and on the DUPSORT multi table
   (new last key with one value), Xv on the DUPSORT small table (M-1). Deleting the only
   record gives D0: the first relative move returns `MDBX_NOTFOUND`, the second `MDBX_ENODATA`
   (`src/cursor-nav.c:161-170 @ c26d8d4b`).
5. **"forbidden" is U.** "Unbound" and "transaction ended" look identical to the user: the end of a
   transaction makes every cursor unbound (`src/cursor-setup.c:264-271 @ c26d8d4b`), and every probe
   returns `MDBX_EINVAL`.
6. **The `MDBX_DUPSORT` position is a second axis (2.2).** Vx has no flag of its own; it is an
   eof encoding of the inner cursor.
7. **Two states are named but left to other contracts (2.3).**
8. **Pairs the probe set cannot separate, or separates by one probe only:** E and "R on the last record"
   differ only by `eof`. P and N differ only by `NEXT`/`PREV`. P and H differ only by
   `on_first`/`on_last` (measured, M-6).
   Two pairs read the same in the measurement. Both are limits of the probe set, not one state for the user:
   - **P and H on a one-key page (M-3).** After a miss below the only key, the stale index is on that key, and
     `on_first` and `on_last` are TRUE for H too. The user still sees a difference: `mdbx_cursor_scroll()`, `mdbx_cursor_delete_range()`,
     `mdbx_cursor_distribute()` and the range actions of `mdbx_cursor_bunch_delete()` accept H and reject P
     (A6, inferred). If A6 and A10 follow `mdbx.h`, P and H become one state for the user.
   - **N on an empty table and D0.** One move on a copy cannot separate them. A second `MDBX_NEXT` on the same
     copy does: N answers `MDBX_NOTFOUND` again, because it keeps acting as `MDBX_FIRST`; D0 became P after the
     first move and answers `MDBX_ENODATA` (`src/cursor-nav.c:161-170 @ c26d8d4b`,
     `src/tree-search.c:45-49 @ c26d8d4b`). Inferred, not measured.

### 2.5 State-level disagreements

These are about the probe calls, not about one row. Section 5 lists them with the row marks.

- **A2. Meaning of `mdbx_cursor_eof()`.** `mdbx.h` says it "Determines whether the cursor is pointed to
  a key-value pair or not" and returns FALSE when "A data is available"
  (`mdbx.h:7029-7030 @ c26d8d4b`, `mdbx.h:7037-7039 @ c26d8d4b`). In state E the cursor points to the
  last record and `GET_CURRENT` reads it, yet `eof` is TRUE (`src/cursor.h:205-208 @ c26d8d4b`,
  `src/cursor-nav.c:117-119 @ c26d8d4b`). Inferred, to measure: after `MDBX_LAST` on a `MDBX_DUPSORT`
  table whose last key has several values, the outer cursor gets no eof flag, so `eof` is FALSE
  (`src/cursor-nav.c:84-108 @ c26d8d4b`). Then `eof` after `MDBX_LAST` depends on the data.
- **A10. `on_first`/`on_last` on H.** `mdbx.h` promises TRUE for a cursor "not positioned at all"
  (`mdbx.h:7056-7059 @ c26d8d4b`, `mdbx.h:7102-7105 @ c26d8d4b`). An H cursor keeps its stack and
  index (`src/cursor-seek.c:100-110 @ c26d8d4b`), and both calls read that index
  (`src/cursor-nav.c:331-346 @ c26d8d4b`). So they can return FALSE. Measured (M-6).

## 3. Table format and outcomes

Columns: ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark.
I added the ID column so notes and later steps can refer to a row.

Every row is measured on a plain table and on a `MDBX_DUPSORT` table, unless the call names one:
"(plain)", "(DUPSORT)" or "(DUPFIXED)".

Outcomes: success (including `MDBX_RESULT_TRUE` where the call returns it as a success),
`MDBX_NOTFOUND`, and the API-caused errors of the proposal: `MDBX_EINVAL`, `MDBX_ENODATA`,
`MDBX_KEYEXIST`, `MDBX_EACCESS` (a write on a read-only transaction), `MDBX_MAP_FULL`
(small database size). I also list `MDBX_EKEYMISMATCH`, `MDBX_INCOMPATIBLE` and `MDBX_BAD_VALSIZE`
where the user causes them through the API. They are not in the proposal's list. Do you want them out?

Rules I used to group starting states. A row says which rule applies.

- **G-abs.** `MDBX_FIRST` and `MDBX_LAST` descend from the root; they never read the old position
  (`src/cursor-nav.c:127-138 @ c26d8d4b`). The starting state does not matter.
- **G-seek.** Search operations read the old position only for a fast path
  (`src/cursor-seek.c:21-128 @ c26d8d4b`). It matters only where the cursor stops after a miss.
  So I use two classes: "no anchor" = N, P, D0 (empty stack), and "anchored" = H, R, E, Xa, Xb, D.
- **G-arg.** Argument checks run before the cursor is touched. The starting state does not matter;
  one row "any bound" is enough.
- **G-U.** Every call on U is covered once, in 4.0.
- **G-read.** Read-only operations cannot return `MDBX_KEYEXIST`, `MDBX_EACCESS` or `MDBX_MAP_FULL`.
  I do not list those outcomes for them.

"any bound" means every state of 2.1 except U.

## 4. Rows

### 4.0 Common rows

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| C-1 | every `mdbx_cursor_get()` operation, `mdbx_cursor_put()`, `mdbx_cursor_del()`, `mdbx_cursor_delete_range()`, `mdbx_cursor_bunch_delete()`, `mdbx_cursor_get_batch()`, `mdbx_cursor_scan()`, `mdbx_cursor_scan_from()`, `mdbx_cursor_scroll()`, `mdbx_cursor_distribute()`, `mdbx_cursor_copy()` as source | U | `MDBX_EINVAL` | "A cursor cannot be used ... when its transaction has ended, except with mdbx_cursor_bind() and mdbx_cursor_renew()" `mdbx.h:6090-6092 @ c26d8d4b` | SET family only: all five `MDBX_SET*` operations return `MDBX_EINVAL` on a created (unbound) cursor; same on both. The other calls: not measured | SET family: every operation on an unbound cursor returns `MDBX_EINVAL`; the cursor stays unbound (by code, not probed) | SET family: **keep**: matches `mdbx.h`, same on both. Other calls: not marked yet |
| C-2 | `mdbx_cursor_get()` with a value outside `MDBX_cursor_op` | any bound | `MDBX_EINVAL` | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| C-3 | any call | transaction unusable (2.3) | `MDBX_BAD_TXN` or `MDBX_OUSTED` | failure-model contract, not covered here | not measured | | |
| C-4 | any call | closed (2.3) | use after free | "must not be used again after this call" `mdbx.h:6238-6239 @ c26d8d4b` | not measured (cannot be measured safely) | | |

Note C-1: `src/cursor-setup.c:533-536 @ c26d8d4b`. The unbound check returns `MDBX_EINVAL` for every call that needs a transaction.

### 4.1 Absolute operations

Grouping: G-abs for `MDBX_FIRST`/`MDBX_LAST`. `MDBX_FIRST_DUP`/`MDBX_LAST_DUP` read only one thing:
is the outer cursor on a record (`src/cursor-seek.c:435-436 @ c26d8d4b`). The value position (V*) does not matter for them.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| ABS-1 | `MDBX_FIRST` | any bound | success | "Position at first key/data item" `mdbx.h:1812 @ c26d8d4b` | not measured | | |
| ABS-2 | `MDBX_FIRST` | any bound, empty table | `MDBX_NOTFOUND` | "MDBX_NOTFOUND No matching key found." `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| ABS-3 | `MDBX_LAST` | any bound | success | "Position at last key/data item" `mdbx.h:1834 @ c26d8d4b`; `eof` meaning `mdbx.h:7029-7030 @ c26d8d4b` | not measured | | ask? |
| ABS-4 | `MDBX_LAST` | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| ABS-5 | `MDBX_FIRST_DUP` | R, E, D | success | "MDBX_DUPSORT-only: Position at first data item of current key." `mdbx.h:1815 @ c26d8d4b` | not measured | | |
| ABS-6 | `MDBX_FIRST_DUP` | N, P, H, Xa, Xb, D0 | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| ABS-7 | `MDBX_FIRST_DUP` | any bound | `MDBX_EINVAL` (data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| ABS-8 | `MDBX_LAST_DUP` | R, E, D | success | "MDBX_DUPSORT-only: Position at last data item of current key." `mdbx.h:1837 @ c26d8d4b` | not measured | | |
| ABS-9 | `MDBX_LAST_DUP` | N, P, H, Xa, Xb, D0 | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| ABS-10 | `MDBX_LAST_DUP` | any bound | `MDBX_EINVAL` (data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |

Notes:

- ABS-2, ABS-4, inferred: the cursor is not changed on an empty table; N stays N (`src/tree-search.c:45-49 @ c26d8d4b`).
- ABS-3, ask? (A2): see 2.5. Code: `src/cursor-nav.c:117-119 @ c26d8d4b`, `src/cursor-nav.c:137 @ c26d8d4b`.
- ABS-5, ABS-8, inferred: from D they act on the record after the deleted one. On a plain table they return the record (`src/cursor-seek.c:440-441 @ c26d8d4b`).
- ABS-6, ABS-9: N does not act as `MDBX_FIRST` here, unlike `MDBX_NEXT_DUP` from N (REL-30).

### 4.2 Relative operations

Every state matters here. No grouping beyond G-U and G-read. These operations accept NULL key and data, so `MDBX_EINVAL` comes only from C-1 and C-2.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| REL-1 | `MDBX_NEXT` | N | success (acts as `MDBX_FIRST`) | "Position at next data item" `mdbx.h:1840 @ c26d8d4b`; after reset "does not allow relative positioning operations" `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| REL-2 | `MDBX_NEXT` | N, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| REL-3 | `MDBX_NEXT` | P, H | `MDBX_ENODATA` | mdbx.h is silent (`MDBX_ENODATA` is not listed, `mdbx.h:6443-6448 @ c26d8d4b`) | not measured | | |
| REL-4 | `MDBX_NEXT` | R, not on the last record | success | `mdbx.h:1840 @ c26d8d4b` | not measured | | |
| REL-5 | `MDBX_NEXT` | R on the last record | `MDBX_NOTFOUND` | mdbx.h is silent on the state; `eof` meaning `mdbx.h:7029-7030 @ c26d8d4b` | not measured | | ask? |
| REL-6 | `MDBX_NEXT` | E | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-7 | `MDBX_NEXT` | Xa | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-8 | `MDBX_NEXT` | Xb | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-9 | `MDBX_NEXT` | D | success | "Both MDBX_NEXT and MDBX_GET_CURRENT will return the same record after this operation." `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | |
| REL-10 | `MDBX_NEXT` | D0 | `MDBX_NOTFOUND` | `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | ask? |
| REL-11 | `MDBX_NEXT` (DUPSORT) | R·V1, R·Vm | success (next value) | `mdbx.h:1840 @ c26d8d4b` | not measured | | |
| REL-12 | `MDBX_NEXT` (DUPSORT) | R·Vn, R·Vs, not on the last key | success (first value of the next key) | `mdbx.h:1840 @ c26d8d4b` | not measured | | |
| REL-13 | `MDBX_NEXT` (DUPSORT) | R·Vn, R·Vs, on the last key | `MDBX_NOTFOUND` | `eof` meaning `mdbx.h:7029-7030 @ c26d8d4b` | not measured | | ask? |
| REL-14 | `MDBX_NEXT` (DUPSORT) | R·Vd | success | `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | |
| REL-15 | `MDBX_NEXT` (DUPSORT) | R·Vt, R·Vx, not on the last key | success (first value of the next key) | mdbx.h is silent | not measured | | |
| REL-16 | `MDBX_NEXT` (DUPSORT) | R·Vt, R·Vx, on the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-17 | `MDBX_PREV` | N | success (acts as `MDBX_LAST`) | "Position at previous data item" `mdbx.h:1854 @ c26d8d4b`; `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| REL-18 | `MDBX_PREV` | N, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| REL-19 | `MDBX_PREV` | P, H | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| REL-20 | `MDBX_PREV` | R, not on the first record | success | `mdbx.h:1854 @ c26d8d4b` | not measured | | |
| REL-21 | `MDBX_PREV` | R on the first record | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-22 | `MDBX_PREV` | E | success | mdbx.h is silent | not measured | | |
| REL-23 | `MDBX_PREV` | Xa, Xb | success | mdbx.h is silent | not measured | | |
| REL-24 | `MDBX_PREV` | D | success | mdbx.h is silent (the delete note covers `MDBX_NEXT` and `MDBX_GET_CURRENT` only, `mdbx.h:6769-6771 @ c26d8d4b`) | not measured | | |
| REL-25 | `MDBX_PREV` | D0 | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-26 | `MDBX_PREV` (DUPSORT) | R·Vm, R·Vn | success (previous value) | `mdbx.h:1854 @ c26d8d4b` | not measured | | |
| REL-27 | `MDBX_PREV` (DUPSORT) | R·V1, R·Vs, not on the first key | success (last value of the previous key) | `mdbx.h:1854 @ c26d8d4b` | not measured | | |
| REL-28 | `MDBX_PREV` (DUPSORT) | R·V1, R·Vs, on the first key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-29 | `MDBX_PREV` (DUPSORT) | R·Vd, R·Vt, R·Vx | success | mdbx.h is silent | not measured | | |
| REL-30 | `MDBX_NEXT_DUP` | N | success (acts as `MDBX_FIRST`) | "Position at next data item of current key." `mdbx.h:1843 @ c26d8d4b`; `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| REL-31 | `MDBX_NEXT_DUP` | N, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| REL-32 | `MDBX_NEXT_DUP` | P, H | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| REL-33 | `MDBX_NEXT_DUP` (DUPSORT) | R·V1, R·Vm, R·Vd | success | `mdbx.h:1843 @ c26d8d4b` | not measured | | |
| REL-34 | `MDBX_NEXT_DUP` (DUPSORT) | R·Vn, R·Vs, R·Vt, R·Vx | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-35 | `MDBX_NEXT_DUP` | E, Xa, Xb, D | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-36 | `MDBX_NEXT_DUP` | D0 | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-37 | `MDBX_NEXT_DUP` (plain) | R | `MDBX_NOTFOUND` | "MDBX_DUPSORT-only" `mdbx.h:1843 @ c26d8d4b` | not measured | | |
| REL-38 | `MDBX_PREV_DUP` | N | success (acts as `MDBX_LAST`) | "Position at previous data item of current key." `mdbx.h:1857 @ c26d8d4b`; `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| REL-39 | `MDBX_PREV_DUP` | N, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| REL-40 | `MDBX_PREV_DUP` | P, H | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| REL-41 | `MDBX_PREV_DUP` (DUPSORT) | R·Vm, R·Vn, R·Vd, R·Vt, R·Vx | success | `mdbx.h:1857 @ c26d8d4b` | not measured | | |
| REL-42 | `MDBX_PREV_DUP` (DUPSORT) | R·V1, R·Vs | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-43 | `MDBX_PREV_DUP` | E, Xa, Xb, D, D0 | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-44 | `MDBX_PREV_DUP` (plain) | R | `MDBX_NOTFOUND` | "MDBX_DUPSORT-only" `mdbx.h:1857 @ c26d8d4b` | not measured | | |
| REL-45 | `MDBX_NEXT_NODUP` | N | success (acts as `MDBX_FIRST`) | "Position at first data item of next key" `mdbx.h:1851 @ c26d8d4b`; `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| REL-46 | `MDBX_NEXT_NODUP` | N, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| REL-47 | `MDBX_NEXT_NODUP` | P, H | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| REL-48 | `MDBX_NEXT_NODUP` | R (any V*), not on the last key | success | `mdbx.h:1851 @ c26d8d4b` | not measured | | |
| REL-49 | `MDBX_NEXT_NODUP` | R (any V*), on the last key | `MDBX_NOTFOUND` | `eof` meaning `mdbx.h:7029-7030 @ c26d8d4b` | not measured | | ask? |
| REL-50 | `MDBX_NEXT_NODUP` | E, Xa, Xb | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-51 | `MDBX_NEXT_NODUP` | D | success | mdbx.h is silent (the delete note names `MDBX_NEXT` only) | not measured | | |
| REL-52 | `MDBX_NEXT_NODUP` | D0 | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-53 | `MDBX_PREV_NODUP` | N | success (acts as `MDBX_LAST`) | "Position at last data item of previous key" `mdbx.h:1860 @ c26d8d4b`; `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| REL-54 | `MDBX_PREV_NODUP` | N, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| REL-55 | `MDBX_PREV_NODUP` | P, H | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| REL-56 | `MDBX_PREV_NODUP` | R (any V*), not on the first key | success | `mdbx.h:1860 @ c26d8d4b` | not measured | | |
| REL-57 | `MDBX_PREV_NODUP` | R (any V*), on the first key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-58 | `MDBX_PREV_NODUP` | E, Xa, Xb, D | success | mdbx.h is silent | not measured | | |
| REL-59 | `MDBX_PREV_NODUP` | D0 | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-60 | `MDBX_GET_CURRENT` | N, P, H | `MDBX_ENODATA` | "Return key/data at current cursor position" `mdbx.h:1825 @ c26d8d4b` | not measured | | |
| REL-61 | `MDBX_GET_CURRENT` | D0 | `MDBX_ENODATA` | `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | ask? |
| REL-62 | `MDBX_GET_CURRENT` | R (V1, Vm, Vn, Vs) | success | `mdbx.h:1825 @ c26d8d4b` | not measured | | |
| REL-63 | `MDBX_GET_CURRENT` | E, Xb | success (the last record) | mdbx.h is silent | not measured | | |
| REL-64 | `MDBX_GET_CURRENT` | Xa | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-65 | `MDBX_GET_CURRENT` | D | success | `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | |
| REL-66 | `MDBX_GET_CURRENT` (DUPSORT) | R·Vd | success (the value after the deleted one) | `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | |
| REL-67 | `MDBX_GET_CURRENT` (DUPSORT) | R·Vt, not on the last key | success (first value of the next key) | mdbx.h is silent | not measured | | |
| REL-68 | `MDBX_GET_CURRENT` (DUPSORT) | R·Vt, on the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| REL-69 | `MDBX_GET_CURRENT` (DUPSORT) | R·Vx | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |

Notes:

- REL-1, REL-17, REL-30, REL-38, REL-45, REL-53, ask? (A1): `mdbx_cursor_reset()` gives N, and from N every relative move acts as `MDBX_FIRST` or `MDBX_LAST` (`src/api-cursor.c:79 @ c26d8d4b`, `src/cursor.h:543-547 @ c26d8d4b`, `src/cursor-nav.c:161-168 @ c26d8d4b`). The same N comes from open and bind, where `mdbx.h` is silent. This is the example from the proposal.
- REL-5, REL-13, REL-49, ask? (A2): after this miss the cursor is in E: `eof` is TRUE but `GET_CURRENT` still reads the last record (`src/cursor-nav.c:266-283 @ c26d8d4b`, then `src/cursor-seek.c:413-418 @ c26d8d4b` clears `z_eof_hard`). This is the two-readings case of `src/cursor.h:55-71 @ c26d8d4b`.
- REL-10, REL-61, ask? (A3): after deleting the only record, `MDBX_NEXT` returns `MDBX_NOTFOUND` and `MDBX_GET_CURRENT` returns `MDBX_ENODATA` (`src/cursor-mut.c:1089-1097 @ c26d8d4b`, `src/cursor-nav.c:169-170 @ c26d8d4b`, `src/cursor-seek.c:279-281 @ c26d8d4b`). `mdbx.h` promises the same result from both.
- REL-8, inferred: `MDBX_NEXT` from Xb turns Xb into E (`src/cursor-seek.c:417 @ c26d8d4b` clears `z_eof_hard` when the index is on a key). After that, `MDBX_PREV` goes to the second-last record instead of the last.
- REL-21, REL-28, REL-57, inferred: a miss at the beginning leaves the cursor on the first record with `eof` FALSE (`src/cursor-nav.c:253-264 @ c26d8d4b`). A miss at the end sets `eof` (REL-5). The two ends are not symmetric.
- REL-35, REL-43: `src/cursor-nav.c:204-205 @ c26d8d4b` returns `MDBX_NOTFOUND` for the `_DUP` moves in every edge state.
- REL-60 to REL-69: `src/cursor-seek.c:277-320 @ c26d8d4b`. REL-65 and REL-66 move the cursor to the returned record (`src/cursor-seek.c:285-288 @ c26d8d4b`).

### 4.3 Search operations

#### 4.3.1 The `MDBX_SET` family (measured)

Grouping: G-seek, G-arg. The "rightmost leaf page" split matters only for a miss past the last key
(`src/cursor-seek.c:93-117 @ c26d8d4b`). To get both cases the measuring program needs a table with one leaf page and a table with several.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| SET-1 | `MDBX_SET` | any bound | success | "Position at specified key" `mdbx.h:1863 @ c26d8d4b` | R (DUPSORT: R·V1 on a key with several values, R·Vs on a one-value key); same on both; every starting state | The cursor is on the key (DUPSORT: on its first value), whatever the starting state | **keep**: matches `mdbx.h`; one result from every start, on both platforms |
| SET-2 | `MDBX_SET` | no anchor, key absent, table not empty | `MDBX_NOTFOUND` | "MDBX_NOTFOUND No matching key found." `mdbx.h:6447 @ c26d8d4b` | H; same on both. On a leaf page with one key the probes cannot tell it from P (M-3) | `MDBX_NOTFOUND`; the cursor is unpositioned (H): `eof` TRUE; `GET_CURRENT`, `NEXT`, `PREV` return `MDBX_ENODATA` | **ask** (A10): `mdbx.h` says `on_first`/`on_last` are TRUE when not positioned; on H they read a stale index and can be FALSE (M-6) |
| SET-3 | `MDBX_SET` | anchored, key absent | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | H; same on both; every anchored starting state | As SET-2 (H), from every anchored start | **ask** (A10): `mdbx.h` says `on_first`/`on_last` are TRUE when not positioned; on H they read a stale index and can be FALSE (M-6) |
| SET-4 | `MDBX_SET` | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | cursor unchanged (N stays N, D0 stays D0; all probes equal the start); same on both | `MDBX_NOTFOUND`; the cursor is not changed (N stays N, D0 stays D0) | **keep**: there is nothing to position on; matches `mdbx.h` |
| SET-5 | `MDBX_SET` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | rc `MDBX_EINVAL`, cursor unchanged in every starting state; same on both | `MDBX_EINVAL`; the cursor is not changed | **keep**: the argument check runs before any move; matches `mdbx.h` |
| SET-6 | `MDBX_SET` | any bound | `MDBX_BAD_VALSIZE` (key length outside the table limits) | mdbx.h is silent | rc `MDBX_NOTFOUND`, not `MDBX_BAD_VALSIZE`: the long key is searched like any absent key (M-2). State: H as in SET-2 and SET-3; unchanged on an empty table; same on both | A key longer than the table limit is searched like any absent key: `MDBX_NOTFOUND`, state as SET-2, SET-3, SET-4. A wrong length for a fixed-size or `MDBX_INTEGERKEY` key stays `MDBX_BAD_VALSIZE` | **ask** (A10) for the state, as SET-2; the return code is a keep (M-2) |
| SETK-1 | `MDBX_SET_KEY` | any bound | success | "Position at specified key, return both key and data" `mdbx.h:1866 @ c26d8d4b` | R (DUPSORT: R·V1 on a key with several values, R·Vs on a one-value key); same on both; every starting state | As SET-1; the key and its (first) value are returned | **keep**: matches `mdbx.h`; one result from every start, on both platforms |
| SETK-2 | `MDBX_SET_KEY` | no anchor, key absent, table not empty | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | H; same on both. On a leaf page with one key the probes cannot tell it from P (M-3) | As SET-2 (H) | **ask** (A10): `mdbx.h` says `on_first`/`on_last` are TRUE when not positioned; on H they read a stale index and can be FALSE (M-6) |
| SETK-3 | `MDBX_SET_KEY` | anchored, key absent | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | H; same on both; every anchored starting state | As SET-3 (H) | **ask** (A10): `mdbx.h` says `on_first`/`on_last` are TRUE when not positioned; on H they read a stale index and can be FALSE (M-6) |
| SETK-4 | `MDBX_SET_KEY` | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | cursor unchanged (N stays N, D0 stays D0; all probes equal the start); same on both | As SET-4: the cursor is not changed | **keep**: there is nothing to position on; matches `mdbx.h` |
| SETK-5 | `MDBX_SET_KEY` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | rc `MDBX_EINVAL`, cursor unchanged in every starting state; same on both | `MDBX_EINVAL`; the cursor is not changed | **keep**: the argument check runs before any move; matches `mdbx.h` |
| SETK-6 | `MDBX_SET_KEY` | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | rc `MDBX_NOTFOUND`, not `MDBX_BAD_VALSIZE`: the long key is searched like any absent key (M-2). State: H as in SETK-2 and SETK-3; unchanged on an empty table; same on both | As SET-6 | **ask** (A10) for the state, as SET-2; the return code is a keep (M-2) |
| SR-1 | `MDBX_SET_RANGE` | any bound | success (exact key) | "Position at first key greater than or equal to specified key." `mdbx.h:1869 @ c26d8d4b` | R (DUPSORT: R·V1 or R·Vs); same on both | The cursor is on the key (DUPSORT: on its first value) | **keep**: matches `mdbx.h` |
| SR-2 | `MDBX_SET_RANGE` | any bound | success (greater key) | `mdbx.h:1869 @ c26d8d4b` | R on the first greater key (DUPSORT: R·V1 or R·Vs); same on both | The cursor is on the first greater key (DUPSORT: on its first value) | **keep**: matches `mdbx.h` |
| SR-3 | `MDBX_SET_RANGE` | no anchor, key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent on the state | plain: Xb (as inferred); DUPSORT: Xv (M-1); same on both. Measured from N and from P | `MDBX_NOTFOUND`; past the end (Xa): `eof` TRUE, `GET_CURRENT` and `NEXT` `MDBX_NOTFOUND`, `PREV` the last record (DUPSORT: the last value of the last key). The same from every starting state, tree shape and table kind | **change**: today Xb or Xv by table kind and the last key's values, while SR-4 gives Xa; Xv is a defect (`GET_CURRENT` says `MDBX_ENODATA` on a positioned cursor) |
| SR-4 | `MDBX_SET_RANGE` | anchored on the rightmost leaf page, key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent on the state | Xa, plain and DUPSORT (as inferred); same on both | As SR-3 (Xa) | **keep**: already the proposed state, on both table kinds |
| SR-5 | `MDBX_SET_RANGE` | anchored on another leaf page, key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent on the state | plain: Xb (as inferred); DUPSORT: Xv (M-1); same on both | As SR-3 (Xa) | **change**: today Xb (plain) or Xv (DUPSORT), while SR-4 gives Xa for the same key |
| SR-6 | `MDBX_SET_RANGE` | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | cursor unchanged (N stays N, D0 stays D0); same on both | `MDBX_NOTFOUND`; the cursor is not changed | **keep**: there is nothing to position on; matches `mdbx.h` |
| SR-7 | `MDBX_SET_RANGE` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | rc `MDBX_EINVAL`, cursor unchanged in every starting state; same on both | `MDBX_EINVAL`; the cursor is not changed | **keep**: the argument check runs before any move; matches `mdbx.h` |
| SR-8 | `MDBX_SET_RANGE` | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | rc `MDBX_NOTFOUND`, not `MDBX_BAD_VALSIZE` (M-2). State as for a key past the last key (SR-3, SR-4, SR-5); unchanged on an empty table; same on both | A key longer than the limit is searched like any other key: the first greater key if one exists (inferred, not measured), else `MDBX_NOTFOUND` and Xa as SR-3. Fixed-size and `MDBX_INTEGERKEY` keys: as SET-6 | **change** for the state, as SR-3 and SR-5; the return code is a keep (M-2) |
| LB-1 | `MDBX_SET_LOWERBOUND` | any bound | `MDBX_SUCCESS` (exact key; DUPSORT: exact pair) | "returns MDBX_SUCCESS if key found exactly or MDBX_RESULT_TRUE if greater key was found" `mdbx.h:1881-1888 @ c26d8d4b` | R, rc success (DUPSORT: R·V1, R·Vm, R·Vn or R·Vs by the pair found); same on both | `MDBX_SUCCESS`; the cursor is on the key (DUPSORT: on the pair) | **keep**: matches `mdbx.h` |
| LB-2 | `MDBX_SET_LOWERBOUND` | any bound | `MDBX_RESULT_TRUE` (greater key or pair) | `mdbx.h:1881-1888 @ c26d8d4b` | R, rc `MDBX_RESULT_TRUE`; same on both. DUPSORT: also when the key exists and the value is below all its values (first value comes back with `MDBX_RESULT_TRUE`) | `MDBX_RESULT_TRUE`; the cursor is on the first greater key or pair (DUPSORT: a value below all values of the key gives its first value) | **keep**: matches `mdbx.h` |
| LB-3 | `MDBX_SET_LOWERBOUND` (DUPSORT) | any bound; key found, value greater than all its values, a next key exists | `MDBX_RESULT_TRUE` (first value of the next key) | "the next pair was returned" `mdbx.h:1887-1888 @ c26d8d4b` | R·Vs (the first value of the next key), rc `MDBX_RESULT_TRUE`; same on both | `MDBX_RESULT_TRUE`; the cursor is on the first value of the next key | **keep**: "the next pair was returned", as `mdbx.h` says |
| LB-4 | `MDBX_SET_LOWERBOUND` | no anchor, key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent on this outcome | plain: Xb; DUPSORT: Xv (M-1); same on both | `MDBX_NOTFOUND`; Xa, as SR-3 | **change**: today Xb or Xv, as SR-3 |
| LB-5 | `MDBX_SET_LOWERBOUND` | anchored (on the rightmost leaf page, or on another leaf page), key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent on this outcome | Xa, plain and DUPSORT. From an anchor on another leaf page: plain Xb, DUPSORT: Xv (M-1); same on both | `MDBX_NOTFOUND`; Xa from every anchor, as SR-4 and SR-5 | **change**: the other-leaf case in this cell gives Xb or Xv; the rightmost case is already Xa |
| LB-6 | `MDBX_SET_LOWERBOUND` (DUPSORT) | any bound; key is the last key, value greater than all its values | `MDBX_NOTFOUND` | mdbx.h is silent on this outcome | Xa from every starting state, N included (a key past the last key from N gives Xb, LB-4); same on both | `MDBX_NOTFOUND`; Xa from every start | **change**: the probes read Xa, but the encoding is Xb with the values cursor past its end: `GET_CURRENT` with a NULL data pointer returns the last key (Xa: `MDBX_NOTFOUND`) |
| LB-7 | `MDBX_SET_LOWERBOUND` | any bound, empty table | `MDBX_NOTFOUND` | mdbx.h is silent on this outcome | cursor unchanged (N stays N, D0 stays D0); same on both | `MDBX_NOTFOUND`; the cursor is not changed | **keep**: there is nothing to position on |
| LB-8 | `MDBX_SET_LOWERBOUND` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | rc `MDBX_EINVAL`, cursor unchanged in every starting state; same on both | `MDBX_EINVAL`; the cursor is not changed | **keep**: the argument check runs before any move; matches `mdbx.h` |
| LB-9 | `MDBX_SET_LOWERBOUND` (plain) | any bound | `MDBX_EINVAL` (data is NULL) | "For non DUPSORT-ed collections this work the same to MDBX_SET_RANGE" `mdbx.h:1881-1883 @ c26d8d4b` | rc `MDBX_EINVAL`, cursor unchanged in every starting state (confirms A9); same on both | Version 1: NULL data is allowed on a plain table, as the code allows it for `MDBX_SET_RANGE`. Version 2 (code): `MDBX_EINVAL`, cursor not changed, and `mdbx.h` says so | **ask** (A9): `mdbx.h` says "the same to `MDBX_SET_RANGE`" but also "return both key and data"; the code rejects NULL data (M-7) |
| LB-10 | `MDBX_SET_LOWERBOUND` (DUPSORT) | any bound; data given with a NULL pointer | success or `MDBX_RESULT_TRUE`, value ignored | mdbx.h is silent | rc `MDBX_RESULT_TRUE`, not success: R·V1 on the first value of the key, the NULL pointer acts as a value below all values; same on both | Version 1 (code): a NULL value pointer is below all values: the first value of the key, always `MDBX_RESULT_TRUE`. Version 2: it means "any value": `MDBX_SUCCESS` when the key matches exactly, as on a plain table | **ask** (A11, M-8): `mdbx.h` is silent; the row expected success for an exact key, the code never returns it |
| LB-11 | `MDBX_SET_LOWERBOUND` | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | rc `MDBX_NOTFOUND`, not `MDBX_BAD_VALSIZE` (M-2). State as for a key past the last key (LB-4, LB-5); unchanged on an empty table; same on both | As SR-8: searched like any other key; on a miss past the last key, Xa as LB-4 and LB-5 | **change** for the state, as LB-4 and LB-5; the return code is a keep (M-2) |
| UB-1 | `MDBX_SET_UPPERBOUND` | any bound | success (greater key or pair found) | "returns MDBX_SUCCESS if the greater key was found or MDBX_NOTFOUND otherwise" `mdbx.h:1895-1902 @ c26d8d4b` | R, rc success (DUPSORT: R·Vs, R·V1 or R·Vn); same on both | `MDBX_SUCCESS`; the cursor is on the first greater key or pair | **keep**: matches `mdbx.h` |
| UB-2 | `MDBX_SET_UPPERBOUND` | any bound; exact match, a next record exists | success (the next record) | `mdbx.h:1895-1902 @ c26d8d4b` | R on the next record, rc success (DUPSORT: the next value, or the first value of the next key); same on both | `MDBX_SUCCESS`; the cursor is on the next record (DUPSORT: the next value, or the first value of the next key) | **keep**: matches `mdbx.h` |
| UB-3 | `MDBX_SET_UPPERBOUND` | any bound; exact match on the last record | `MDBX_NOTFOUND` | `mdbx.h:1895-1902 @ c26d8d4b` | plain: Xb (as inferred); DUPSORT: Xb·Vs when the pair is the last record ("Vs" is a probe artefact: the `_dup` probes are TRUE when the outer cursor is not filled; the last key has three values); Xa when the value is above all values of the last key (the case of LB-6); same on both | `MDBX_NOTFOUND`; Xa, as SR-3 | **change**: today Xb (the internal next step keeps `z_eof_hard`), so `GET_CURRENT` returns the record equal to the bound, while UB-5 gives Xa for the same "nothing greater" |
| UB-4 | `MDBX_SET_UPPERBOUND` | no anchor, key past the last key | `MDBX_NOTFOUND` | `mdbx.h:1895-1902 @ c26d8d4b` | plain: Xb; DUPSORT: Xv (M-1); same on both | `MDBX_NOTFOUND`; Xa, as SR-3 | **change**: today Xb or Xv, as SR-3 |
| UB-5 | `MDBX_SET_UPPERBOUND` | anchored (on the rightmost leaf page, or on another leaf page), key past the last key | `MDBX_NOTFOUND` | `mdbx.h:1895-1902 @ c26d8d4b` | Xa, plain and DUPSORT. From an anchor on another leaf page: plain Xb, DUPSORT: Xv (M-1); same on both | `MDBX_NOTFOUND`; Xa from every anchor, as SR-4 and SR-5 | **change**: the other-leaf case in this cell gives Xb or Xv; the rightmost case is already Xa |
| UB-6 | `MDBX_SET_UPPERBOUND` | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:1895-1902 @ c26d8d4b` | cursor unchanged (N stays N, D0 stays D0); same on both | `MDBX_NOTFOUND`; the cursor is not changed | **keep**: there is nothing to position on; matches `mdbx.h` |
| UB-7 | `MDBX_SET_UPPERBOUND` | any bound | `MDBX_EINVAL` (key or data is NULL) | `mdbx.h:6448 @ c26d8d4b` | rc `MDBX_EINVAL` (key NULL or data NULL), cursor unchanged in every starting state; same on both | Key NULL: `MDBX_EINVAL`, cursor not changed. Data NULL on a plain table: as LB-9 (two versions) | **ask** (A9) for NULL data on a plain table: `mdbx.h` says "like `MDBX_SET_RANGE`" but also "return both key and data"; key NULL and DUPSORT data NULL are a keep |
| UB-8 | `MDBX_SET_UPPERBOUND` | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | rc `MDBX_NOTFOUND`, not `MDBX_BAD_VALSIZE` (M-2). State as for a key past the last key (UB-4, UB-5); unchanged on an empty table; same on both | As SR-8: on a miss past the last key, Xa as UB-4 and UB-5 | **change** for the state, as UB-4 and UB-5; the return code is a keep (M-2) |

Notes:

- SET-2, SET-3, SETK-2, SETK-3, inferred: the cursor goes to H (`src/cursor-seek.c:101-113 @ c26d8d4b`, `src/cursor-seek.c:147-148 @ c26d8d4b`). The stored index differs between the two classes; that matters for A10 and A6.
- SET-4, SR-6, LB-7, UB-6, inferred: the cursor is not changed; N stays N (`src/tree-search.c:45-49 @ c26d8d4b`).
- SR-3 and SR-5, inferred: Xb (`src/cursor-seek.c:150-154 @ c26d8d4b`, `src/cursor-nav.c:266-283 @ c26d8d4b`). SR-4, inferred: Xa (`src/cursor-seek.c:99-117 @ c26d8d4b`). So the same miss leaves a different state depending on the starting state.
- UB-3, inferred: Xb. The cursor moves with an internal next step that, unlike `MDBX_NEXT`, does not clear `z_eof_hard` (`src/cursor-seek.c:485-487 @ c26d8d4b` against `src/cursor-seek.c:413-418 @ c26d8d4b`). `MDBX_NEXT` from the same record gives E (REL-5).
- LB-9, ask? (A9): the code returns `MDBX_EINVAL` for a NULL data pointer on every table (`src/cursor-seek.c:451-452 @ c26d8d4b`); `MDBX_SET_RANGE` accepts it (`src/cursor-seek.c:333-334 @ c26d8d4b`).
- LB-10: the code comment says this changes the meaning on purpose (`src/cursor-seek.c:459-462 @ c26d8d4b`).
- Marks, the change rows (SR-3, SR-5, SR-8, LB-4, LB-5, LB-6, LB-11, UB-3, UB-4, UB-5, UB-8) share one rule: a call
  that finds nothing at or after the given key or pair leaves one "past the end" state. Today it is Xa, Xb or Xv,
  through three code paths: the fast path (Xa), the tree search and `cursor_sibling_right()` (Xb, Xv), and an
  internal next step that keeps `z_eof_hard` (UB-3: Xb; LB-6: Xb with the values cursor past its end, which reads
  as Xa). I propose Xa for all of them. The code's own design says so: after a
  failed range search the cursor "may/should" stand after the last row (`src/cursor.h:72-74 @ c26d8d4b`), and with
  `z_eof_hard` no read at the current position is allowed (`src/cursor.h:84-86 @ c26d8d4b`). SR-4 and LB-6 already
  give Xa for the probe set. Xb lets `GET_CURRENT`, and by inference `mdbx_cursor_put(MDBX_CURRENT)` (PUT-8), act on a record that did
  not match the search. Xv is a defect in any reading. The alternative, Xb everywhere, would make `GET_CURRENT` read
  the last record from Xa and Xv too; I do not propose it.
- Marks, the ask rows: A10 (SET-2, SET-3, SET-6, SETK-2, SETK-3, SETK-6), A9 (LB-9, UB-7), A11 (LB-10). Section 5
  gives both versions of each.

##### Measurement notes (the `MDBX_SET` family)

Program: `tests/ut/cursor/contract-measure.c++` (gtest `ut_cursor_contract_measure.set_family`).
It prints one line per scenario (row ID, table, starting state, call, return code, raw probes, state)
and asserts only the setup steps. It ran 4837 scenarios on Windows (MSVC, Debug) and on Linux (gcc 11, Debug,
WSL, ext4). Both runs printed 4956 lines. A line-by-line comparison shows no difference: **no Windows/Linux
difference in any SET-family row**. No crash, no assertion inside the library.

Tables. The page size is read from the environment (4096 on both platforms); no code compares it to a constant.

- plain table, small: 5 keys, one leaf page (depth 1). Plain table, multi: 60 keys, values of one sixth of a page,
  depth 2 (the program asserts depth >= 2 in setup).
- DUPSORT table, small: 5 keys, depth 1. DUPSORT table, multi: 60 keys, depth 2. Every odd-index key and the
  last key have three values (`a`, `b`, `c`, padded); the other keys have one value.
  I did not check whether any value set became a nested tree.
- One-key tables (plain, DUPSORT): used only for D0 (delete the only key) and for N and R on the only key.
  The plain one holds one record; the DUPSORT one holds one key with three values, because the last key of
  every DUPSORT table has three values.
- Empty tables (plain, DUPSORT): N on an empty table.
- Keys in a table are `k00010`, `k00020`, ...; absent keys are `k00005` (below), a key between, `k99999` (above).
  The key too long is one byte over `mdbx_env_get_maxkeysize_ex()`.

Starting states, each reached with public calls on a fresh cursor in a write transaction (aborted after the scenario).
The program prints the state it recognises before the call, so a recipe that misses its target is visible.

| Start | Recipe | State seen before the call |
|---|---|---|
| N | `mdbx_cursor_open()` | N (on an empty table the probes cannot tell N from D0) |
| P | `MDBX_SET` on the middle key, `mdbx_drop(del=false)`, put the same records back | P (one-key page: P or H, see M-3) |
| H low, mid, high | `MDBX_SET` on an absent key below, between, above | H |
| R first, mid, near last, last | `MDBX_SET` on that key | R (DUPSORT: R·Vs or R·V1) |
| E after LAST | `MDBX_LAST` | plain: E. DUPSORT: R·Vn (M-4) |
| E after NEXT | `MDBX_LAST`, then `MDBX_NEXT` returns `MDBX_NOTFOUND`; variant: `MDBX_SET` on the last key (DUPSORT: then `MDBX_LAST_DUP`), then `MDBX_NEXT` | E (DUPSORT: E·Vn) |
| Xr from N, from R last, from R first | `MDBX_SET_RANGE` on `k99999` | plain multi: Xb, Xa, Xb; plain small: Xb, Xa, Xa. DUPSORT multi: Xv (M-1), Xa, Xv; DUPSORT small: Xv, Xa, Xa |
| Xub last record | `MDBX_SET_UPPERBOUND` on the last pair | Xb |
| D mid, first | `MDBX_SET`, `mdbx_cursor_del()` (DUPSORT: `MDBX_ALLDUPS`) | D |
| D last | the same on the last key | plain: Xb. DUPSORT multi: Xb·Vs. DUPSORT small: Xv (M-1) |
| D0 | the same on the only record | D0 |
| DUPSORT Rv1, Rvm, Rvn, Rvs | `MDBX_SET`, then `MDBX_NEXT_DUP` or `MDBX_LAST_DUP` | R·V1, R·Vm, R·Vn, R·Vs |
| Vd | on the second value, `mdbx_cursor_del(MDBX_CURRENT)` | D·Vn (the value after the deleted one, as 2.2 says) |
| Vt | on the last value of a non-last key, `mdbx_cursor_del(MDBX_CURRENT)` | printed D·Vn; it is D·Vt (M-5) |
| Vt on the last key | the same on the last key | `GET_CURRENT` `MDBX_NOTFOUND`, `NEXT` `MDBX_NOTFOUND`, `PREV` the last remaining value, `eof` FALSE |
| Vx | `MDBX_TO_PAIR_EQUAL` on a value that is absent between two values | Vx (`GET_CURRENT` `MDBX_NOTFOUND`, `NEXT` the first value of the next key, `PREV` the last value) |

"Anchor" for SR-4/SR-5 (and LB, UB): an anchor is on the rightmost leaf page when it comes from the last
keys (H high, R near last and last, E, X*, D last, the DUPSORT starts on the last key) or when the table has
one leaf page. Other starts are on another leaf page in the multi tables.

What the measurement says about the grouping rules of section 3. For `MDBX_SET`, `MDBX_SET_KEY` and the
success rows, the result does not depend on the starting state at all. Only the "past the last key" rows
(SR-3 to SR-5, LB-4, LB-5, UB-4, UB-5, and the long key) depend on it, and only through the anchor.
So G-seek holds as written: "no anchor" against "anchored on the rightmost leaf" against "anchored elsewhere".

Contradictions with the state model, with the "ask?" inferences, and with the outcome column:

- **M-1. A state outside the model.** On a DUPSORT table, `MDBX_SET_RANGE`, `MDBX_SET_LOWERBOUND` and
  `MDBX_SET_UPPERBOUND` past the last key give neither Xa nor Xb when the call starts from N, P, or an anchor
  on another leaf page (SR-3, SR-5, LB-4, LB-5 other leaf, UB-4, UB-5 other leaf, and the long key rows).
  Raw probes: `eof` TRUE, `on_first` FALSE, `on_last` TRUE, `on_first_dup` TRUE, `on_last_dup` TRUE,
  `GET_CURRENT` `MDBX_ENODATA`, `NEXT` `MDBX_NOTFOUND`, `PREV` the last record.
  Xa answers `MDBX_NOTFOUND` to `GET_CURRENT`, Xb returns the last record; this state answers `MDBX_ENODATA`.
  The same state appears after deleting the last key of the small DUPSORT table (D last), where 2.4 point 4
  said Xb. The plain table follows the model in all these rows (inferences for SR-3, SR-4, SR-5, UB-3 confirmed).
  I name the state **Xv** and added it to 2.1. For the user: the cursor is past the end and `PREV` returns the
  last value of the last key, but `GET_CURRENT` answers `MDBX_ENODATA`, the code of an unpositioned cursor,
  where Xa answers `MDBX_NOTFOUND` and Xb the last record.
  How the code produces it. Xv is the Xb encoding on a last key with several values, with the inner (values)
  cursor reset before the cursor reached the end. The number of values alone is not enough: UB-3 on the same
  last key gives Xb, because there the inner cursor is still on the last value. From N or P the inner cursor is already reset; from an anchor the fast path resets it
  (`src/cursor-seek.c:42 @ c26d8d4b`) and then leaves the page for the tree search
  (`src/cursor-seek.c:95-97 @ c26d8d4b`, `src/cursor-seek.c:131-132 @ c26d8d4b`). The leaf has no greater key,
  so `cursor_sibling_right()` runs (`src/cursor-seek.c:150-154 @ c26d8d4b`). At the last page it puts the index
  on the last key, sets `z_eof_soft` + `z_eof_hard` and leaves the inner cursor as it is
  (`src/cursor-nav.c:274-281 @ c26d8d4b`). `MDBX_GET_CURRENT` then passes the outer checks, because the index
  is on a key (`src/cursor-seek.c:279-284 @ c26d8d4b`), finds a node with several values and a reset inner
  cursor, and returns `MDBX_ENODATA` (`src/cursor-seek.c:300-306 @ c26d8d4b`). With one value on the last key
  the same position reads the record (`src/cursor-seek.c:316-318 @ c26d8d4b`): that is Xb. The `_dup` probes
  are TRUE because the outer cursor is not filled (`src/api-cursor.c:427 @ c26d8d4b`,
  `src/api-cursor.c:443 @ c26d8d4b`). The delete path is `src/cursor-mut.c:1112-1117 @ c26d8d4b`:
  `cursor_sibling_right()`, then the inner cursor is reset. The fast path on the rightmost leaf gives Xa
  instead: index = number of keys (`src/cursor-seek.c:99-117 @ c26d8d4b`).
  In the measured DUPSORT tables the last key always has three values, so every tree-search path past the end
  gave Xv; Xb there on a one-value last key is inferred, not measured (except D last on the multi table).
  The probe set reads `GET_CURRENT` with a data pointer. With a NULL data pointer, `GET_CURRENT` returns
  `MDBX_SUCCESS` and the last key in Xb, Xv and LB-6, and `MDBX_NOTFOUND` only in Xa
  (`src/cursor-seek.c:282-283 @ c26d8d4b`, `src/cursor-seek.c:297-299 @ c26d8d4b`). Inferred, not measured.
- **M-2. Too long key.** `MDBX_SET`, `MDBX_SET_KEY`, `MDBX_SET_RANGE`, `MDBX_SET_LOWERBOUND`,
  `MDBX_SET_UPPERBOUND` with a key one byte over the limit return `MDBX_NOTFOUND`, not `MDBX_BAD_VALSIZE`
  (SET-6, SETK-6, SR-8, LB-11, UB-8). The key is searched like any absent key. The outcome column of these
  rows lists `MDBX_BAD_VALSIZE`; that outcome does not occur for reads. This is not in A1 to A10.
  The code returns `MDBX_BAD_VALSIZE` only for a key shorter than the minimum, longer than a fixed key size
  (`src/cogs.h:252-254 @ c26d8d4b`), or of a length other than 4 or 8 on an `MDBX_INTEGERKEY` table
  (`src/cogs.h:257-267 @ c26d8d4b`). So "never `MDBX_BAD_VALSIZE`" holds for tables with variable-length keys,
  as measured. Commit `78df707c` ("rework assertion checking at all") removed a debug-build branch that returned
  `MDBX_BAD_VALSIZE` with an assertion here, so today every build answers the same. I read this as deliberate;
  that is my inference, the commit message does not say it. A range search with a long key can be meaningful
  (a greater key may exist; not measured: the measured long key sorts above every key). I mark the return code
  keep; the state follows the miss rows.
- **M-3. P and H on a one-key page.** The probes separate P from H only by `on_first`/`on_last`.
  On a page with one key both are TRUE after an H, so SET-2 (key below the first key, from N) and
  SET-3 on the one-record table read as P. They are H by the code path; I could not separate them with the probe set.
  This is a limit of the probe set, not one state for the user (2.4 point 8). It holds only for a miss below the
  only key; a miss above it leaves the index past the key, `on_first` is FALSE and the program prints H.
- **M-4. A2 confirmed for DUPSORT.** After `MDBX_LAST` on a DUPSORT table whose last key has several values,
  `eof` is FALSE and the state is R·Vn. On the plain table `MDBX_LAST` gives E (`eof` TRUE).
  So `eof` after `MDBX_LAST` depends on the table kind.
- **M-5. Vt reads as D on the outer level.** After deleting the last value of a key that is not the last key,
  the cursor shows `eof` FALSE, `GET_CURRENT` the first value of the next key, `NEXT` the same value,
  `PREV` the last remaining value, `on_first_dup` FALSE, `on_last_dup` TRUE. The program prints D·Vn.
  On the last key it shows `GET_CURRENT` and `NEXT` `MDBX_NOTFOUND`, `eof` FALSE.
  Correction to my first reading: the 2.2 row Vt holds where measured. Its `MDBX_NOTFOUND` is in the
  `NEXT_DUP` column, which the probe set does not run, not in `NEXT`. "Vn" is the classifier's reading of the
  `_dup` probes; a real R·Vn returns its own value from `GET_CURRENT`. What was wrong is the 2.2 sentence
  "the main state is R or E": after a value delete the outer probes read as D. 2.2 is corrected.
  Rows that start from R·Vd, or from R·Vt on a key that is not the last key, keep their expectations but mean
  D·Vd and D·Vt: REL-14, REL-15, REL-29, REL-33, REL-34, REL-41, REL-66, REL-67, GM-4, PM-3, PUT-14, DEL-10,
  DEL-11, OUT-13. Vt on the last key does not read as D (`GET_CURRENT` and `NEXT` return `MDBX_NOTFOUND`), so
  REL-16 and REL-68 stay as written. The SET rows are not affected.
- **M-6. A10 confirmed.** An H cursor reads its stale index: after `MDBX_SET` on a key between two keys,
  `on_first` and `on_last` are both FALSE. After a miss below the first key `on_first` is TRUE, above the
  last key `on_last` is TRUE. `mdbx.h` promises TRUE for a cursor that is not positioned.
- **M-7. A9 confirmed.** `MDBX_SET_LOWERBOUND` on a plain table with a NULL data pointer returns
  `MDBX_EINVAL` (LB-9). `MDBX_SET_UPPERBOUND` does the same (UB-7).
- **M-8. LB-10.** On a DUPSORT table a data value with a NULL pointer returns `MDBX_RESULT_TRUE` and the first
  value of the key, even when the key exists. The row's outcome says "success or `MDBX_RESULT_TRUE`"; it is always
  `MDBX_RESULT_TRUE`.
- **M-9. LB-3 and LB-6 differ.** A value above all values of a key that has a next key returns the first value of the
  next key (LB-3). The same on the last key returns `MDBX_NOTFOUND` and gives Xa from every start, N included,
  while a key past the last key from N gives Xb (LB-4). Same state names as SR-3/SR-4, reached differently.
- **M-11. Row labels in the program (found in review).** `row_id()` labels some DUPSORT pair calls by their
  intent, not by their result. On the one-key DUPSORT table and from the VtLastKey start, lines labelled LB-2,
  LB-3, UB-1 and UB-2 return `MDBX_NOTFOUND`: the pair is past the end there. Their states (Xa, Xb, Xv) are those
  of LB-4, LB-6, UB-3 and UB-4, so no mark changes. The measured cells of LB-2, LB-3, UB-1 and UB-2 describe the
  correctly labelled lines. `key_hit_value_below_all` is labelled LB-1 and UB-2; its LB result is in LB-2 by
  hand. LB with NULL data on a DUPSORT table is labelled LB-9: `MDBX_EINVAL`, cursor not changed, as on a plain
  table. The labels get fixed when the program becomes a regression test.
- **M-10. Not contradicted.** SET-2 and SET-3 (H), SET-4, SR-6, LB-7, UB-6 (cursor unchanged), the success rows
  (R), and the argument errors (`MDBX_EINVAL`, cursor unchanged) match the inferences.

Not measured: C-1 for calls outside the SET family; the C-3 and C-4 rows; a read-only transaction (all scenarios
run in a write transaction, aborted afterwards); `MDBX_DUPFIXED`; trees deeper than two levels; DUPSORT value sets
that are nested trees (not checked); the D0 start on a table with other data (D0 exists only on the empty table).
`MDBX_KEYEXIST`, `MDBX_EACCESS` and `MDBX_MAP_FULL` cannot occur for read operations (rule G-read).

#### 4.3.2 `MDBX_GET_BOTH` and `MDBX_GET_BOTH_RANGE`

Grouping: G-seek, G-arg.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| GB-1 | `MDBX_GET_BOTH` (DUPSORT) | any bound | success | "MDBX_DUPSORT-only: Position at key/data pair." `mdbx.h:1818 @ c26d8d4b` | not measured | | |
| GB-2 | `MDBX_GET_BOTH` (DUPSORT) | any bound, key absent | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GB-3 | `MDBX_GET_BOTH` (DUPSORT) | any bound, key present, value absent | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GB-4 | `MDBX_GET_BOTH` (DUPSORT) | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GB-5 | `MDBX_GET_BOTH` | any bound | `MDBX_EINVAL` (key or data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| GB-6 | `MDBX_GET_BOTH` (plain) | any bound | `MDBX_INCOMPATIBLE` | "MDBX_DUPSORT-only" `mdbx.h:1818 @ c26d8d4b` | not measured | | |
| GB-7 | `MDBX_GET_BOTH` (DUPSORT) | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | not measured | | |
| GBR-1 | `MDBX_GET_BOTH_RANGE` (DUPSORT) | any bound | success (first value greater than or equal) | "Position at given key and at first data greater than or equal to specified data." `mdbx.h:1821-1822 @ c26d8d4b` | not measured | | |
| GBR-2 | `MDBX_GET_BOTH_RANGE` (DUPSORT) | any bound, key absent | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GBR-3 | `MDBX_GET_BOTH_RANGE` (DUPSORT) | any bound; key with several values, all smaller than data | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GBR-4 | `MDBX_GET_BOTH_RANGE` (DUPSORT) | no anchor; key with a single value, smaller than data | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GBR-5 | `MDBX_GET_BOTH_RANGE` (DUPSORT) | anchored; key with a single value, smaller than data | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GBR-6 | `MDBX_GET_BOTH_RANGE` (DUPSORT) | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| GBR-7 | `MDBX_GET_BOTH_RANGE` | any bound | `MDBX_EINVAL` (key or data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| GBR-8 | `MDBX_GET_BOTH_RANGE` (plain) | any bound | `MDBX_INCOMPATIBLE` | "MDBX_DUPSORT-only" `mdbx.h:1821 @ c26d8d4b` | not measured | | |

Notes:

- GB-2, GB-3, GBR-2, GBR-3, inferred: H (`src/cursor-seek.c:206-212 @ c26d8d4b`).
- GBR-4, GBR-5, inferred: for a single-value key the code returns `MDBX_NOTFOUND` without setting any state (`src/cursor-seek.c:246-248 @ c26d8d4b`). The cursor keeps the flags it had before, on the new index. So the result depends on the starting state: it may look like R on that key.

#### 4.3.3 `MDBX_TO_KEY_*`

All five share one sentence: "Doubtless cursor positioning at a specified key." (`mdbx.h:1905-1910 @ c26d8d4b`).
I write "TO_KEY" below for that quote. Grouping: G-seek, G-arg.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| TKLT-1 | `MDBX_TO_KEY_LESSER_THAN` | any bound | success (a smaller key exists) | TO_KEY `mdbx.h:1905-1906 @ c26d8d4b` | not measured | | |
| TKLT-2 | `MDBX_TO_KEY_LESSER_THAN` | any bound; key past the last key | success (the last key) | TO_KEY | not measured | | |
| TKLT-3 | `MDBX_TO_KEY_LESSER_THAN` | any bound; key less than or equal to the first key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKLT-4 | `MDBX_TO_KEY_LESSER_THAN` | N, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKLT-5 | `MDBX_TO_KEY_LESSER_THAN` | P, D0, empty table | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| TKLT-6 | `MDBX_TO_KEY_LESSER_THAN` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TKLE-1 | `MDBX_TO_KEY_LESSER_OR_EQUAL` | any bound | success (exact key) | TO_KEY | not measured | | |
| TKLE-2 | `MDBX_TO_KEY_LESSER_OR_EQUAL` | any bound | success (smaller key, including past the last key) | TO_KEY | not measured | | |
| TKLE-3 | `MDBX_TO_KEY_LESSER_OR_EQUAL` | any bound; key less than the first key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKLE-4 | `MDBX_TO_KEY_LESSER_OR_EQUAL` | N, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKLE-5 | `MDBX_TO_KEY_LESSER_OR_EQUAL` | P, D0, empty table | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| TKLE-6 | `MDBX_TO_KEY_LESSER_OR_EQUAL` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TKEQ-1 | `MDBX_TO_KEY_EQUAL` | any bound | success | TO_KEY | not measured | | |
| TKEQ-2 | `MDBX_TO_KEY_EQUAL` | any bound; key absent, a greater key exists | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKEQ-3 | `MDBX_TO_KEY_EQUAL` | no anchor; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKEQ-4 | `MDBX_TO_KEY_EQUAL` | anchored on the rightmost leaf page; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKEQ-5 | `MDBX_TO_KEY_EQUAL` | any bound, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKEQ-6 | `MDBX_TO_KEY_EQUAL` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TKGE-1 | `MDBX_TO_KEY_GREATER_OR_EQUAL` | any bound | success (exact or greater key) | TO_KEY | not measured | | |
| TKGE-2 | `MDBX_TO_KEY_GREATER_OR_EQUAL` | no anchor; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGE-3 | `MDBX_TO_KEY_GREATER_OR_EQUAL` | anchored on the rightmost leaf page; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGE-4 | `MDBX_TO_KEY_GREATER_OR_EQUAL` | any bound, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGE-5 | `MDBX_TO_KEY_GREATER_OR_EQUAL` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TKGT-1 | `MDBX_TO_KEY_GREATER_THAN` | any bound | success (greater key) | TO_KEY | not measured | | |
| TKGT-2 | `MDBX_TO_KEY_GREATER_THAN` | any bound; key equals the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGT-3 | `MDBX_TO_KEY_GREATER_THAN` | no anchor; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGT-4 | `MDBX_TO_KEY_GREATER_THAN` | anchored on the rightmost leaf page; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGT-5 | `MDBX_TO_KEY_GREATER_THAN` | any bound, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TKGT-6 | `MDBX_TO_KEY_GREATER_THAN` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TK-V | all five `MDBX_TO_KEY_*` | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | not measured | | |

Notes (all inferred):

- TKLT-3, TKLE-3: the cursor stays on the first record with `eof` FALSE (`src/cursor-seek.c:509-510 @ c26d8d4b`, `src/cursor-nav.c:253-264 @ c26d8d4b`).
- TKLT-4 against TKLT-5: on an empty table the result code depends on the starting state. From N the internal previous step acts as `MDBX_LAST` and returns `MDBX_NOTFOUND`; from P it returns `MDBX_ENODATA` (`src/cursor-seek.c:509-510 @ c26d8d4b`, `src/cursor-nav.c:161-170 @ c26d8d4b`).
- TKEQ-2: the cursor stays on the greater key, in R, although the call returns `MDBX_NOTFOUND` (`src/cursor-seek.c:511-512 @ c26d8d4b`). `MDBX_SET` gives H for the same miss.
- TKGT-2: Xb, like UB-3 (`src/cursor-seek.c:507-508 @ c26d8d4b`).

#### 4.3.4 `MDBX_TO_EXACT_KEY_VALUE_*`

All five share: "Doubtless cursor positioning at a specified key-value pair for dupsort/multi-value hives."
(`mdbx.h:1912-1918 @ c26d8d4b`). I write "TO_EXACT" for that quote. The key must match exactly first
(`src/cursor-seek.c:527 @ c26d8d4b`). Grouping: G-arg; the starting state matters only as in G-seek.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| TX-1 | all five | any bound, key absent | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TX-2 | all five | any bound, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TX-3 | all five | any bound | `MDBX_EINVAL` (key or data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TX-4 | all five | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | not measured | | |
| TX-5 | all five, plain table or single-value key | any bound | success (the comparison with the one value holds) | TO_EXACT | not measured | | |
| TX-6 | all five, plain table or single-value key | any bound | `MDBX_NOTFOUND` (the comparison fails) | mdbx.h is silent | not measured | | |
| TXLT-1 | `MDBX_TO_EXACT_KEY_VALUE_LESSER_THAN` (DUPSORT) | any bound; key with several values | success (a smaller value exists) | TO_EXACT | not measured | | |
| TXLT-2 | `MDBX_TO_EXACT_KEY_VALUE_LESSER_THAN` (DUPSORT) | any bound; value less than or equal to the first value | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TXLE-1 | `MDBX_TO_EXACT_KEY_VALUE_LESSER_OR_EQUAL` (DUPSORT) | any bound; key with several values | success | TO_EXACT | not measured | | |
| TXLE-2 | `MDBX_TO_EXACT_KEY_VALUE_LESSER_OR_EQUAL` (DUPSORT) | any bound; value less than the first value | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TXEQ-1 | `MDBX_TO_EXACT_KEY_VALUE_EQUAL` (DUPSORT) | any bound; key with several values | success | TO_EXACT | not measured | | |
| TXEQ-2 | `MDBX_TO_EXACT_KEY_VALUE_EQUAL` (DUPSORT) | any bound; value absent | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TXGE-1 | `MDBX_TO_EXACT_KEY_VALUE_GREATER_OR_EQUAL` (DUPSORT) | any bound; key with several values | success | TO_EXACT | not measured | | |
| TXGE-2 | `MDBX_TO_EXACT_KEY_VALUE_GREATER_OR_EQUAL` (DUPSORT) | any bound; value greater than the last value | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TXGT-1 | `MDBX_TO_EXACT_KEY_VALUE_GREATER_THAN` (DUPSORT) | any bound; key with several values | success | TO_EXACT | not measured | | |
| TXGT-2 | `MDBX_TO_EXACT_KEY_VALUE_GREATER_THAN` (DUPSORT) | any bound; value greater than or equal to the last value | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |

Notes (all inferred):

- TX-1: H (`src/cursor-seek.c:527-528 @ c26d8d4b`, through `MDBX_SET_KEY`).
- TX-6: the cursor stays on the key in R although the call returns `MDBX_NOTFOUND` (`src/cursor-seek.c:556-576 @ c26d8d4b`).
- TXLT-2, TXLE-2: the inner cursor stays on the first value (`src/cursor-seek.c:544-545 @ c26d8d4b`).
- TXEQ-2: Vx (`src/cursor-seek.c:548-555 @ c26d8d4b`).
- TXGT-2: the inner cursor is left past the last value with its index on the last value, the Xb shape for the inner cursor. `GET_CURRENT` may then return that last value (`src/cursor-seek.c:542-543 @ c26d8d4b`, `src/cursor-nav.c:266-283 @ c26d8d4b`).

#### 4.3.5 `MDBX_TO_PAIR_*`

All five share: "Doubtless cursor positioning at a specified key-value pair for dupsort/multi-value hives."
(`mdbx.h:1920-1926 @ c26d8d4b`). I write "TO_PAIR" for that quote. Unlike TO_EXACT, the key is searched as a
range, so the cursor can move to another key. Grouping: G-seek, G-arg.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| TP-1 | all five | any bound, empty table | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TP-2 | all five | any bound | `MDBX_EINVAL` (key or data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| TP-3 | all five | any bound | `MDBX_BAD_VALSIZE` | mdbx.h is silent | not measured | | |
| TPLT-1 | `MDBX_TO_PAIR_LESSER_THAN` | any bound | success (smaller pair in the same key) | TO_PAIR | not measured | | |
| TPLT-2 | `MDBX_TO_PAIR_LESSER_THAN` | any bound | success (pair in a previous key) | TO_PAIR | not measured | | |
| TPLT-3 | `MDBX_TO_PAIR_LESSER_THAN` | any bound; no smaller pair | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TPLE-1 | `MDBX_TO_PAIR_LESSER_OR_EQUAL` | any bound | success (exact or smaller pair in the same key) | TO_PAIR | not measured | | |
| TPLE-2 | `MDBX_TO_PAIR_LESSER_OR_EQUAL` | any bound | success (pair in a previous key) | TO_PAIR | not measured | | |
| TPLE-3 | `MDBX_TO_PAIR_LESSER_OR_EQUAL` | any bound; no smaller or equal pair | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TPEQ-1 | `MDBX_TO_PAIR_EQUAL` | any bound | success | TO_PAIR | not measured | | |
| TPEQ-2 | `MDBX_TO_PAIR_EQUAL` | any bound; key absent, a greater key exists | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TPEQ-3 | `MDBX_TO_PAIR_EQUAL` | any bound; key present, value absent | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TPEQ-4 | `MDBX_TO_PAIR_EQUAL` | any bound; key past the last key | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TPGE-1 | `MDBX_TO_PAIR_GREATER_OR_EQUAL` | any bound | success (exact or greater pair in the same key) | TO_PAIR | not measured | | |
| TPGE-2 | `MDBX_TO_PAIR_GREATER_OR_EQUAL` | any bound | success (pair in a next key) | TO_PAIR | not measured | | |
| TPGE-3 | `MDBX_TO_PAIR_GREATER_OR_EQUAL` | any bound; no greater or equal pair | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| TPGT-1 | `MDBX_TO_PAIR_GREATER_THAN` | any bound | success (greater pair in the same key) | TO_PAIR | not measured | | |
| TPGT-2 | `MDBX_TO_PAIR_GREATER_THAN` | any bound | success (pair in a next key) | TO_PAIR | not measured | | |
| TPGT-3 | `MDBX_TO_PAIR_GREATER_THAN` | any bound; no greater pair | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |

Notes (all inferred):

- TPEQ-2: the cursor stays on the greater key in R (`src/cursor-seek.c:654-655 @ c26d8d4b`).
- TPEQ-3: Vx (`src/cursor-seek.c:615-625 @ c26d8d4b`) on a multi-value key; on a single-value key the cursor stays on the key in R (`src/cursor-seek.c:639-641 @ c26d8d4b`).
- TPEQ-4: Xa or Xb as in SR-3 to SR-5.
- TPGT-3 when the last pair matches: Xb, as UB-3 (`src/cursor-seek.c:607-608 @ c26d8d4b`).

### 4.4 Multiple values (`MDBX_DUPFIXED`)

Grouping: G-arg. The outer state matters through "is the outer cursor on a record"
(`src/cursor-seek.c:360-361 @ c26d8d4b`) and the inner position through "is it on a value" (`src/cursor-seek.c:368-374 @ c26d8d4b`).

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| GM-1 | `MDBX_GET_MULTIPLE` (DUPFIXED) | R, E, D on a key with several values | success | "Return a batch of up to a page of duplicate data items of the current key ... Move cursor to prepare for MDBX_NEXT_MULTIPLE." `mdbx.h:1828-1831 @ c26d8d4b` | not measured | | |
| GM-2 | `MDBX_GET_MULTIPLE` (DUPFIXED) | R·Vs | success (the single value) | `mdbx.h:1828-1831 @ c26d8d4b` | not measured | | |
| GM-3 | `MDBX_GET_MULTIPLE` (DUPFIXED) | N, P, H, Xa, Xb, D0 | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| GM-4 | `MDBX_GET_MULTIPLE` (DUPFIXED) | R·Vt, R·Vx | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| GM-5 | `MDBX_GET_MULTIPLE` | any bound | `MDBX_EINVAL` (data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| GM-6 | `MDBX_GET_MULTIPLE` (plain, or DUPSORT without DUPFIXED) | any bound | `MDBX_INCOMPATIBLE` | "MDBX_DUPFIXED-only" `mdbx.h:1828 @ c26d8d4b` | not measured | | |
| NM-1 | `MDBX_NEXT_MULTIPLE` (DUPFIXED) | R on a key with several values, more pages of values follow | success | "Return up to a page of duplicate data items from next cursor position." `mdbx.h:1846-1848 @ c26d8d4b` | not measured | | |
| NM-2 | `MDBX_NEXT_MULTIPLE` (DUPFIXED) | R on the last page of values, or R·Vs | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| NM-3 | `MDBX_NEXT_MULTIPLE` (DUPFIXED) | N | success (acts as `MDBX_FIRST`, then the batch) | `mdbx.h:1846-1848 @ c26d8d4b`; `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| NM-4 | `MDBX_NEXT_MULTIPLE` (DUPFIXED) | P, H | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| NM-5 | `MDBX_NEXT_MULTIPLE` (DUPFIXED) | E, Xa, Xb, D, D0 | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| NM-6 | `MDBX_NEXT_MULTIPLE` | any bound | `MDBX_EINVAL` (data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| NM-7 | `MDBX_NEXT_MULTIPLE` (not DUPFIXED) | any bound | `MDBX_INCOMPATIBLE` | "MDBX_DUPFIXED-only" `mdbx.h:1846 @ c26d8d4b` | not measured | | |
| PM-1 | `MDBX_PREV_MULTIPLE` (DUPFIXED) | R, inner cursor not on the first page of values | success | "Position at previous page and return up to a page of duplicate data items." `mdbx.h:1872-1874 @ c26d8d4b` | not measured | | |
| PM-2 | `MDBX_PREV_MULTIPLE` (DUPFIXED) | R, inner cursor on the first page of values | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| PM-3 | `MDBX_PREV_MULTIPLE` (DUPFIXED) | N, P, H, Xa, Xb, D0, R·Vs, R·Vt, R·Vx | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| PM-4 | `MDBX_PREV_MULTIPLE` | any bound | `MDBX_EINVAL` (data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| PM-5 | `MDBX_PREV_MULTIPLE` (not DUPFIXED) | any bound | `MDBX_INCOMPATIBLE` | "MDBX_DUPFIXED-only" `mdbx.h:1872 @ c26d8d4b` | not measured | | |
| SG-1 | `MDBX_SEEK_AND_GET_MULTIPLE` (DUPFIXED) | any bound | success | "Seek to given key and return a batch of up to a page of the duplicate data items of this key." `mdbx.h:1928-1930 @ c26d8d4b` | not measured | | |
| SG-2 | `MDBX_SEEK_AND_GET_MULTIPLE` (DUPFIXED) | any bound, key absent | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| SG-3 | `MDBX_SEEK_AND_GET_MULTIPLE` | any bound, empty table | `MDBX_NOTFOUND` | `mdbx.h:6447 @ c26d8d4b` | not measured | | |
| SG-4 | `MDBX_SEEK_AND_GET_MULTIPLE` | any bound | `MDBX_EINVAL` (key is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| SG-5 | `MDBX_SEEK_AND_GET_MULTIPLE` | any bound, key present | `MDBX_EINVAL` (data is NULL) | `mdbx.h:6448 @ c26d8d4b` | not measured | | |
| SG-6 | `MDBX_SEEK_AND_GET_MULTIPLE` (not DUPFIXED) | any bound, key present | `MDBX_INCOMPATIBLE` | "MDBX_DUPFIXED-only" `mdbx.h:1928 @ c26d8d4b` | not measured | | |

Notes:

- NM-3, ask? (A1): `src/cursor-seek.c:383 @ c26d8d4b` calls the `_DUP` step, which acts as `MDBX_FIRST` from N (`src/cursor-nav.c:161-168 @ c26d8d4b`). Inferred, to reproduce before any claim: if the first key has a single value, the batch code then reads an inner cursor that is not on a page (`src/cursor-seek.c:387-392 @ c26d8d4b`; only a debug assertion guards it).
- SG-5, SG-6, inferred: the seek moves the cursor before the argument is checked (`src/cursor-seek.c:346-359 @ c26d8d4b`). So an `MDBX_EINVAL` or `MDBX_INCOMPATIBLE` leaves the cursor on the key, not in the starting state.

### 4.5 Cursor calls that set or change the state

#### 4.5.1 Life cycle

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| LC-1 | `mdbx_cursor_create()` | (no cursor) | success | "Create a cursor handle but not bind it to transaction nor DBI-handle." `mdbx.h:6087 @ c26d8d4b` | not measured | | |
| LC-2 | `mdbx_cursor_create()` | (no cursor) | NULL (out of memory) | "Created cursor handle or NULL in case out of memory." `mdbx.h:6108 @ c26d8d4b` | not measured | | |
| LC-3 | `mdbx_cursor_open()` | (no cursor) | success | "equivalent to calling mdbx_cursor_create() and then mdbx_cursor_bind()" `mdbx.h:6206-6207 @ c26d8d4b` | not measured | | |
| LC-4 | `mdbx_cursor_open()` | (no cursor) | `MDBX_EINVAL` (NULL result pointer, bad table handle) | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6232 @ c26d8d4b` | not measured | | |
| LC-5 | `mdbx_cursor_open()` | (no cursor) | `MDBX_EACCESS` (the GC table in a write or nested transaction) | mdbx.h is silent | not measured | | |
| LC-6 | `mdbx_cursor_bind()` | U | success | "A cursor may be associated with a new transaction ... whether the previous transaction is live or dead." `mdbx.h:6139-6141 @ c26d8d4b` | not measured | | |
| LC-7 | `mdbx_cursor_bind()` | any bound, same transaction and table | success | mdbx.h is silent | not measured | | |
| LC-8 | `mdbx_cursor_bind()` | any bound, other transaction or table | success | `mdbx.h:6139-6141 @ c26d8d4b` | not measured | | |
| LC-9 | `mdbx_cursor_bind()` | a parent's cursor while a child transaction uses it | `MDBX_EINVAL` | "the cursor should not be used in its parent transaction" `mdbx.h:6143-6145 @ c26d8d4b` | not measured | | |
| LC-10 | `mdbx_cursor_bind()` | any | `MDBX_EINVAL` (NULL transaction, bad table handle) | `mdbx.h:6161 @ c26d8d4b` | not measured | | |
| LC-11 | `mdbx_cursor_bind()` | any | `MDBX_EACCESS` (the GC table in a write or nested transaction) | mdbx.h is silent | not measured | | |
| LC-12 | `mdbx_cursor_bind()` | any | `MDBX_BAD_TXN` (transaction finished or has a child) | mdbx.h is silent | not measured | | |
| LC-13 | `mdbx_cursor_renew()` | U, bound before | success | "A cursor may be associated with a new transaction whether the previous transaction is running or finished." `mdbx.h:6338-6339 @ c26d8d4b` | not measured | | |
| LC-14 | `mdbx_cursor_renew()` | U, never bound | `MDBX_BAD_DBI` or `MDBX_EINVAL` | "MDBX_BAD_DBI The cursor was not bound to a DBI-handle" `mdbx.h:6357-6358 @ c26d8d4b` | not measured | | |
| LC-15 | `mdbx_cursor_renew()` | any bound, same transaction | success | mdbx.h is silent | not measured | | |
| LC-16 | `mdbx_cursor_renew()` | any bound, other transaction | success | `mdbx.h:6338-6339 @ c26d8d4b` | not measured | | |
| LC-17 | `mdbx_cursor_renew()` | a parent's cursor while a child transaction uses it | `MDBX_EINVAL` | `mdbx.h:6143-6145 @ c26d8d4b` (bind) | not measured | | |
| LC-18 | `mdbx_cursor_renew()` | any | `MDBX_BAD_TXN` (transaction finished or has a child) | mdbx.h is silent | not measured | | |
| LC-19 | `mdbx_cursor_unbind()` | any bound | success | "Unbinded cursor is disassociated with any transactions but still holds the original DBI-handle" `mdbx.h:6167-6169 @ c26d8d4b` | not measured | | |
| LC-20 | `mdbx_cursor_unbind()` | U | success (no-op) | mdbx.h is silent | not measured | | |
| LC-21 | `mdbx_cursor_unbind()` | a parent's cursor while a child transaction uses it | `MDBX_EINVAL` | `mdbx.h:6171-6173 @ c26d8d4b` | not measured | | |
| LC-22 | `mdbx_cursor_reset()` | any bound | success | "the cursor becomes unpositioned and does not allow relative positioning operations" `mdbx.h:6194-6196 @ c26d8d4b` | not measured | | ask? |
| LC-23 | `mdbx_cursor_reset()` | U | success (no effect) | mdbx.h is silent | not measured | | |
| LC-24 | `mdbx_cursor_close()`, `mdbx_cursor_close2()` | any bound, or U | success (handle freed) | "The cursor handle will be freed and must not be used again after this call" `mdbx.h:6238-6239 @ c26d8d4b` | not measured | | |
| LC-25 | `mdbx_cursor_close()`, `mdbx_cursor_close2()` | a parent's cursor inside an active child transaction | success (handle kept until the child ends) | `mdbx.h:6238-6239 @ c26d8d4b` | not measured | | |
| LC-26 | `mdbx_cursor_copy()` | source: any bound state; destination: U or any bound | success | "Copy cursor position and state." `mdbx.h:6375 @ c26d8d4b` | not measured | | |
| LC-27 | `mdbx_cursor_copy()` | source: U | `MDBX_EINVAL` | "A source cursor handle returned by mdbx_cursor_create() or mdbx_cursor_open()." `mdbx.h:6378-6379 @ c26d8d4b` | not measured | | ask? |
| LC-28 | `mdbx_cursor_copy()` | source and destination are the same cursor | `MDBX_EINVAL` | mdbx.h is silent | not measured | | |
| LC-29 | `mdbx_cursor_copy()` | destination: a parent's cursor while a child transaction uses it | `MDBX_EINVAL` | `mdbx.h:6143-6145 @ c26d8d4b` (bind) | not measured | | |
| LC-30 | `mdbx_txn_release_all_cursors_ex(unbind=true)` | all cursors of the transaction and its parents, any state | success | "Unbinds either closes all cursors associated ... with the given transaction" `mdbx.h:6283-6284 @ c26d8d4b`; "If non-zero, unbinds cursors and leaves ones reusable." `mdbx.h:6294 @ c26d8d4b` | not measured | | |
| LC-31 | `mdbx_txn_release_all_cursors_ex(unbind=false)` | same | success (all closed) | "Otherwise close and dispose cursors." `mdbx.h:6295 @ c26d8d4b` | not measured | | |
| LC-32 | `mdbx_txn_release_all_cursors_ex()` | transaction with an active child | `MDBX_BAD_TXN` | "Given transaction is invalid or has a child/nested transaction" `mdbx.h:6303-6304 @ c26d8d4b` | not measured | | |

Notes:

- LC-7, LC-15, inferred: binding or renewing a live cursor to its own transaction and table is a no-op. The position is kept; the cursor does not become N (`src/api-cursor.c:37-39 @ c26d8d4b`). In every other success the cursor becomes N (`src/cursor-setup.c:291 @ c26d8d4b`).
- LC-22, ask? (A1): see REL-1. Code: `src/api-cursor.c:79 @ c26d8d4b`, `src/cursor.h:543-547 @ c26d8d4b`.
- LC-23: `src/api-cursor.c:75-80 @ c26d8d4b` and `src/cursor-setup.c:533-536 @ c26d8d4b`.
- LC-25: `src/api-cursor.c:195-199 @ c26d8d4b`. The user cannot see the difference unless they touch the closed handle.
- LC-27, ask? (A8): a just-created cursor is unbound, and the source check rejects it (`src/api-cursor.c:226-228 @ c26d8d4b`, `src/cursor-setup.c:533-536 @ c26d8d4b`).
- LC-26: the destination is bound to the source's transaction and table first (`src/api-cursor.c:230-232 @ c26d8d4b`); its own starting state is irrelevant.
- `mdbx.h` lists the calls allowed after the transaction ends twice, differently: "renew and close" (`mdbx.h:4594-4599 @ c26d8d4b`) and "bind and renew", plus close (`mdbx.h:6090-6092 @ c26d8d4b`). The code also allows unbind (LC-20) and reset (LC-23). Not a code disagreement, so no mark.

#### 4.5.2 `mdbx_cursor_put()`

Grouping: the starting state matters for `MDBX_CURRENT` (it reads the current record first,
`src/cursor-mut.c:20-75 @ c26d8d4b`) and for `MDBX_APPEND`/`MDBX_APPENDDUP`. For the other flags the call
seeks the key itself (`src/cursor-mut.c:151 @ c26d8d4b`), so the starting state does not matter (G-seek, and no row
cares where a miss stops because the insert follows). Argument checks: G-arg.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| PUT-1 | `put`, flags 0 or `MDBX_UPSERT` | any bound | success (new or updated record) | "The cursor is positioned at the new item, or on failure usually near it." `mdbx.h:6686-6687 @ c26d8d4b` | not measured | | |
| PUT-2 | `put`, flags 0 | N, P, empty table | success (first record) | `mdbx.h:6686-6687 @ c26d8d4b` | not measured | | |
| PUT-3 | `put`, `MDBX_NOOVERWRITE` | any bound, key absent | success | `mdbx.h:6686-6687 @ c26d8d4b` | not measured | | |
| PUT-4 | `put`, `MDBX_NOOVERWRITE` | any bound, key present | `MDBX_KEYEXIST` | "The function will return MDBX_KEYEXIST if the key already appears in the table, even if the table supports duplicates" `mdbx.h:6711-6715 @ c26d8d4b` | not measured | | |
| PUT-5 | `put`, `MDBX_NODUPDATA` (DUPSORT) | any bound, pair absent | success | `mdbx.h:6705-6709 @ c26d8d4b` | not measured | | |
| PUT-6 | `put`, `MDBX_NODUPDATA` (DUPSORT) | any bound, pair present | `MDBX_KEYEXIST` | "will return MDBX_KEYEXIST if the key/data pair already appears in the table" `mdbx.h:6707-6709 @ c26d8d4b` | not measured | | |
| PUT-7 | `put`, `MDBX_CURRENT` | R, E | success | "Replace the item at the current cursor position." `mdbx.h:6695-6699 @ c26d8d4b` | not measured | | |
| PUT-8 | `put`, `MDBX_CURRENT` | Xb | success | `mdbx.h:6695-6699 @ c26d8d4b` | not measured | | |
| PUT-9 | `put`, `MDBX_CURRENT` | D | success | `mdbx.h:6695-6699 @ c26d8d4b`; `mdbx.h:6769-6771 @ c26d8d4b` | not measured | | |
| PUT-10 | `put`, `MDBX_CURRENT` (DUPSORT) | R·V*, new value size differs or the key has several values | success (old value deleted, new value inserted) | `mdbx.h:6701-6703 @ c26d8d4b` | not measured | | |
| PUT-11 | `put`, `MDBX_CURRENT` | R | `MDBX_EKEYMISMATCH` (other key) | "must match it, otherwise the function return MDBX_EKEYMISMATCH" `mdbx.h:6696-6698 @ c26d8d4b` | not measured | | |
| PUT-12 | `put`, `MDBX_CURRENT` | N, P, H, D0 | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| PUT-13 | `put`, `MDBX_CURRENT` | Xa | `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |
| PUT-14 | `put`, `MDBX_CURRENT` (DUPSORT) | R·Vt, R·Vx | success or error (to measure) | mdbx.h is silent | not measured | | |
| PUT-15 | `put`, `MDBX_CURRENT` + `MDBX_ALLDUPS` (DUPSORT) | R | success (all values replaced) | "With combination the MDBX_ALLDUPS will replace all multi-values." `mdbx.h:6698-6699 @ c26d8d4b` | not measured | | |
| PUT-16 | `put`, `MDBX_ALLDUPS` without `MDBX_CURRENT` (DUPSORT) | any bound | success | mdbx.h is silent | not measured | | |
| PUT-17 | `put`, `MDBX_RESERVE` (plain) | any bound | success | `mdbx.h:6717-6723 @ c26d8d4b` | not measured | | |
| PUT-18 | `put`, `MDBX_RESERVE` (DUPSORT) | any bound | `MDBX_INCOMPATIBLE` | "This flag must not be specified if the table was opened with MDBX_DUPSORT." `mdbx.h:6722-6723 @ c26d8d4b` | not measured | | |
| PUT-19 | `put`, `MDBX_APPEND` | any bound; key greater than the last key | success | "Append the given key/data pair to the end of the table." `mdbx.h:6725-6728 @ c26d8d4b` | not measured | | |
| PUT-20 | `put`, `MDBX_APPEND` | any bound; key smaller than the last key | `MDBX_EKEYMISMATCH` | "Loading unsorted keys with this flag will cause a MDBX_KEYEXIST error." `mdbx.h:6728-6729 @ c26d8d4b` | not measured | | ask? |
| PUT-21 | `put`, `MDBX_APPEND` | any bound; key equals the last key | success | mdbx.h is silent | not measured | | |
| PUT-22 | `put`, `MDBX_APPENDDUP` (DUPSORT) | any bound; value greater than the last value of the key | success | "As above, but for sorted dup data." `mdbx.h:6731-6732 @ c26d8d4b` | not measured | | |
| PUT-23 | `put`, `MDBX_APPENDDUP` (DUPSORT) | any bound; value not greater than the last value | `MDBX_EKEYMISMATCH` | "As above" `mdbx.h:6731-6732 @ c26d8d4b`, so `MDBX_KEYEXIST` per `mdbx.h:6728-6729 @ c26d8d4b` | not measured | | ask? |
| PUT-24 | `put`, `MDBX_MULTIPLE` (DUPFIXED) | any bound | success | "Store multiple contiguous data elements in a single request." `mdbx.h:6734-6748 @ c26d8d4b` | not measured | | |
| PUT-25 | `put`, `MDBX_MULTIPLE` (not DUPFIXED) | any bound | `MDBX_INCOMPATIBLE` | "This flag may only be specified if the table was opened with MDBX_DUPFIXED." `mdbx.h:6735-6737 @ c26d8d4b` | not measured | | |
| PUT-26 | `put`, `MDBX_CURRENT` with `MDBX_APPEND` or `MDBX_NOOVERWRITE`; `MDBX_MULTIPLE` with `MDBX_RESERVE`; unknown flag bits; NULL key or data | any bound | `MDBX_EINVAL` | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6762 @ c26d8d4b` | not measured | | |
| PUT-27 | `put`, any flags | any bound | `MDBX_BAD_VALSIZE` (key or value length outside the table limits) | mdbx.h is silent | not measured | | |
| PUT-28 | `put`, any flags | any bound, read-only transaction | `MDBX_EACCESS` | "MDBX_EACCES An attempt was made to write in a read-only transaction." `mdbx.h:6760-6761 @ c26d8d4b` | not measured | | |
| PUT-29 | `put`, any flags | any bound, small database size | `MDBX_MAP_FULL` | "MDBX_MAP_FULL The database is full" `mdbx.h:6757-6758 @ c26d8d4b` | not measured | | |

Notes:

- PUT-1, PUT-3, PUT-19: after success the cursor should be R on the new item (`src/cursor-mut.c:803 @ c26d8d4b`).
- PUT-4, inferred: the cursor is on the existing record (the internal seek, `src/cursor-mut.c:151-161 @ c26d8d4b`).
- PUT-8, inferred: from Xb, `MDBX_CURRENT` replaces the last record, because the internal `GET_CURRENT` reads it (`src/cursor-mut.c:28 @ c26d8d4b`, `src/cursor-seek.c:279-284 @ c26d8d4b`). From Xa it fails (PUT-13).
- PUT-9, inferred: from D, `MDBX_CURRENT` replaces the record after the deleted one (`src/cursor-seek.c:285-286 @ c26d8d4b`). This matches the delete note of `mdbx.h`.
- PUT-11, PUT-12, PUT-13, PUT-26, PUT-28, inferred: the cursor is not changed; the check runs before any move.
- PUT-20, PUT-23, ask? (A4): the code returns `MDBX_EKEYMISMATCH` (`src/cursor-mut.c:137-139 @ c26d8d4b`, `src/cursor-mut.c:181-182 @ c26d8d4b`, `src/cursor-mut.c:400-401 @ c26d8d4b`). The `mdbx_put()` text already says `MDBX_EKEYMISMATCH` (`mdbx.h:5904-5906 @ c26d8d4b`); only the `mdbx_cursor_put()` text says `MDBX_KEYEXIST`. Inferred: PUT-20 also moves the cursor to the last record, because the code reads it first (`src/cursor-mut.c:129 @ c26d8d4b`).
- PUT-21, inferred: `MDBX_APPEND` with the last key updates the last value (`src/cursor-mut.c:140-143 @ c26d8d4b`).
- PUT-29: after `MDBX_MAP_FULL` the code may mark the transaction as failed (`src/cursor-mut.c:839 @ c26d8d4b`). If so, the cursor state belongs to the failure-model contract. I measure only while the transaction stays usable.

#### 4.5.3 `mdbx_cursor_del()`

Grouping: the state matters (the call deletes "the current record"). `MDBX_NODUPDATA` is a synonym of `MDBX_ALLDUPS` here (`mdbx.h:6787-6790 @ c26d8d4b`); I do not repeat the rows for it.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| DEL-1 | `del`, flags 0 or `MDBX_CURRENT` | R, not on the last record | success | "This does not invalidate the cursor ... Both MDBX_NEXT and MDBX_GET_CURRENT will return the same record after this operation." `mdbx.h:6768-6771 @ c26d8d4b` | not measured | | |
| DEL-2 | `del`, flags 0 | R on the last record, other records remain | success | `mdbx.h:6768-6771 @ c26d8d4b` | not measured | | ask? |
| DEL-3 | `del`, flags 0 | R on the only record | success | `mdbx.h:6768-6771 @ c26d8d4b` | not measured | | ask? |
| DEL-4 | `del`, flags 0 | E | success (deletes the last record) | `mdbx.h:6768-6771 @ c26d8d4b` | not measured | | ask? |
| DEL-5 | `del`, flags 0 | D | success (deletes the record after the earlier deleted one) | mdbx.h is silent | not measured | | |
| DEL-6 | `del`, any flags | N, P, H, Xa, Xb, D0 | `MDBX_ENODATA` | mdbx.h is silent (`MDBX_ENODATA` is not listed, `mdbx.h:6794-6803 @ c26d8d4b`) | not measured | | |
| DEL-7 | `del`, flags 0 (DUPSORT) | R·V1, R·Vm | success (one value deleted) | "MDBX_CURRENT Delete only single entry at current cursor position." `mdbx.h:6786 @ c26d8d4b` | not measured | | |
| DEL-8 | `del`, flags 0 (DUPSORT) | R·Vn | success | `mdbx.h:6786 @ c26d8d4b` | not measured | | |
| DEL-9 | `del`, flags 0 (DUPSORT) | R·Vs | success (the key is removed) | `mdbx.h:6786 @ c26d8d4b` | not measured | | |
| DEL-10 | `del`, flags 0 (DUPSORT) | R·Vd | success | mdbx.h is silent | not measured | | |
| DEL-11 | `del`, flags 0 (DUPSORT) | R·Vt | success (no-op) | mdbx.h is silent | not measured | | |
| DEL-12 | `del`, flags 0 (DUPSORT) | R·Vx | `MDBX_ENODATA` | mdbx.h is silent | not measured | | |
| DEL-13 | `del`, `MDBX_ALLDUPS` (DUPSORT) | R (any V*), not on the last key | success | "Delete all of the data items for the current key." `mdbx.h:6787-6790 @ c26d8d4b` | not measured | | |
| DEL-14 | `del`, `MDBX_ALLDUPS` (DUPSORT) | R (any V*) on the last key, other keys remain | success | `mdbx.h:6787-6790 @ c26d8d4b`; `mdbx.h:6768-6771 @ c26d8d4b` | not measured | | ask? |
| DEL-15 | `del`, `MDBX_ALLDUPS` (plain) | R | success (same as flags 0) | "This flag has effect only for table(s) was created with MDBX_DUPSORT." `mdbx.h:6789-6790 @ c26d8d4b` | not measured | | |
| DEL-16 | `del`, unknown flag bits | any bound | `MDBX_EINVAL` | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6803 @ c26d8d4b` | not measured | | |
| DEL-17 | `del`, any flags | any bound, read-only transaction | `MDBX_EACCESS` | "MDBX_EACCES An attempt was made to write in a read-only transaction." `mdbx.h:6801-6802 @ c26d8d4b` | not measured | | |
| DEL-18 | `del`, any flags | R, small database size | `MDBX_MAP_FULL` | "MDBX_MAP_FULL The database is full" `mdbx.h:6798-6799 @ c26d8d4b` | not measured | | |

Notes:

- DEL-1: D (`src/cursor-mut.c:1089 @ c26d8d4b`).
- DEL-2, DEL-4, DEL-14, ask? (A3), inferred: the cursor ends in Xb, not D (`src/cursor-mut.c:1112-1117 @ c26d8d4b`, `src/cursor-nav.c:280 @ c26d8d4b`). Then `MDBX_NEXT` returns `MDBX_NOTFOUND`, but `MDBX_GET_CURRENT` returns the new last record (`src/cursor-seek.c:279-297 @ c26d8d4b`). `mdbx.h` promises the same record from both.
- DEL-3, ask? (A3): D0. `MDBX_NEXT` returns `MDBX_NOTFOUND`, `MDBX_GET_CURRENT` returns `MDBX_ENODATA` (REL-10, REL-61).
- DEL-5, inferred: D counts as "on a record" for delete (`src/cursor-mut.c:925 @ c26d8d4b`), so a second delete without a move removes the next record.
- DEL-6: `src/cursor-mut.c:925-929 @ c26d8d4b`.
- DEL-7, DEL-8, DEL-11, DEL-12: Vd, Vt, no-op, `MDBX_ENODATA` (`src/cursor-mut.c:958-965 @ c26d8d4b`, `src/cursor-mut.c:1011-1022 @ c26d8d4b`).
- DEL-18: see PUT-29.

#### 4.5.4 `mdbx_cursor_delete_range()`

Grouping: the states of `begin` and `end` matter only through "has a stack" (`src/api-cursor.c:936-945 @ c26d8d4b`, `src/tree-cutoff.c:518-519 @ c26d8d4b`).

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| RNG-1 | `delete_range` | begin R, end R, begin before end | success | "Performs mass deletion of elements between positions of given cursors pair" `mdbx.h:6809-6810 @ c26d8d4b` | not measured | | |
| RNG-2 | `delete_range` | begin and end on the same record | success | mdbx.h is silent | not measured | | |
| RNG-3 | `delete_range` | begin NULL, end R | success | "or can be NULL to delete starting the first item." `mdbx.h:6812-6813 @ c26d8d4b` | not measured | | |
| RNG-4 | `delete_range` | begin R, end NULL | success | "or can be NULL to delete up to the last item." `mdbx.h:6815-6816 @ c26d8d4b` | not measured | | |
| RNG-5 | `delete_range` | begin after end | `MDBX_ENODATA` | "or position of begin cursor is after the end cursor." `mdbx.h:6828-6829 @ c26d8d4b` | not measured | | |
| RNG-6 | `delete_range` | begin or end in N, P or D0 | `MDBX_ENODATA` | "One or both of the given cursor(s) is not positioned to a data" `mdbx.h:6828 @ c26d8d4b` | not measured | | |
| RNG-7 | `delete_range` | begin or end in H or Xa | success or an error from a stale position | `mdbx.h:6828 @ c26d8d4b` | not measured | | ask? |
| RNG-8 | `delete_range` | begin or end in E, Xb, D | success | mdbx.h is silent | not measured | | |
| RNG-9 | `delete_range` | both NULL; different transaction or table | `MDBX_EINVAL` | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6832 @ c26d8d4b` | not measured | | |
| RNG-10 | `delete_range` | any bound, read-only transaction | `MDBX_EACCESS` | "MDBX_EACCES An attempt was made to write in a read-only transaction." `mdbx.h:6831 @ c26d8d4b` | not measured | | |
| RNG-11 | `delete_range` | begin R, end R, small database size | `MDBX_MAP_FULL` | mdbx.h is silent (`MDBX_TXN_FULL` is listed, `mdbx.h:6830 @ c26d8d4b`) | not measured | | |

Notes:

- The state of `begin` and `end` after success is not described anywhere. The measuring program records both, and the state of other cursors inside and around the range (OUT-19).
- RNG-7, ask? (A6): the code checks only "has a stack" (`src/api-cursor.c:936-937 @ c26d8d4b`, `src/api-cursor.c:944-945 @ c26d8d4b`, `src/tree-cutoff.c:518-519 @ c26d8d4b`). H and Xa have a stack but no record under the cursor.

#### 4.5.5 `mdbx_cursor_bunch_delete()`

The default build has `MDBX_ENABLE_BUNCHES_REMOVAL=1` (`src/options.h:125 @ c26d8d4b`). The other build takes different code paths
(`src/api-cursor.c:787-911 @ c26d8d4b`); I measure the default build only. All actions share one `mdbx.h` sentence:
"Performs massive deletion much faster by cutting whole pages and branches" (`mdbx.h:6965-6966 @ c26d8d4b`),
and one error: "MDBX_ENODATA The given cursor is not positioned to a data." (`mdbx.h:6980 @ c26d8d4b`).

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| BUN-1 | `MDBX_DELETE_CURRENT_VALUE` | R, E, D | success (as `del` flags 0) | `mdbx.h:6965-6966 @ c26d8d4b` | not measured | | |
| BUN-2 | `MDBX_DELETE_CURRENT_VALUE` | N, P, H, Xa, Xb, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-3 | `MDBX_DELETE_CURRENT_MULTIVAL_ALL` | R, E, D | success (as `del` `MDBX_ALLDUPS`) | `mdbx.h:6965-6966 @ c26d8d4b` | not measured | | |
| BUN-4 | `MDBX_DELETE_CURRENT_MULTIVAL_ALL` | N, P, H, Xa, Xb, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-5 | `MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_EXCLUDING`, `_BEFORE_INCLUDING` (DUPSORT) | R (any V*) | success | `mdbx.h:6965-6966 @ c26d8d4b` | not measured | | |
| BUN-6 | `MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_*` | N, P, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-7 | `MDBX_DELETE_CURRENT_MULTIVAL_BEFORE_*` | H, Xa | success or an error from a stale position | `mdbx.h:6980 @ c26d8d4b` | not measured | | ask? |
| BUN-8 | `MDBX_DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING`, `_AFTER_EXCLUDING` (DUPSORT) | R (any V*) | success | `mdbx.h:6965-6966 @ c26d8d4b` | not measured | | |
| BUN-9 | `MDBX_DELETE_CURRENT_MULTIVAL_AFTER_INCLUDING` | N, P, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-10 | `MDBX_DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING` | P, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-11 | `MDBX_DELETE_CURRENT_MULTIVAL_AFTER_EXCLUDING` | N | success (acts as if on the first record) | `mdbx.h:6980 @ c26d8d4b` | not measured | | ask? |
| BUN-12 | `MDBX_DELETE_CURRENT_MULTIVAL_AFTER_*` | H, Xa | success or an error from a stale position | `mdbx.h:6980 @ c26d8d4b` | not measured | | ask? |
| BUN-13 | `MDBX_DELETE_CURRENT_MULTIVAL_*` (plain) | R | success (INCLUDING deletes the record, EXCLUDING deletes nothing) | mdbx.h is silent | not measured | | |
| BUN-14 | `MDBX_DELETE_BEFORE_EXCLUDING`, `MDBX_DELETE_BEFORE_INCLUDING` | R, E, Xb, D | success | `mdbx.h:6965-6966 @ c26d8d4b` | not measured | | |
| BUN-15 | `MDBX_DELETE_BEFORE_*` | N, P, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-16 | `MDBX_DELETE_BEFORE_*` | H, Xa | success or an error from a stale position | `mdbx.h:6980 @ c26d8d4b` | not measured | | ask? |
| BUN-17 | `MDBX_DELETE_AFTER_INCLUDING`, `MDBX_DELETE_AFTER_EXCLUDING` | R, E, Xb, D | success | `mdbx.h:6965-6966 @ c26d8d4b` | not measured | | |
| BUN-18 | `MDBX_DELETE_AFTER_INCLUDING` | N, P, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-19 | `MDBX_DELETE_AFTER_EXCLUDING` | P, D0 | `MDBX_ENODATA` | `mdbx.h:6980 @ c26d8d4b` | not measured | | |
| BUN-20 | `MDBX_DELETE_AFTER_EXCLUDING` | N | success (acts as if on the first record) | `mdbx.h:6980 @ c26d8d4b` | not measured | | ask? |
| BUN-21 | `MDBX_DELETE_AFTER_*` | H, Xa | success or an error from a stale position | `mdbx.h:6980 @ c26d8d4b` | not measured | | ask? |
| BUN-22 | `MDBX_DELETE_WHOLE` | any bound | success | mdbx.h is silent on the cursors | not measured | | |
| BUN-23 | any action | any bound | `MDBX_EINVAL` (action out of range) | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6983 @ c26d8d4b` | not measured | | |
| BUN-24 | any action | any bound, read-only transaction | `MDBX_EACCESS` | "MDBX_EACCES An attempt was made to write in a read-only transaction." `mdbx.h:6982 @ c26d8d4b` | not measured | | |
| BUN-25 | any deleting action | R, small database size | `MDBX_MAP_FULL` | mdbx.h is silent (`MDBX_TXN_FULL` is listed, `mdbx.h:6981 @ c26d8d4b`) | not measured | | |

Notes:

- BUN-7, BUN-12, BUN-16, BUN-21, ask? (A6): the range actions check only "has a stack" (`src/tree-cutoff.c:518-519 @ c26d8d4b`).
- BUN-11, BUN-20, ask? (A7), inferred: these actions first do an internal next step (`src/api-cursor.c:816-822 @ c26d8d4b`, `src/api-cursor.c:879-885 @ c26d8d4b`). From N it acts as `MDBX_FIRST` (`src/cursor-nav.c:161-168 @ c26d8d4b`). So the call deletes data instead of returning `MDBX_ENODATA`. This needs a reproduction before any claim.
- BUN-22: every cursor of the table becomes P (`src/table.c:184-187 @ c26d8d4b`); N stays N, because the reset only adds bits (`src/cursor.h:292-303 @ c26d8d4b`). Inferred.

#### 4.5.6 `mdbx_cursor_get_batch()` (plain tables)

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| BAT-1 | `get_batch`, `MDBX_FIRST` | any bound | success (more pairs remain) | "MDBX_FIRST always starts from the first pair, whatever the cursor position" `mdbx.h:6661-6662 @ c26d8d4b`; "MDBX_SUCCESS A chunk of key-value pairs has been returned, and more pairs may be available." `mdbx.h:6669-6670 @ c26d8d4b` | not measured | | |
| BAT-2 | `get_batch`, `MDBX_FIRST` | any bound | `MDBX_RESULT_TRUE` (last chunk) | "The returned chunk is the last one, and there are no pairs left." `mdbx.h:6671-6672 @ c26d8d4b` | not measured | | |
| BAT-3 | `get_batch`, `MDBX_FIRST` | any bound, empty table | `MDBX_NOTFOUND` | "No any key-value pairs are available: the table is empty (MDBX_FIRST)" `mdbx.h:6673-6675 @ c26d8d4b` | not measured | | |
| BAT-4 | `get_batch`, `MDBX_NEXT` | R, D | success | "MDBX_NEXT continues from the current position." `mdbx.h:6662-6663 @ c26d8d4b` | not measured | | |
| BAT-5 | `get_batch`, `MDBX_NEXT` | R, D | `MDBX_RESULT_TRUE` | `mdbx.h:6671-6672 @ c26d8d4b` | not measured | | |
| BAT-6 | `get_batch`, `MDBX_NEXT` | E, Xa, Xb | `MDBX_NOTFOUND` | "there are no pairs after the cursor position (MDBX_NEXT)" `mdbx.h:6674-6675 @ c26d8d4b` | not measured | | |
| BAT-7 | `get_batch`, `MDBX_NEXT` | N, P | `MDBX_ENODATA` | "The cursor has no position to continue from, i.e. it has never been positioned or was reset (MDBX_NEXT)." `mdbx.h:6676-6678 @ c26d8d4b` | not measured | | |
| BAT-8 | `get_batch`, `MDBX_NEXT` | H | `MDBX_NOTFOUND` | mdbx.h is silent on H | not measured | | |
| BAT-9 | `get_batch`, `MDBX_NEXT` | D0 | `MDBX_ENODATA` | `mdbx.h:6676-6678 @ c26d8d4b` | not measured | | |
| BAT-10 | `get_batch` | any bound | `MDBX_EINVAL` (count NULL, limit below 2, other operation) | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6679 @ c26d8d4b` | not measured | | |
| BAT-11 | `get_batch` (DUPSORT) | any bound | `MDBX_INCOMPATIBLE` | "For MDBX_DUPSORT tables please use MDBX_GET_MULTIPLE and MDBX_NEXT_MULTIPLE." `mdbx.h:6629-6630 @ c26d8d4b` | not measured | | |

Notes (all inferred):

- BAT-4: the batch starts with the current record (`src/api-cursor.c:615-626 @ c26d8d4b`), while `MDBX_NEXT` of `mdbx_cursor_get()` skips it. `mdbx.h` "continues from the current position" allows both readings.
- BAT-2, BAT-5: at the end the cursor is in Xb (`src/api-cursor.c:633-638 @ c26d8d4b`, `src/cursor-nav.c:266-283 @ c26d8d4b`).
- BAT-7 against REL-1: the same N gives `MDBX_ENODATA` here and acts as `MDBX_FIRST` in `mdbx_cursor_get(MDBX_NEXT)`.
- BAT-8: H passes as "positioned" here (`src/api-cursor.c:599-600 @ c26d8d4b`), while `mdbx_cursor_get(MDBX_NEXT)` gives `MDBX_ENODATA` for H (REL-3).

#### 4.5.7 `mdbx_cursor_scan()` and `mdbx_cursor_scan_from()`

Grouping: the starting state matters only through the start operation. `MDBX_FIRST`/`MDBX_LAST` follow G-abs; the others follow their own rows in 4.1, 4.2 and 4.4.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| SCN-1 | `scan`, start `MDBX_FIRST` or `MDBX_LAST` | any bound | `MDBX_RESULT_TRUE` (the predicate matched) | "MDBX_RESULT_TRUE if a key-value pair is found for which the predicative function returned MDBX_RESULT_TRUE." `mdbx.h:6542-6543 @ c26d8d4b` | not measured | | |
| SCN-2 | `scan`, turn `MDBX_NEXT` or `MDBX_NEXT_NODUP` | any bound | `MDBX_RESULT_FALSE` (end reached) | "MDBX_RESULT_FALSE if a suitable key-value pair is NOT found, the end of the data has been reached during the search, or there is no data to search for." `mdbx.h:6544-6545 @ c26d8d4b` | not measured | | |
| SCN-3 | `scan`, turn `MDBX_PREV` or `MDBX_PREV_NODUP` | any bound | `MDBX_RESULT_FALSE` (beginning reached) | `mdbx.h:6544-6545 @ c26d8d4b` | not measured | | |
| SCN-4 | `scan`, turn `MDBX_NEXT_DUP` or `MDBX_PREV_DUP` (DUPSORT) | any bound | `MDBX_RESULT_FALSE` (no more values of the key) | `mdbx.h:6544-6545 @ c26d8d4b` | not measured | | |
| SCN-5 | `scan`, turn `MDBX_NEXT_MULTIPLE` or `MDBX_PREV_MULTIPLE` (DUPFIXED) | any bound | `MDBX_RESULT_FALSE` | `mdbx.h:6544-6545 @ c26d8d4b` | not measured | | |
| SCN-6 | `scan`, start `MDBX_FIRST` or `MDBX_LAST` | any bound, empty table | `MDBX_RESULT_FALSE` | `mdbx.h:6544-6545 @ c26d8d4b` | not measured | | |
| SCN-7 | `scan`, start `MDBX_GET_CURRENT`, `MDBX_FIRST_DUP`, `MDBX_LAST_DUP` or `MDBX_GET_MULTIPLE` | R, E, Xb, D | as SCN-1 to SCN-5 | "To scan without changing the initial cursor position, use MDBX_GET_CURRENT." `mdbx.h:6521-6522 @ c26d8d4b` | not measured | | |
| SCN-8 | `scan`, start `MDBX_GET_CURRENT`, `MDBX_FIRST_DUP`, `MDBX_LAST_DUP` or `MDBX_GET_MULTIPLE` | N, P, H, D0 | `MDBX_ENODATA` | "an error code during positioning the cursor" `mdbx.h:6546-6547 @ c26d8d4b` | not measured | | |
| SCN-9 | `scan`, start `MDBX_GET_CURRENT` | Xa | `MDBX_RESULT_FALSE` | `mdbx.h:6544-6545 @ c26d8d4b` | not measured | | |
| SCN-10 | `scan` | any bound | the predicate's own error code | "a user-defined code for stopping the search or an user-defined error" `mdbx.h:6546-6547 @ c26d8d4b` | not measured | | |
| SCN-11 | `scan` | any bound | `MDBX_EINVAL` (predicate NULL, start or turn operation not allowed) | `mdbx.h:6523-6529 @ c26d8d4b` (allowed values) | not measured | | |
| SCF-1 | `scan_from`, any allowed from-operation | any bound | `MDBX_RESULT_TRUE` | `mdbx.h:6615-6616 @ c26d8d4b` | not measured | | |
| SCF-2 | `scan_from` | any bound | `MDBX_RESULT_FALSE` (end of data reached while scanning) | `mdbx.h:6617-6618 @ c26d8d4b` | not measured | | |
| SCF-3 | `scan_from` | any bound | `MDBX_RESULT_FALSE` (the from-operation returned `MDBX_NOTFOUND`) | `mdbx.h:6617-6618 @ c26d8d4b` | not measured | | |
| SCF-4 | `scan_from` | any bound | an error from the from-operation (`MDBX_EINVAL`, `MDBX_INCOMPATIBLE`, `MDBX_ENODATA`) | `mdbx.h:6619-6620 @ c26d8d4b` | not measured | | |
| SCF-5 | `scan_from` | any bound | the predicate's own error code | `mdbx.h:6619-6620 @ c26d8d4b` | not measured | | |
| SCF-6 | `scan_from` | any bound | `MDBX_EINVAL` (predicate or key NULL, from or turn operation not allowed) | `mdbx.h:6574-6593 @ c26d8d4b` (allowed values) | not measured | | |

Notes (all inferred):

- SCN-2: the scan loop moves with an internal step that does not clear `z_eof_hard` (`src/api-cursor.c:472-474 @ c26d8d4b`). So the cursor ends in Xb, while a user loop of `MDBX_NEXT` ends in E (REL-5).
- SCF-3: the state is the state after that from-operation's `MDBX_NOTFOUND` (4.3).

#### 4.5.8 `mdbx_cursor_scroll()`

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| SCR-1 | `scroll`, amount > 0 or < 0 | R, D | success | "The number of logical steps by which the cursor will be moved" `mdbx.h:6881-6884 @ c26d8d4b` | not measured | | |
| SCR-2 | `scroll`, amount > 0 | R, D | `MDBX_NOTFOUND` (end reached first) | "The end of the data was reached before the cursor moved by the requested number of steps." `mdbx.h:6895-6896 @ c26d8d4b` | not measured | | |
| SCR-3 | `scroll`, amount < 0 | R, D | `MDBX_NOTFOUND` (beginning reached first) | `mdbx.h:6895-6896 @ c26d8d4b` | not measured | | |
| SCR-4 | `scroll`, amount 0 | R | success | mdbx.h is silent | not measured | | |
| SCR-5 | `scroll` | N, P, D0 | `MDBX_ENODATA` | "MDBX_ENODATA Given cursor is not positioned to a data." `mdbx.h:6894 @ c26d8d4b` | not measured | | |
| SCR-6 | `scroll` | H, Xa | success or `MDBX_NOTFOUND` (from a stale position) | `mdbx.h:6894 @ c26d8d4b` | not measured | | ask? |
| SCR-7 | `scroll` | E, Xb | success or `MDBX_NOTFOUND` | mdbx.h is silent | not measured | | |

Notes:

- SCR-6, ask? (A6): `src/api-cursor.c:1071-1072 @ c26d8d4b` checks only "has a stack".
- SCR-2, inferred: Xb (`src/cursor-nav.c:353-357 @ c26d8d4b`). SCR-3, inferred: the cursor stays on the first record with `eof` FALSE (`src/cursor-nav.c:391-396 @ c26d8d4b`).

#### 4.5.9 `mdbx_cursor_distribute()`

The array cursors' starting state is irrelevant: the call overwrites their position (`src/cursor-seek.c:831-857 @ c26d8d4b`). They must be bound.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| DIS-1 | `distribute` | first and last R (or one of them NULL) | success | "Places a given set of cursors as evenly as possible" `mdbx.h:6904-6906 @ c26d8d4b` | not measured | | |
| DIS-2 | `distribute` | first and last R | `MDBX_RESULT_TRUE` (not enough positions) | "some cursors remained unset and mdbx_cursor_eof() will return MDBX_RESULT_TRUE for ones." `mdbx.h:6933-6935 @ c26d8d4b` | not measured | | |
| DIS-3 | `distribute` | first or last in N, P or D0 | `MDBX_ENODATA` | "The first or last cursor(s) is not positioned to a data." `mdbx.h:6932 @ c26d8d4b` | not measured | | |
| DIS-4 | `distribute` | first or last in H or Xa | success or `MDBX_RESULT_TRUE` (from a stale position) | `mdbx.h:6932 @ c26d8d4b` | not measured | | ask? |
| DIS-5 | `distribute` | first or last in E, Xb, D | success or `MDBX_RESULT_TRUE` | mdbx.h is silent | not measured | | |
| DIS-6 | `distribute` | any | `MDBX_EINVAL` (both NULL, array NULL, count below 1, no array cursor besides first and last, mixed tables, an array cursor in U) | "MDBX_EINVAL An invalid parameter was specified." `mdbx.h:6937 @ c26d8d4b` | not measured | | |

Notes:

- DIS-2: the left-over cursors become P (`src/cursor-seek.c:844-846 @ c26d8d4b`). This matches `mdbx.h`.
- DIS-4, ask? (A6): `src/api-cursor.c:1001-1002 @ c26d8d4b`, `src/api-cursor.c:1009-1010 @ c26d8d4b`.
- DIS-1, inferred: `last` is cloned when it is also in the array, so the caller's `last` keeps its state (`src/api-cursor.c:1057-1061 @ c26d8d4b`).

### 4.6 Effects from outside the cursor

B is the observed cursor. A is another cursor of the same transaction, or a call without a cursor
(`mdbx_put()`, `mdbx_del()`, `mdbx_replace()`). Each row is measured with A as a cursor and with A as a call
without a cursor, where both exist. `mdbx.h` says nothing about B's state in any of these rows; it describes only
that pointers returned earlier become stale (`mdbx.h:6427-6436 @ c26d8d4b`). I write "silent (pointers only)" for that.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| OUT-1 | A inserts a key before B's record | B: R | success | silent (pointers only) `mdbx.h:6427-6436 @ c26d8d4b` | not measured | | |
| OUT-2 | A inserts a key after B's record | B: R | success | silent (pointers only) | not measured | | |
| OUT-3 | A inserts a key after the last key | B: E, Xa, Xb | success | silent (pointers only) | not measured | | |
| OUT-4 | A inserts a key | B: H | success | silent (pointers only) | not measured | | |
| OUT-5 | A inserts into an empty table | B: N, P, D0 | success | silent (pointers only) | not measured | | |
| OUT-6 | A deletes B's record | B: R | success | silent (pointers only) | not measured | | |
| OUT-7 | A deletes a record before or after B's record | B: R | success | silent (pointers only) | not measured | | |
| OUT-8 | A deletes the last record while B is on it | B: R, E | success | silent (pointers only) | not measured | | |
| OUT-9 | A deletes the only record while B is on it | B: R | success | silent (pointers only) | not measured | | |
| OUT-10 | A deletes a record | B: D, Xa, Xb, H | success | silent (pointers only) | not measured | | |
| OUT-11 | A updates B's value: same size, other size, large value | B: R | success | silent (pointers only) | not measured | | |
| OUT-12 | A adds a value to B's key (DUPSORT) | B: R·V1, R·Vm, R·Vn, R·Vs | success | silent (pointers only) | not measured | | |
| OUT-13 | A deletes B's value (DUPSORT) | B: R·Vm, R·Vn | success | silent (pointers only) | not measured | | |
| OUT-14 | A deletes all values of B's key (DUPSORT) | B: R (any V*) | success | silent (pointers only) | not measured | | |
| OUT-15 | A's insert or delete splits or merges B's page | B: R | success | silent (pointers only) | not measured | | |
| OUT-16 | A's call fails: `MDBX_KEYEXIST`, `MDBX_NOTFOUND`, `MDBX_EINVAL`, `MDBX_EKEYMISMATCH` | B: any bound | that error | silent (pointers only) | not measured | | |
| OUT-17 | A's call fails with `MDBX_MAP_FULL` | B: R | `MDBX_MAP_FULL` | silent (pointers only) | not measured | | |
| OUT-18 | A is a cursor in a read-only transaction; the call is `mdbx_put()` | B: R | `MDBX_EACCESS` | silent (pointers only) | not measured | | |
| OUT-19 | A runs `mdbx_cursor_delete_range()` or a range action of `mdbx_cursor_bunch_delete()` over B's record, or next to it | B: R | success | silent (pointers only) | not measured | | |
| OUT-20 | `mdbx_drop(del=false)` | B: any bound | success | "Empty or delete and close a table." `mdbx.h:5568 @ c26d8d4b`; silent on cursors | not measured | | |
| OUT-21 | `mdbx_drop(del=true)` | B: any bound | success | "Handles should only be closed if no other threads are going to reference the table handle or one of its cursors any further." `mdbx.h:5556-5557 @ c26d8d4b` | not measured | | |
| OUT-22 | `mdbx_env_defrag()` inside the user's own write transaction | B: any bound | success, `MDBX_RESULT_TRUE` | mdbx.h is silent on cursors | not measured | | |

Notes:

- OUT-6: B becomes D ([#304](https://github.com/Mithril-mine/libmdbx-devel/issues/304); `src/cursor-mut.c:1067-1079 @ c26d8d4b`). OUT-8, inferred: B becomes Xb (`src/cursor-mut.c:1105-1121 @ c26d8d4b`).
- OUT-13, inferred: B becomes Vd or Vt (`src/cursor-mut.c:986-1005 @ c26d8d4b`).
- OUT-17: see PUT-29.
- OUT-20, inferred: every cursor of the table becomes P, and N stays N (`src/table.c:184-187 @ c26d8d4b`).
- OUT-22, inferred: the defragmentation can run in the caller's write transaction (`src/api-gc.c:122-128 @ c26d8d4b`). Each cycle resets the cursors of moved tables (`src/defrag.c:1145-1146 @ c26d8d4b`) and then commits with a checkpoint (`src/api-gc.c:185 @ c26d8d4b`), which makes every cursor U (`src/txn-basal.c:321-322 @ c26d8d4b`). When `mdbx_env_defrag()` starts its own transaction, no user cursor can be in it, so I list no row for that case.

### 4.7 Transaction calls

#### 4.7.1 Calls that end the transaction (not measured)

The proposal says: record the rule `mdbx.h` already gives, and do not measure each case. The rule:
the cursor "must not be used, except with mdbx_cursor_renew() and mdbx_cursor_close()"
(`mdbx.h:4594-4599 @ c26d8d4b`). By code, every cursor becomes U at this point (`src/cursor-setup.c:264-271 @ c26d8d4b`,
called from `src/txn.c:49-62 @ c26d8d4b`). So C-1 and the U rows of 4.5.1 cover what follows.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| TXE-1 | `mdbx_txn_commit()`, `mdbx_txn_commit_ex()` | any | any result except `MDBX_THREAD_MISMATCH` | "Cursor(s) associated with transaction must not be used, except with mdbx_cursor_renew() and mdbx_cursor_close()." `mdbx.h:4594-4599 @ c26d8d4b`, `mdbx.h:4830-4835 @ c26d8d4b` | not measured (rule only) | | |
| TXE-2 | `mdbx_txn_abort()`, `mdbx_txn_abort_ex()` | any | any result except `MDBX_THREAD_MISMATCH` | "It and its cursors must not be used again after this call, except with mdbx_cursor_renew() and mdbx_cursor_close()." `mdbx.h:4883-4885 @ c26d8d4b` | not measured (rule only) | | |
| TXE-3 | `mdbx_txn_reset()` (read-only) | any | success | "Cursors opened within the transaction must not be used again after this call, except with mdbx_cursor_renew() and mdbx_cursor_close()." `mdbx.h:4949-4950 @ c26d8d4b` | not measured (rule only) | | |
| TXE-4 | `mdbx_txn_commit_embark_read()` | any | success | "similar to the sequence of calls mdbx_txn_commit_ex() and then mdbx_txn_begin(MDBX_TXN_RDONLY)"; cursor transfer is only planned `mdbx.h:4679-4685 @ c26d8d4b` | not measured (rule only) | | |
| TXE-5 | `mdbx_txn_amend()` | cursors of the read transaction, any | success | "the previous reading transaction is finished"; cursor transfer is only planned `mdbx.h:4721-4728 @ c26d8d4b` | not measured (rule only) | | |
| TXE-6 | nested `mdbx_txn_commit()` or `mdbx_txn_abort()` | cursors opened inside the child | any result | as TXE-1, TXE-2 | not measured (rule only) | | |

#### 4.7.2 Calls after which the transaction continues on the same handle (to measure)

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| TXC-1 | `mdbx_txn_checkpoint()` | any bound | success | "similar to the sequence of calls mdbx_txn_commit_ex() and then mdbx_txn_begin(MDBX_TXN_READWRITE)" `mdbx.h:4643-4645 @ c26d8d4b` | not measured | | |
| TXC-2 | `mdbx_txn_checkpoint()` | any bound | `MDBX_RESULT_TRUE` (no changes) | "no actions have been performed." `mdbx.h:4660-4661 @ c26d8d4b` | not measured | | |
| TXC-3 | `mdbx_txn_checkpoint()` on a nested transaction | the parent's cursors used in the child; cursors opened in the child | success | `mdbx.h:4643-4645 @ c26d8d4b` | not measured | | |
| TXC-4 | `mdbx_txn_rollback()`, write transaction | any bound | success | "Aborts and then restarts the transaction, rolling back all uncommitted changes." `mdbx.h:4781 @ c26d8d4b`; silent on cursors | not measured | | |
| TXC-5 | `mdbx_txn_rollback()`, read-only transaction | any bound | success | `mdbx.h:4781 @ c26d8d4b`; silent on cursors | not measured | | |
| TXC-6 | `mdbx_txn_rollback()` on a nested transaction | the parent's cursors used in the child; cursors opened in the child | success | `mdbx.h:4785-4787 @ c26d8d4b`; silent on cursors | not measured | | |
| TXC-7 | `mdbx_txn_refresh()` | any bound | success | "Refresh a read-only transaction for a recent data." `mdbx.h:5087 @ c26d8d4b`; silent on cursors | not measured | | |
| TXC-8 | `mdbx_txn_refresh()` | any bound | `MDBX_RESULT_TRUE` (already recent) | "no actions have been performed." `mdbx.h:5099-5101 @ c26d8d4b` | not measured | | |
| TXC-9 | `mdbx_txn_park()` | any bound | success | "To continue execution ... the parked transaction must be restored using mdbx_txn_unpark()" `mdbx.h:4982-4984 @ c26d8d4b` | not measured | | |
| TXC-10 | a cursor call on a parked transaction, auto-unpark on | any bound | as the call's own rows | "automatic un-parking is provided when using a parked transaction in API functions involving data reading." `mdbx.h:4983-4984 @ c26d8d4b` | not measured | | |
| TXC-11 | a cursor call on a parked transaction, auto-unpark off | any bound | `MDBX_BAD_TXN` (to measure) | mdbx.h is silent | not measured | | |
| TXC-12 | `mdbx_txn_unpark()` | any bound | success | "The parked transaction was successfully restored" `mdbx.h:5042 @ c26d8d4b` | not measured | | |
| TXC-13 | `mdbx_txn_unpark(restart_if_ousted=false)` | any bound | `MDBX_OUSTED` | "The transaction is reset to a similar state after calling mdbx_txn_reset()" `mdbx.h:5044-5048 @ c26d8d4b` | not measured | | |
| TXC-14 | `mdbx_txn_unpark(restart_if_ousted=true)` | any bound | `MDBX_RESULT_TRUE` (restarted) | "ousted but has now been restarted to read recent MVCC-snapshot" `mdbx.h:5050-5051 @ c26d8d4b` | not measured | | |
| TXC-15 | `mdbx_txn_renew()` after `mdbx_txn_reset()` | U (from TXE-3) | success | "It must be called before a reset transaction may be used again." `mdbx.h:5065-5067 @ c26d8d4b` | not measured | | |
| TXC-16 | `mdbx_txn_break()` | any bound | success | "makes impossible to perform any operations within a broken transaction." `mdbx.h:4927-4929 @ c26d8d4b` | not measured (failure model) | | |

Notes (all inferred):

- TXC-1, TXC-4, TXC-5, TXC-7: the code makes every cursor U although the handle continues (`src/txn-basal.c:321-322 @ c26d8d4b` through `src/txn-basal.c:505-510 @ c26d8d4b`; `src/api-txn.c:730-732 @ c26d8d4b`; `src/txn-ro.c:296-297 @ c26d8d4b` through `src/api-txn.c:199-202 @ c26d8d4b`). For checkpoint this follows from the `mdbx.h` sentence. For rollback and refresh `mdbx.h` says nothing.
- TXC-2, TXC-8: no change (`src/api-txn.c:449-450 @ c26d8d4b`, `src/api-txn.c:200-201 @ c26d8d4b`).
- TXC-13, TXC-14: an ousted transaction is reset, so the cursors become U, as after TXE-3.
- TXC-15: the cursors stay U; the user must call `mdbx_cursor_renew()`.
- TXC-16: the cursor stays bound and every call fails (C-3). This belongs to the failure-model contract.

#### 4.7.3 Nested transactions

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| NST-1 | any cursor call on a parent's cursor while a child is active | any bound | success (the call runs in the child) | "A parent transaction and its cursors may not issue any other operations than mdbx_txn_commit and mdbx_txn_abort() while it has active child transactions." `mdbx.h:4182-4185 @ c26d8d4b`; `mdbx.h:6143-6145 @ c26d8d4b` | not measured | | ask? |
| NST-2 | child `mdbx_txn_commit()` | a parent's cursor used in the child | success | "the cursor should not be used in its parent transaction ... nor impossible to define the expected behavior." `mdbx.h:6143-6145 @ c26d8d4b` | not measured | | |
| NST-3 | child `mdbx_txn_abort()` | a parent's cursor used in the child | success | "Otherwise it is no way to restore state if this nested transaction will be aborted" `mdbx.h:6143-6145 @ c26d8d4b` | not measured | | |
| NST-4 | child `mdbx_txn_commit()` or `mdbx_txn_abort()` | a parent's cursor not used in the child | success | mdbx.h is silent | not measured | | |
| NST-5 | child `mdbx_txn_commit()` fails | a parent's cursor used in the child | error | mdbx.h is silent | not measured | | |
| NST-6 | child ends | a parent's cursor closed inside the child (LC-25) | any result | `mdbx.h:6238-6239 @ c26d8d4b` | not measured | | |

Notes:

- NST-1, ask? (A5): the code moves ("shadows") the parent's cursor into the child on the first use (`src/cursor-setup.c:557-570 @ c26d8d4b`, `src/cursor-setup.c:201-229 @ c26d8d4b`); cursors of the main table are shadowed when the child starts (`src/txn-nested.c:433 @ c26d8d4b`). So the parent's cursors do work while the child is active.
- NST-2, inferred: the cursor keeps the position it reached in the child (`src/cursor-setup.c:244-254 @ c26d8d4b`). NST-3, inferred: the cursor returns to its state before the child (`src/cursor-setup.c:255-261 @ c26d8d4b`). This is [#326](https://github.com/Mithril-mine/libmdbx-devel/issues/326).
- NST-5, inferred: a failed child commit restores the parent's cursors as an abort does (`src/txn-nested.c:674-681 @ c26d8d4b`).

### 4.8 Errors in the middle of an operation

These need the probe build (`MDBX_PROBES`) and probably new fault sites in the cursor code.
If the error poisons the transaction, the cursor state after it belongs to the failure-model contract.

| ID | Call | Starting state | Outcome | What `mdbx.h` promises | State today (measured) | Proposed rule | Mark |
|---|---|---|---|---|---|---|---|
| MID-1 | every `mdbx_cursor_get()` operation that reads a page; `scan`, `scan_from`, `get_batch`, `scroll`, `distribute` | any bound | `MDBX_CORRUPTED`, `MDBX_EIO`, page-read errors | mdbx.h is silent | not measured yet (needs MDBX_PROBES) | | |
| MID-2 | `mdbx_cursor_put()` | any bound | `MDBX_ENOMEM`, `MDBX_EIO`, `MDBX_CORRUPTED`, `MDBX_TXN_FULL` | "MDBX_TXN_FULL The transaction has too many dirty pages." `mdbx.h:6759 @ c26d8d4b` | not measured yet (needs MDBX_PROBES) | | |
| MID-3 | `mdbx_cursor_del()`, `mdbx_cursor_delete_range()`, `mdbx_cursor_bunch_delete()` | R | `MDBX_ENOMEM`, `MDBX_EIO`, `MDBX_CORRUPTED`, `MDBX_TXN_FULL` | "MDBX_TXN_FULL The transaction has too many dirty pages." `mdbx.h:6800 @ c26d8d4b` | not measured yet (needs MDBX_PROBES) | | |
| MID-4 | `mdbx_cursor_open()`, `mdbx_cursor_bind()`, first use of a parent's cursor in a child | any | `MDBX_ENOMEM` | mdbx.h is silent | not measured yet (needs MDBX_PROBES) | | |
| MID-5 | any positioning call | any bound | `MDBX_CURSOR_FULL` (tree deeper than the cursor stack) | mdbx.h is silent | not measured yet (needs MDBX_PROBES) | | |
| MID-6 | `mdbx_txn_checkpoint()`, `mdbx_txn_rollback()`, nested commit | any bound | `MDBX_ENOMEM`, `MDBX_EIO` | mdbx.h is silent on cursors | not measured yet (needs MDBX_PROBES) | | |

Notes:

- MID-5: the code resets the cursor and marks the transaction as failed (`src/cursor.h:390-394 @ c26d8d4b`), so this is a failure-model case.
- MID-1, inferred: on a page error during a move the cursor is reset to P (`src/cursor-nav.c:46-49 @ c26d8d4b`, `src/tree-search.c:30-35 @ c26d8d4b`).

## 5. Disagreements marked "ask?"

Each item: what `mdbx.h` says, what the code does, and the rows that carry the mark.
"Inferred" means I read it in the code and have not measured it yet.

| ID | Topic | `mdbx.h` says | The code does | Rows |
|---|---|---|---|---|
| A1 | relative moves after reset | no relative moves after `mdbx_cursor_reset()` (`mdbx.h:6194-6196 @ c26d8d4b`) | reset gives N; from N every relative move acts as `MDBX_FIRST`/`MDBX_LAST` (`src/api-cursor.c:79 @ c26d8d4b`, `src/cursor.h:543-547 @ c26d8d4b`, `src/cursor-nav.c:161-168 @ c26d8d4b`) | REL-1, REL-17, REL-30, REL-38, REL-45, REL-53, NM-3, LC-22 |
| A2 | meaning of `mdbx_cursor_eof()` | TRUE means not pointed to a pair; FALSE means "A data is available" (`mdbx.h:7029-7030 @ c26d8d4b`, `mdbx.h:7037-7039 @ c26d8d4b`) | TRUE in E, where the last record is still readable (`src/cursor.h:205-208 @ c26d8d4b`, `src/cursor-nav.c:117-119 @ c26d8d4b`); inferred: FALSE after `MDBX_LAST` when the last key has several values (`src/cursor-nav.c:84-108 @ c26d8d4b`) | ABS-3, REL-5, REL-13, REL-49 |
| A3 | state after a delete | "Both MDBX_NEXT and MDBX_GET_CURRENT will return the same record" (`mdbx.h:6768-6771 @ c26d8d4b`) | inferred: after deleting the last record they differ (Xb: `MDBX_NOTFOUND` against the new last record); after deleting the only record they differ (`MDBX_NOTFOUND` against `MDBX_ENODATA`) (`src/cursor-mut.c:1089-1097 @ c26d8d4b`, `src/cursor-mut.c:1112-1117 @ c26d8d4b`, `src/cursor-nav.c:280 @ c26d8d4b`, `src/cursor-seek.c:279-297 @ c26d8d4b`) | DEL-2, DEL-3, DEL-4, DEL-14, REL-10, REL-61 |
| A4 | `MDBX_APPEND` error code | `mdbx_cursor_put()`: `MDBX_KEYEXIST` (`mdbx.h:6728-6729 @ c26d8d4b`); `mdbx_put()`: `MDBX_EKEYMISMATCH` (`mdbx.h:5904-5906 @ c26d8d4b`) | `MDBX_EKEYMISMATCH` (`src/cursor-mut.c:137-139 @ c26d8d4b`, `src/cursor-mut.c:181-182 @ c26d8d4b`, `src/cursor-mut.c:400-401 @ c26d8d4b`) | PUT-20, PUT-23 |
| A5 | parent's cursors while a child is active | the parent and its cursors may not issue operations (`mdbx.h:4182-4185 @ c26d8d4b`, `mdbx.h:6143-6145 @ c26d8d4b`) | the cursor is moved into the child and works (`src/cursor-setup.c:557-570 @ c26d8d4b`) | NST-1 |
| A6 | "not positioned to a data" check | `MDBX_ENODATA` when the cursor is not positioned to a data (`mdbx.h:6828 @ c26d8d4b`, `mdbx.h:6894 @ c26d8d4b`, `mdbx.h:6932 @ c26d8d4b`, `mdbx.h:6980 @ c26d8d4b`) | checks only "has a stack", so H and Xa pass (`src/api-cursor.c:936-945 @ c26d8d4b`, `src/api-cursor.c:1001-1010 @ c26d8d4b`, `src/api-cursor.c:1071-1072 @ c26d8d4b`, `src/tree-cutoff.c:518-519 @ c26d8d4b`) | RNG-7, BUN-7, BUN-12, BUN-16, BUN-21, SCR-6, DIS-4 |
| A7 | "after, excluding" actions from N | `MDBX_ENODATA` when not positioned (`mdbx.h:6980 @ c26d8d4b`) | inferred: the internal next step acts as `MDBX_FIRST` from N, so the call deletes data (`src/api-cursor.c:816-822 @ c26d8d4b`, `src/api-cursor.c:879-885 @ c26d8d4b`) | BUN-11, BUN-20 |
| A8 | copy from a created cursor | the source may come from `mdbx_cursor_create()` (`mdbx.h:6378-6379 @ c26d8d4b`) | a created cursor is unbound and is rejected with `MDBX_EINVAL` (`src/api-cursor.c:226-228 @ c26d8d4b`) | LC-27 |
| A9 | `MDBX_SET_LOWERBOUND` and `MDBX_SET_UPPERBOUND` on a plain table | work "the same to" / "like" `MDBX_SET_RANGE` (`mdbx.h:1881-1883 @ c26d8d4b`, `mdbx.h:1895-1897 @ c26d8d4b`) | require a data pointer, unlike `MDBX_SET_RANGE` (`src/cursor-seek.c:451-452 @ c26d8d4b`); measured (M-7). `mdbx.h` itself does not say that `MDBX_SET_RANGE` accepts NULL data (the code does, `src/cursor-seek.c:333-334 @ c26d8d4b`), and it says both operations "return both key and data" (`mdbx.h:1876-1877 @ c26d8d4b`, `mdbx.h:1890-1891 @ c26d8d4b`). Version 1: accept NULL data on a plain table, as `MDBX_SET_RANGE` does. Version 2: keep `MDBX_EINVAL` and document it | LB-9, UB-7 |
| A10 | `on_first`/`on_last` on H | TRUE when not positioned (`mdbx.h:7056-7059 @ c26d8d4b`, `mdbx.h:7102-7105 @ c26d8d4b`); "tells whether it makes sense to move the cursor backward" (`mdbx.h:7047-7050 @ c26d8d4b`) | computed from the stale index of H (`src/cursor-nav.c:331-346 @ c26d8d4b`); measured (M-6). Version 1: TRUE on H, as `mdbx.h` says (no move from H works). Version 2: keep the code and document that they are meaningful only when `GET_CURRENT` succeeds. I lean to version 1 | state-level (2.5); SET-2, SET-3, SET-6, SETK-2, SETK-3, SETK-6 |
| A11 | `MDBX_SET_LOWERBOUND` on a DUPSORT table, a value with a NULL pointer | mdbx.h is silent | skips the value search and always returns `MDBX_RESULT_TRUE` with the first value, even when the key matches exactly (`src/cursor-seek.c:457-462 @ c26d8d4b`, `src/cursor-seek.c:478-479 @ c26d8d4b`); measured (M-8). Version 1: keep, the NULL value is below all values. Version 2: the NULL value means "any value", `MDBX_SUCCESS` for an exact key, as on a plain table | LB-10 |

## 6. Row count

| Group | Rows |
|---|---|
| 4.0 common | 4 |
| 4.1 absolute | 10 |
| 4.2 relative | 69 |
| 4.3.1 `MDBX_SET` family | 39 |
| 4.3.2 `MDBX_GET_BOTH`, `MDBX_GET_BOTH_RANGE` | 15 |
| 4.3.3 `MDBX_TO_KEY_*` | 30 |
| 4.3.4 `MDBX_TO_EXACT_KEY_VALUE_*` | 16 |
| 4.3.5 `MDBX_TO_PAIR_*` | 19 |
| 4.4 multiple values | 24 |
| 4.5.1 life cycle | 32 |
| 4.5.2 put | 29 |
| 4.5.3 del | 18 |
| 4.5.4 delete_range | 11 |
| 4.5.5 bunch_delete | 25 |
| 4.5.6 get_batch | 11 |
| 4.5.7 scan, scan_from | 17 |
| 4.5.8 scroll | 7 |
| 4.5.9 distribute | 6 |
| 4.6 effects from outside | 22 |
| 4.7.1 transaction ends | 6 |
| 4.7.2 transaction continues | 16 |
| 4.7.3 nested transactions | 6 |
| 4.8 errors in the middle | 6 |
| **Total** | **438** |
