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
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
/// \author Alkoyd <alkoyd@symbolsyndicate.us> \date 2026

// Measuring program for docs/engineering/cursor-contract.md (the MDBX_SET family).
//
// For every combination of table, starting state, call and key argument it brings a cursor into the
// starting state with public API calls only, runs the call, and records the resulting state with the
// probe set (on mdbx_cursor_copy() copies, so the probes do not move the cursor under test).
// It PRINTS one line per scenario and does NOT assert today's behaviour: only the setup steps are
// asserted. Lines start with `CM|`.

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../test-env.h"

namespace {

const char *const kDb = "contract-measure.mdbx";

// ---------------------------------------------------------------------------------------------
// Small helpers

std::string rcname(int rc) {
  if (rc == MDBX_SUCCESS)
    return "OK";
  if (rc == MDBX_NOTFOUND)
    return "NOTFOUND";
  if (rc == MDBX_ENODATA)
    return "ENODATA";
  if (rc == MDBX_RESULT_TRUE)
    return "TRUE";
  if (rc == MDBX_EINVAL)
    return "EINVAL";
  if (rc == MDBX_BAD_VALSIZE)
    return "BAD_VALSIZE";
  if (rc == MDBX_EBADSIGN)
    return "EBADSIGN";
  if (rc == MDBX_KEYEXIST)
    return "KEYEXIST";
  return "rc" + std::to_string(rc);
}

std::string show(const MDBX_val &v) {
  if (!v.iov_base || !v.iov_len)
    return "";
  const std::string s(static_cast<const char *>(v.iov_base), v.iov_len);
  // keys are short; long values are shown as the first byte, a tilde and the length
  if (s.size() <= 8)
    return s;
  return s.substr(0, 1) + "~" + std::to_string(s.size());
}

MDBX_val val_of(const std::string &s) {
  MDBX_val v;
  v.iov_base = const_cast<char *>(s.data());
  v.iov_len = s.size();
  return v;
}

// ---------------------------------------------------------------------------------------------
// Tables

struct table {
  const char *name;
  bool dup;
  bool empty;
  std::vector<std::string> keys;
  std::vector<int> nvals; // values per key (dup tables)
  size_t pad;             // value padding
  MDBX_dbi dbi = 0;
  unsigned depth = 0;

  std::string value(size_t, int vi) const { return std::string(1, char('a' + vi)) + std::string(pad, 'p'); }
  std::string value_between_b_and_c() const { return "b" + std::string(pad, 'p') + "~"; }
  size_t n() const { return keys.size(); }
};

std::string make_key(size_t i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "k%05zu", i);
  return buf;
}

table make_table(const char *name, bool dup, size_t count, size_t pad) {
  table t;
  t.name = name;
  t.dup = dup;
  t.empty = count == 0;
  t.pad = pad;
  for (size_t i = 0; i < count; ++i) {
    t.keys.push_back(make_key((i + 1) * 10));
    t.nvals.push_back(dup ? ((i + 1 == count || (i & 1)) ? 3 : 1) : 1);
  }
  return t;
}

