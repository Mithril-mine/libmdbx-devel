/* perf-d-cxx-inline.cpp — stream D probe: verify the C++ wrapper zero-overhead claim.
 *
 * Compiles to assembly (-S -O2). Expected per function:
 *  - txn_get_probe:    exactly one call to _mdbx_get (the C API) + branch on rc; no slice copies.
 *  - cursor_move_probe: one call to _mdbx_cursor_get + switch on rc.
 *  - slice ops:        inline pointer arithmetic only (no library calls at all).
 * Anything beyond that (memcpy, extra calls, re-validation) would be a wrapper overhead.
 */
#include "mdbx.h++"

using namespace mdbx;

slice txn_get_probe(const txn &t, map_handle m, const slice &k) { return t.get(m, k); }

bool cursor_move_probe(cursor &c, const slice &k) {
  auto r = c.find(k, false);
  return r.done;
}

size_t slice_len_probe(const slice &s) { return s.length(); }

bool slice_eq_probe(const slice &a, const slice &b) { return a == b; }

MDBX_cache_result_t cache_probe(const txn &t, map_handle m, const slice &k, slice &out, cache_entry &e) {
  return t.get_cached(m, k, &out, e);
}