void fill(MDBX_txn *txn, const table &t) {
  for (size_t i = 0; i < t.n(); ++i) {
    MDBX_val k = val_of(t.keys[i]);
    for (int v = 0; v < t.nvals[i]; ++v) {
      const std::string s = t.value(i, v);
      MDBX_val d = val_of(s);
      ASSERT_EQ(mdbx_put(txn, t.dbi, &k, &d, MDBX_UPSERT), MDBX_SUCCESS);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Probe set and the state classifier

struct probes {
  std::string eof, first, last, fdup, ldup, gc, next, prev;
};

std::string tf(int rc) {
  if (rc == MDBX_RESULT_TRUE)
    return "T";
  if (rc == MDBX_RESULT_FALSE)
    return "F";
  return rcname(rc);
}

template <typename F> std::string on_copy(const MDBX_cursor *src, F &&f) {
  MDBX_cursor *c = mdbx_cursor_create(nullptr);
  if (!c)
    return "nocursor";
  std::string r;
  const int rc = mdbx_cursor_copy(src, c);
  if (rc != MDBX_SUCCESS)
    r = "copy:" + rcname(rc);
  else
    r = f(c);
  mdbx_cursor_close(c);
  return r;
}

std::string get_rec(const MDBX_cursor *src, MDBX_cursor_op op) {
  return on_copy(src, [&](MDBX_cursor *c) {
    MDBX_val k = {nullptr, 0}, d = {nullptr, 0};
    const int rc = mdbx_cursor_get(c, &k, &d, op);
    return rc == MDBX_SUCCESS ? "{" + show(k) + "/" + show(d) + "}" : rcname(rc);
  });
}

probes probe(const MDBX_cursor *src) {
  probes p;
  p.eof = on_copy(src, [](MDBX_cursor *c) { return tf(mdbx_cursor_eof(c)); });
  p.first = on_copy(src, [](MDBX_cursor *c) { return tf(mdbx_cursor_on_first(c)); });
  p.last = on_copy(src, [](MDBX_cursor *c) { return tf(mdbx_cursor_on_last(c)); });
  p.fdup = on_copy(src, [](MDBX_cursor *c) { return tf(mdbx_cursor_on_first_dup(c)); });
  p.ldup = on_copy(src, [](MDBX_cursor *c) { return tf(mdbx_cursor_on_last_dup(c)); });
  p.gc = get_rec(src, MDBX_GET_CURRENT);
  p.next = get_rec(src, MDBX_NEXT);
  p.prev = get_rec(src, MDBX_PREV);
  return p;
}

bool is_rec(const std::string &s) { return !s.empty() && s[0] == '{'; }

// Maps the observation to the document's state names (sections 2.1 and 2.2).
std::string classify(const probes &p, bool dup) {
  if (p.gc == "EINVAL" && p.next == "EINVAL" && p.prev == "EINVAL")
    return "U";
  std::string st;
  if (p.eof == "T") {
    if (p.gc == "ENODATA") {
      if (p.next == "ENODATA" && p.prev == "ENODATA")
        st = (p.first == "T" && p.last == "T") ? "P-or-H" : "H"; // both TRUE: an empty stack or a one-key page
      else if (is_rec(p.next) && is_rec(p.prev))
        st = "N";
      else if (p.next == "NOTFOUND" && p.prev == "NOTFOUND")
        st = "N-or-D0"; // N on an empty table, or D0: the probes cannot tell them apart
    } else if (p.gc == "NOTFOUND") {
      if (p.next == "NOTFOUND" && is_rec(p.prev))
        st = "Xa";
    } else if (is_rec(p.gc)) {
      if (p.next == "NOTFOUND")
        st = (p.prev == p.gc) ? "Xb" : "E";
    }
  } else if (p.eof == "F") {
    if (is_rec(p.gc))
      st = (p.next == p.gc) ? "D" : "R";
    else if (p.gc == "NOTFOUND" && is_rec(p.prev))
      return is_rec(p.next) ? "Vx" : "Vx-or-Vt(last key)"; // dup tables only: past the last value of the key
  }
  if (st.empty())
    return "unmatched";
  if (dup && (st == "R" || st == "E" || st == "D" || st == "Xb")) {
    if (p.fdup == "T" && p.ldup == "T")
      st += ".Vs";
    else if (p.fdup == "T" && p.ldup == "F")
      st += ".V1";
    else if (p.fdup == "F" && p.ldup == "F")
      st += ".Vm";
    else if (p.fdup == "F" && p.ldup == "T")
      st += ".Vn";
  }
  return st;
}

std::string raw(const probes &p) {
  return "eof=" + p.eof + " first=" + p.first + " last=" + p.last + " fdup=" + p.fdup + " ldup=" + p.ldup +
         " GC=" + p.gc + " NEXT=" + p.next + " PREV=" + p.prev;
}

// ---------------------------------------------------------------------------------------------
// Starting states

struct start {
  const char *name;
  const char *intended; // the document's state name this recipe aims at
  bool anchored;        // the cursor keeps a position (not N, P, D0)
  bool rightmost;       // the anchor is on the rightmost leaf page
  bool for_dup_only;
  bool for_nonempty_only;
  bool one_only;                                         // only for the single-record tables
  bool (*run)(MDBX_txn *, MDBX_cursor *, const table &); // returns false if a setup step failed
};

int cget(MDBX_cursor *c, MDBX_cursor_op op, const std::string &key, const std::string *data = nullptr) {
  MDBX_val k = val_of(key), d = {nullptr, 0};
  if (data)
    d = val_of(*data);
  return mdbx_cursor_get(c, &k, &d, op);
}

int cmove(MDBX_cursor *c, MDBX_cursor_op op) {
  MDBX_val k = {nullptr, 0}, d = {nullptr, 0};
  return mdbx_cursor_get(c, &k, &d, op);
}

const std::string kHigh = "k99999";
const std::string kLow = "k00005";

std::string mid_miss(const table &t) {
  size_t v = 0;
  sscanf(t.keys[t.n() / 2].c_str() + 1, "%zu", &v);
  return make_key(v + 5);
}

#define OK_(x) ((x) == MDBX_SUCCESS)

bool st_new(MDBX_txn *, MDBX_cursor *, const table &) { return true; }

bool st_P(MDBX_txn *txn, MDBX_cursor *c, const table &t) {
  if (!OK_(cget(c, MDBX_SET, t.keys[t.n() / 2])))
    return false;
  if (!OK_(mdbx_drop(txn, t.dbi, false)))
    return false;
  for (size_t i = 0; i < t.n(); ++i) {
    MDBX_val k = val_of(t.keys[i]);
    for (int v = 0; v < t.nvals[i]; ++v) {
      const std::string s = t.value(i, v);
      MDBX_val d = val_of(s);
      if (!OK_(mdbx_put(txn, t.dbi, &k, &d, MDBX_UPSERT)))
        return false;
    }
  }
  return true;
}

bool st_Hlo(MDBX_txn *, MDBX_cursor *c, const table &) { return cget(c, MDBX_SET, kLow) == MDBX_NOTFOUND; }
bool st_Hmid(MDBX_txn *, MDBX_cursor *c, const table &t) { return cget(c, MDBX_SET, mid_miss(t)) == MDBX_NOTFOUND; }
bool st_Hhi(MDBX_txn *, MDBX_cursor *c, const table &) { return cget(c, MDBX_SET, kHigh) == MDBX_NOTFOUND; }

bool st_Rf(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[0])); }
bool st_Rm(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[t.n() / 2])); }
bool st_Rr(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[t.n() - 2])); }
bool st_Rl(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[t.n() - 1])); }

bool st_Elast(MDBX_txn *, MDBX_cursor *c, const table &) { return OK_(cmove(c, MDBX_LAST)); }
bool st_Enext(MDBX_txn *, MDBX_cursor *c, const table &) {
  return OK_(cmove(c, MDBX_LAST)) && cmove(c, MDBX_NEXT) == MDBX_NOTFOUND;
}
bool st_EnextRl(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[t.n() - 1])) && (!t.dup || OK_(cmove(c, MDBX_LAST_DUP))) &&
         cmove(c, MDBX_NEXT) == MDBX_NOTFOUND;
}

bool st_XrN(MDBX_txn *, MDBX_cursor *c, const table &) { return cget(c, MDBX_SET_RANGE, kHigh) == MDBX_NOTFOUND; }
bool st_XrRl(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[t.n() - 1])) && cget(c, MDBX_SET_RANGE, kHigh) == MDBX_NOTFOUND;
}
bool st_XrRf(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[0])) && cget(c, MDBX_SET_RANGE, kHigh) == MDBX_NOTFOUND;
}
bool st_XubL(MDBX_txn *, MDBX_cursor *c, const table &t) {
  // exact pair of the very last record, then SET_UPPERBOUND: nothing greater
  const std::string &k = t.keys[t.n() - 1];
  const std::string d = t.value(t.n() - 1, t.nvals[t.n() - 1] - 1);
  MDBX_val kk = val_of(k), dd = val_of(d);
  return mdbx_cursor_get(c, &kk, &dd, MDBX_SET_UPPERBOUND) == MDBX_NOTFOUND;
}

bool del_(MDBX_cursor *c, const table &t) { return OK_(mdbx_cursor_del(c, t.dup ? MDBX_ALLDUPS : MDBX_CURRENT)); }
bool st_Dmid(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[t.n() / 2])) && del_(c, t);
}
bool st_Dfirst(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[0])) && del_(c, t); }
bool st_Dlast(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[t.n() - 1])) && del_(c, t);
}

// DUPSORT value axis (keys[1] has three values, so does the last key)
bool st_Rv1(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[1])); }
bool st_Rvm(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[1])) && OK_(cmove(c, MDBX_NEXT_DUP));
}
bool st_Rvn(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[1])) && OK_(cmove(c, MDBX_LAST_DUP));
}
bool st_Rvs(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[0])); }
bool st_RvnLast(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[t.n() - 1])) && OK_(cmove(c, MDBX_LAST_DUP));
}
bool st_Vd(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[1])) && OK_(cmove(c, MDBX_NEXT_DUP)) && OK_(mdbx_cursor_del(c, MDBX_CURRENT));
}
bool st_Vt(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[1])) && OK_(cmove(c, MDBX_LAST_DUP)) && OK_(mdbx_cursor_del(c, MDBX_CURRENT));
}
bool st_VtLast(MDBX_txn *, MDBX_cursor *c, const table &t) {
  return OK_(cget(c, MDBX_SET, t.keys[t.n() - 1])) && OK_(cmove(c, MDBX_LAST_DUP)) &&
         OK_(mdbx_cursor_del(c, MDBX_CURRENT));
}
bool vx(MDBX_cursor *c, const table &t, size_t ki) {
  if (!OK_(cget(c, MDBX_SET, t.keys[ki])))
    return false;
  // a miss of the exact pair search among the values of an existing key
  const std::string btw = t.value_between_b_and_c();
  return cget(c, MDBX_TO_PAIR_EQUAL, t.keys[ki], &btw) == MDBX_NOTFOUND;
}
bool st_Vx(MDBX_txn *, MDBX_cursor *c, const table &t) { return vx(c, t, 1); }
bool st_VxLast(MDBX_txn *, MDBX_cursor *c, const table &t) { return vx(c, t, t.n() - 1); }

bool st_D0(MDBX_txn *, MDBX_cursor *c, const table &t) { return OK_(cget(c, MDBX_SET, t.keys[0])) && del_(c, t); }

//                                                  name        int  anch  rmost dup    nonempty one
const start kStarts[] = {
    {"N", "N", false, false, false, false, false, st_new},
    {"P", "P", false, false, false, true, false, st_P},
    {"Hlo", "H", true, false, false, true, false, st_Hlo},
    {"Hmid", "H", true, false, false, true, false, st_Hmid},
    {"Hhi", "H", true, true, false, true, false, st_Hhi},
    {"Rfirst", "R", true, false, false, true, false, st_Rf},
    {"Rmid", "R", true, false, false, true, false, st_Rm},
    {"Rnearlast", "R", true, true, false, true, false, st_Rr},
    {"Rlast", "R", true, true, false, true, false, st_Rl},
    {"E_afterLAST", "E", true, true, false, true, false, st_Elast},
    {"E_afterNEXT", "E", true, true, false, true, false, st_Enext},
    {"E_afterNEXT_Rlast", "E", true, true, false, true, false, st_EnextRl},
    {"Xr_fromN", "Xa/Xb", true, true, false, true, false, st_XrN},
    {"Xr_fromRlast", "Xa/Xb", true, true, false, true, false, st_XrRl},
    {"Xr_fromRfirst", "Xa/Xb", true, true, false, true, false, st_XrRf},
    {"Xub_lastrec", "Xb", true, true, false, true, false, st_XubL},
    {"Dmid", "D", true, false, false, true, false, st_Dmid},
    {"Dfirst", "D", true, false, false, true, false, st_Dfirst},
    {"Dlast", "Xb", true, true, false, true, false, st_Dlast},
    {"Rv1", "R.V1", true, false, true, true, false, st_Rv1},
    {"Rvm", "R.Vm", true, false, true, true, false, st_Rvm},
    {"Rvn", "R.Vn", true, false, true, true, false, st_Rvn},
    {"Rvs", "R.Vs", true, false, true, true, false, st_Rvs},
    {"RvnLastKey", "R.Vn", true, true, true, true, false, st_RvnLast},
    {"Vd", "R.Vd", true, false, true, true, false, st_Vd},
    {"Vt", "R.Vt", true, false, true, true, false, st_Vt},
    {"VtLastKey", "R.Vt", true, true, true, true, false, st_VtLast},
    {"Vx", "R.Vx", true, false, true, true, false, st_Vx},
    {"VxLastKey", "R.Vx", true, true, true, true, false, st_VxLast},
    {"D0", "D0", false, false, false, true, true, st_D0},
};

// ---------------------------------------------------------------------------------------------
// Calls

enum class kop { SET, SETK, SR, LB, UB };
const char *opname(kop o) {
  switch (o) {
  case kop::SET:
    return "MDBX_SET";
  case kop::SETK:
    return "MDBX_SET_KEY";
  case kop::SR:
    return "MDBX_SET_RANGE";
  case kop::LB:
    return "MDBX_SET_LOWERBOUND";
  default:
    return "MDBX_SET_UPPERBOUND";
  }
}
MDBX_cursor_op cop(kop o) {
  switch (o) {
  case kop::SET:
    return MDBX_SET;
  case kop::SETK:
    return MDBX_SET_KEY;
  case kop::SR:
    return MDBX_SET_RANGE;
  case kop::LB:
    return MDBX_SET_LOWERBOUND;
  default:
    return MDBX_SET_UPPERBOUND;
  }
}

enum class cls {
  HIT,       // an existing key (LB/UB on a dup table: an existing pair)
  GT,        // absent key, a greater one exists (LB/UB on dup: pair absent, a greater pair exists)
  GTV,       // dup: the key exists, the value is greater than all of its values, a next key exists
  PAST,      // greater than the last key
  PASTV,     // dup: the last key, the value is greater than all of its values
  LASTHIT,   // UB: the very last record
  NULLKEY,   // key parameter is NULL
  BADKEY,    // key longer than the limit
  DATA_NULL, // the data parameter is NULL
  DATA_PTR0, // the data parameter points to a value with a NULL pointer
};

struct call {
  std::string label;
  cls c;
  bool key_null = false;
  std::string key;
  bool data_null = false;
  bool data_ptr0 = false;
  bool has_data = false;
  std::string data;
};

std::vector<call> make_calls(kop o, const table &t, size_t maxkey) {
  std::vector<call> v;
  auto add = [&](const char *label, cls c, std::string key, const std::string *data = nullptr) {
    call x;
    x.label = label;
    x.c = c;
    x.key = std::move(key);
    if (data) {
      x.has_data = true;
      x.data = *data;
    }
    v.push_back(std::move(x));
  };
  const bool pairs = t.dup && (o == kop::LB || o == kop::UB);
  const std::string midk = t.empty ? make_key(30) : t.keys[t.n() / 2];
  const std::string k1 = t.n() >= 2 ? t.keys[1] : t.empty ? make_key(20) : t.keys[0];
  const std::string lastk = t.empty ? make_key(30) : t.keys[t.n() - 1];
  const std::string kmiss = t.empty ? make_key(35) : mid_miss(t);
  const std::string zero = "";
  const std::string zz = "z";
  const std::string a = t.empty ? "a" : t.value(0, 0);

  if (!pairs) {
    // LB/UB on a plain table still need a data parameter; the value is ignored for the search
    const std::string *d = (o == kop::LB || o == kop::UB) ? &zero : nullptr;
    add("hit_mid", cls::HIT, midk, d);
    add("hit_last", o == kop::UB ? cls::LASTHIT : cls::HIT, lastk, d);
    add("miss_low", cls::GT, kLow, d);
    add("miss_mid", cls::GT, kmiss, d);
    add("miss_high", cls::PAST, kHigh, d);
    if (t.dup && !t.empty && (o == kop::SET || o == kop::SETK || o == kop::SR))
      add("hit_multi", cls::HIT, k1, d);
  } else {
    const std::string b = t.value(1, 1), c = t.value(1, 2), btw = t.value_between_b_and_c();
    add("hit_pair_single", cls::HIT, midk, &a);
    add("hit_pair_multi_mid", cls::HIT, k1, &b);
    add("hit_pair_multi_last", cls::HIT, k1, &c);
    add("pair_between_values", cls::GT, k1, &btw);
    add("key_hit_value_below_all", cls::HIT, k1, &zero);
    add("key_hit_value_above_all", cls::GTV, k1, &zz);
    add("last_key_value_above_all", cls::PASTV, lastk, &zz);
    add("last_record_pair", o == kop::UB ? cls::LASTHIT : cls::HIT, lastk, &a);
    add("miss_low", cls::GT, kLow, &a);
    add("miss_mid", cls::GT, kmiss, &a);
    add("miss_high", cls::PAST, kHigh, &a);
  }
  // "last_record_pair" must carry the real last value of the last key (kept alive below)
  for (auto &x : v)
    if (x.label == "last_record_pair")
      x.data = t.empty ? a : t.value(t.n() - 1, t.nvals[t.n() - 1] - 1);

  // API-caused errors
  {
    call x;
    x.label = "key_NULL";
    x.c = cls::NULLKEY;
    x.key_null = true;
    x.has_data = (o == kop::LB || o == kop::UB);
    x.data = a;
    v.push_back(x);
  }
  {
    call x;
    x.label = "key_too_long";
    x.c = cls::BADKEY;
    x.key = std::string(maxkey + 1, 'k');
    x.has_data = (o == kop::LB || o == kop::UB);
    x.data = a;
    v.push_back(x);
  }
  if (o == kop::LB || o == kop::UB) {
    call x;
    x.label = "data_NULL";
    x.c = cls::DATA_NULL;
    x.key = midk;
    x.data_null = true;
    v.push_back(x);
  }
  if (o == kop::LB && t.dup) {
    call x;
    x.label = "data_ptr_NULL";
    x.c = cls::DATA_PTR0;
    x.key = t.empty ? midk : k1;
    x.data_ptr0 = true;
    x.has_data = true;
    v.push_back(x);
  }
  return v;
}

// Row ID by the document's table (section 4.3.1). `anchored`/`rightmost` describe the starting state.
std::string row_id(kop o, const call &k, bool table_empty, bool anchored, bool rightmost) {
  const char *p = o == kop::SET ? "SET" : o == kop::SETK ? "SETK" : o == kop::SR ? "SR" : o == kop::LB ? "LB" : "UB";
  auto mk = [&](int n, const char *extra = "") { return std::string(p) + "-" + std::to_string(n) + extra; };
  switch (o) {
  case kop::SET:
  case kop::SETK:
    if (k.c == cls::NULLKEY)
      return mk(5);
    if (k.c == cls::BADKEY)
      return mk(6);
    if (table_empty)
      return mk(4);
    if (k.c == cls::HIT)
      return mk(1);
    return anchored ? mk(3) : mk(2);
  case kop::SR:
    if (k.c == cls::NULLKEY)
      return mk(7);
    if (k.c == cls::BADKEY)
      return mk(8);
    if (table_empty)
      return mk(6);
    if (k.c == cls::HIT)
      return mk(1);
    if (k.c == cls::GT)
      return mk(2);
    return !anchored ? mk(3) : rightmost ? mk(4) : mk(5);
  case kop::LB:
    if (k.c == cls::NULLKEY)
      return mk(8);
    if (k.c == cls::DATA_NULL)
      return mk(9);
    if (k.c == cls::DATA_PTR0)
      return mk(10);
    if (k.c == cls::BADKEY)
      return mk(11);
    if (table_empty)
      return mk(7);
    if (k.c == cls::HIT)
      return mk(1);
    if (k.c == cls::GT)
      return mk(2);
    if (k.c == cls::GTV)
      return mk(3);
    if (k.c == cls::PASTV)
      return mk(6);
    return !anchored ? mk(4) : rightmost ? mk(5) : mk(5, "(other leaf)");
  default:
    if (k.c == cls::NULLKEY || k.c == cls::DATA_NULL)
      return mk(7);
    if (k.c == cls::BADKEY)
      return mk(8);
    if (table_empty)
      return mk(6);
    if (k.c == cls::GT || k.c == cls::GTV)
      return mk(1);
    if (k.c == cls::HIT)
      return mk(2);
    if (k.c == cls::LASTHIT)
      return mk(3);
    if (k.c == cls::PASTV)
      return mk(3, "(last key, value above all)");
    return !anchored ? mk(4) : rightmost ? mk(5) : mk(5, "(other leaf)");
  }
}

// ---------------------------------------------------------------------------------------------
// The runner

struct ctx {
  MDBX_env *env = nullptr;
  size_t maxkey_plain = 0, maxkey_dup = 0;
  int setup_failures = 0;
  int scenarios = 0;
};

void print_start(ctx &x, const table &t, const start &s) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(x.env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return;
  MDBX_cursor *c = nullptr;
  if (mdbx_cursor_open(txn, t.dbi, &c) == MDBX_SUCCESS) {
    const bool ok = s.run(txn, c, t);
    const probes p = probe(c);
    printf("CM|start tbl=%s depth=%u start=%s intended=%s setup_ok=%d state=%s %s\n", t.name, t.depth, s.name,
           s.intended, ok, classify(p, t.dup).c_str(), raw(p).c_str());
    mdbx_cursor_close(c);
  }
  mdbx_txn_abort(txn);
}

void run_scenario(ctx &x, const table &t, const start &s, kop o, const call &k) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(x.env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS) {
    ++x.setup_failures;
    return;
  }
  MDBX_cursor *c = nullptr;
  bool ok = mdbx_cursor_open(txn, t.dbi, &c) == MDBX_SUCCESS;
  std::string s0 = "-";
  if (ok) {
    ok = s.run(txn, c, t);
    if (!ok) {
      ++x.setup_failures;
      printf("CM|SETUP-FAIL tbl=%s start=%s\n", t.name, s.name);
    }
    const probes p0 = probe(c);
    s0 = classify(p0, t.dup);
  }
  if (ok) {
    MDBX_val key = val_of(k.key), data = {nullptr, 0};
    std::string dstr = k.data;
    if (k.has_data)
      data = val_of(dstr);
    if (k.data_ptr0) {
      data.iov_base = nullptr;
      data.iov_len = 0;
    }
    MDBX_val *kp = k.key_null ? nullptr : &key;
    MDBX_val *dp = k.data_null ? nullptr : &data;
    // The start recipe may have deleted the searched key or value: reclassify such a "hit" as a miss.
    call kc = k;
    if (kc.c == cls::HIT || kc.c == cls::LASTHIT) {
      bool exists;
      if (t.dup && kc.has_data && !kc.data.empty()) {
        MDBX_cursor *tmp = nullptr;
        exists = mdbx_cursor_open(txn, t.dbi, &tmp) == MDBX_SUCCESS;
        if (exists) {
          MDBX_val k2 = val_of(kc.key), d2 = val_of(kc.data);
          exists = mdbx_cursor_get(tmp, &k2, &d2, MDBX_GET_BOTH) == MDBX_SUCCESS;
          mdbx_cursor_close(tmp);
        }
      } else {
        MDBX_val k2 = val_of(kc.key), d2 = {nullptr, 0};
        exists = mdbx_get(txn, t.dbi, &k2, &d2) == MDBX_SUCCESS;
      }
      if (!exists) {
        MDBX_val k2 = val_of(kc.key), d2 = {nullptr, 0};
        kc.c = mdbx_get_equal_or_great(txn, t.dbi, &k2, &d2) == MDBX_NOTFOUND ? cls::PAST : cls::GT;
      }
    } else if ((kc.c == cls::GT || kc.c == cls::PAST) && !(t.dup && kc.has_data)) {
      // the table may be too small for "a key between": decide GT or PAST from the data
      MDBX_val k2 = val_of(kc.key), d2 = {nullptr, 0};
      kc.c = mdbx_get_equal_or_great(txn, t.dbi, &k2, &d2) == MDBX_NOTFOUND ? cls::PAST : cls::GT;
    }
    const int rc = mdbx_cursor_get(c, kp, dp, cop(o));
    std::string ret;
    if (rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) {
      ret = "{";
      if (kp)
        ret += show(*kp);
      ret += "/";
      if (dp)
        ret += show(*dp);
      ret += "}";
    }
    const probes p1 = probe(c);
    const bool dup = t.dup;
    const bool empty_now = t.empty || std::string(s.name) == "D0";
    const bool rightmost = s.rightmost || t.depth <= 1;
    printf(
        "CM|row=%s|tbl=%s|depth=%u|start=%s|intended=%s|start_state=%s|op=%s|case=%s|key=%s|rc=%s|ret=%s|%s|state=%s\n",
        row_id(o, kc, empty_now, s.anchored, rightmost).c_str(), t.name, t.depth, s.name, s.intended, s0.c_str(),
        opname(o), k.label.c_str(), k.key_null ? "NULL" : k.key.substr(0, 12).c_str(), rcname(rc).c_str(), ret.c_str(),
        raw(p1).c_str(), classify(p1, dup).c_str());
    ++x.scenarios;
  }
  if (c)
    mdbx_cursor_close(c);
  mdbx_txn_abort(txn);
}

} // namespace

TEST(ut_cursor_contract_measure, set_family) {
  const env_files_guard guard({kDb});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  ctx x;
  ASSERT_EQ(mdbx_env_create(&x.env), MDBX_SUCCESS);
  env_handle_guard env_guard(&x.env);
  ASSERT_EQ(mdbx_env_set_maxdbs(x.env, 16), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(x.env, -1, -1, 64 << 20, -1, -1, -1), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(x.env, kDb, MDBX_NOSUBDIR | MDBX_UTTERLY_NOSYNC, 0644), MDBX_SUCCESS);

  MDBX_stat st;
  ASSERT_EQ(mdbx_env_stat_ex(x.env, nullptr, &st, sizeof(st)), MDBX_SUCCESS);
  const size_t psize = st.ms_psize;
  x.maxkey_plain = size_t(mdbx_env_get_maxkeysize_ex(x.env, MDBX_DB_DEFAULTS));
  x.maxkey_dup = size_t(mdbx_env_get_maxkeysize_ex(x.env, MDBX_DUPSORT));

  // Tables. Values are sized from the page size so that the "multi" tables get several leaf pages.
  std::vector<table> tabs;
  tabs.push_back(make_table("plain_small", false, 5, 4));
  tabs.push_back(make_table("plain_multi", false, 60, psize / 6));
  tabs.push_back(make_table("dup_small", true, 5, 4));
  tabs.push_back(make_table("dup_multi", true, 60, psize / 16));
  tabs.push_back(make_table("plain_one", false, 1, 4));
  tabs.push_back(make_table("dup_one", true, 1, 4));
  tabs.push_back(make_table("plain_empty", false, 0, 4));
  tabs.push_back(make_table("dup_empty", true, 0, 4));
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(mdbx_txn_begin(x.env, nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    for (auto &t : tabs) {
      ASSERT_EQ(mdbx_dbi_open(txn, t.name, t.dup ? (MDBX_CREATE | MDBX_DUPSORT) : MDBX_CREATE, &t.dbi), MDBX_SUCCESS);
      fill(txn, t);
      MDBX_stat ds;
      ASSERT_EQ(mdbx_dbi_stat(txn, t.dbi, &ds, sizeof(ds)), MDBX_SUCCESS);
      t.depth = ds.ms_depth;
      if (t.n() > 5) {
        ASSERT_GE(t.depth, 2u) << t.name << ": the tree must have more than one leaf page";
      }
    }
    ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  }
  printf("CM|env page_size=%zu maxkey_plain=%zu maxkey_dup=%zu\n", psize, x.maxkey_plain, x.maxkey_dup);
  for (const auto &t : tabs)
    printf("CM|table %s dup=%d keys=%zu depth=%u\n", t.name, t.dup, t.n(), t.depth);

  // C-1: every call on an unbound cursor
  {
    MDBX_cursor *u = mdbx_cursor_create(nullptr);
    ASSERT_NE(u, nullptr);
    for (kop o : {kop::SET, kop::SETK, kop::SR, kop::LB, kop::UB}) {
      MDBX_val k = val_of(tabs[0].keys[1]), d = {nullptr, 0};
      const int rc = mdbx_cursor_get(u, &k, &d, cop(o));
      printf("CM|row=C-1|tbl=unbound|start=U|op=%s|rc=%s\n", opname(o), rcname(rc).c_str());
    }
    mdbx_cursor_close(u);
  }

  const kop ops[] = {kop::SET, kop::SETK, kop::SR, kop::LB, kop::UB};
  for (const auto &t : tabs) {
    const size_t maxkey = t.dup ? x.maxkey_dup : x.maxkey_plain;
    for (const auto &s : kStarts) {
      if (s.for_dup_only && !t.dup)
        continue;
      if (s.for_nonempty_only && t.empty)
        continue;
      const bool one = t.n() == 1;
      if (s.one_only && !one)
        continue;
      if (one && !s.one_only && std::string(s.name) != "N" && std::string(s.name) != "Rlast")
        continue; // the single-record tables are only for N, R on the only record, and D0
      if (!one && !t.empty && s.one_only)
        continue;
      if (std::string(s.name) == "Rlast" && t.empty)
        continue;
      print_start(x, t, s);
      for (kop o : ops)
        for (const auto &k : make_calls(o, t, maxkey))
          run_scenario(x, t, s, o, k);
    }
  }
  printf("CM|done scenarios=%d setup_failures=%d\n", x.scenarios, x.setup_failures);
  fflush(stdout);
  EXPECT_EQ(x.setup_failures, 0);
}
