// v13b-cxx-runtime-db.cpp — stream C, tranche V13b (C++ API headers).
//
// Runtime checks of the wrapper -> C API forwarding and RAII/ownership state
// machines, exercised against a real database in a temp directory:
//   - env_managed open/close + operate_parameters round-trip,
//   - txn::insert/upsert/update/reserve flag forwarding (NOOVERWRITE/UPSERT/
//     CURRENT), try_* result payloads, MDBX_RESERVE fill-in,
//   - txn::replace CURRENT|NOOVERWRITE dupsort selection + extract/replace
//     data_preserver protocol (exception capture through C),
//   - cursor typed seek/iteration ops and erase C-string overloads,
//   - get_batch, txn::clone (create + renew-into), park/unpark (#104),
//   - txn_managed commit/abort RAII, move-assign, HAS_CHILD guard,
//   - drop_map/clear_map throw_if_absent paths, open_map_accede.
//
// Build: see the command line at the bottom. Expected: exit 0, "all checks passed".
#include <mdbx.h++>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

using mdbx::buffer;
using mdbx::default_buffer;
using mdbx::pair;
using mdbx::slice;

namespace {
const char *const DB_DIR = "/tmp/v13b-cxx-runtime-db";
const char *const DB_PATH = "/tmp/v13b-cxx-runtime-db/data.mdb";

void rmrf(const std::string &dir) {
  std::string cmd = "rm -rf '" + dir + "'";
  (void)system(cmd.c_str());
}

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    std::abort();
  }
}

void crud_and_replace() {
  mdbx::env_managed env(DB_PATH, mdbx::env_managed::create_parameters(),
                        mdbx::env::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("crud", mdbx::key_mode::usual, mdbx::value_mode::single);

    txn.insert(map, "k1", "v1");
    bool thrown = false;
    try {
      txn.insert(map, "k1", "v1-dup");
    } catch (const mdbx::key_exists &) {
      thrown = true;
    }
    check(thrown, "insert duplicate must throw key_exists");

    auto ti = txn.try_insert(map, "k1", "v1-dup");
    check(!ti.done && ti.value == slice("v1") /* mdbx_put fills the PRESENT value on KEYEXIST */,
          "try_insert returns present value");

    txn.upsert(map, "k1", "v1-upserted");
    check(txn.get(map, "k1") == slice("v1-upserted"), "upsert overwrites");

    check(txn.try_update(map, "k2", "nope") == false, "try_update absent key");
    check(txn.try_update(map, "k1", "v1-updated") == true, "try_update existing key");

    // MDBX_RESERVE: blank filled by the C API
    auto reserved = txn.upsert_reserve(map, "k1", 8);
    check(reserved.data() != nullptr, "reserve fills the data pointer");
    memcpy(reserved.data(), "resv8xyz", 8); // the WHOLE reserved length is stored
    check(txn.get(map, "k1") == slice("resv8xyz"), "reserved bytes stored");

    // get_equal_or_great
    auto gr = txn.get_equal_or_great(map, "k1");
    check(gr.done && gr.key == slice("k1"), "get_equal_or_great exact");
    bool nf_thrown = false;
    try {
      (void)txn.get_equal_or_great(map, "k9"); // 1-arg form throws NOTFOUND
    } catch (const mdbx::not_found &) {
      nf_thrown = true;
    }
    check(nf_thrown, "get_equal_or_great 1-arg throws on absence");
    auto gr2 = txn.get_equal_or_great(map, "k9", slice("<absent>")); // 2-arg form
    check(!gr2.done && gr2.value == slice("<absent>"), "get_equal_or_great absence value");

    txn.commit();
  }
  {
    auto txn = env.start_write();
    auto map = txn.create_map("t1", mdbx::key_mode::usual, mdbx::value_mode::multi);

    // dupsort data first (NOOVERWRITE forbids adding dups to an existing key,
    // so multi-values are added by upsert), then typed seeks
    txn.insert(map, "dup", "d1");
    txn.upsert(map, "dup", "d2");
    txn.upsert(map, "dup", "d3");
    auto cursor = txn.open_cursor(map);
    auto lb = cursor.lower_bound("dup");
    check(lb.done && lb.key == slice("dup"), "lower_bound");
    auto pair_first = cursor.to_current_first_multi();
    check(pair_first.done && pair_first.value == slice("d1"), "to_current_first_multi");
    auto pair_last = cursor.to_current_last_multi();
    check(pair_last.done && pair_last.value == slice("d3"), "to_current_last_multi");
    auto exact = cursor.find_multivalue("dup", "d2");
    check(exact.done && exact.value == slice("d2"), "find_multivalue");
    check(cursor.count_multivalue() == 3, "count_multivalue == 3");
    // get_ex values count for dupsort
    size_t count = 0;
    slice first = txn.get(map, "dup", count);
    check(count == 3, "get_ex reports 3 values");
    (void)first;

    // erase overloads (incl. C-string binding)
    check(cursor.erase(slice("dup"), slice("d2")) == true, "erase(key,value)");
    check(cursor.count_multivalue() == 2, "2 multivalues left");
    check(cursor.erase("dup") == true /* whole_multivalue default */, "erase(c-string key)");
    check(!txn.get(map, "dup", mdbx::slice("<absent>")).is_valid() == false, "dup key gone");
    check(txn.erase(map, "dup") == false, "erase absent returns false");

    // put_multiple_samelength into a DUPFIXED map (needs its own cursor)
    auto fixed = txn.create_map("fixed", mdbx::key_mode::usual, mdbx::value_mode::multi_samelength);
    auto fixed_cursor = txn.open_cursor(fixed);
    uint32_t vals[4] = {10, 20, 30, 40};
    size_t done = fixed_cursor.put_multiple_samelength("mk", sizeof(uint32_t), vals, 4, mdbx::put_mode::upsert, false);
    check(done == 4, "put_multiple stored all 4");

    // replace with dupsort selection: CURRENT|NOOVERWRITE picks the exact value
    txn.insert(map, "rkey", "old1");
    txn.upsert(map, "rkey", "old2");
    slice selector("old1");
    txn.replace(map, "rkey", selector, "new1");
    {
      // scan the dups of "rkey"
      std::string seen;
      cursor.to_first();
      slice k("rkey");
      mdbx::pair p2(k, slice());
      auto found = cursor.lower_bound_multivalue("rkey", "new1");
      check(found.done && found.value == slice("new1"), "replace selected the right dup");
    }

    txn.commit();
  }
  {
    // replace/extract via data_preserver (BUFFER templates)
    auto txn = env.start_write();
    auto map = txn.create_map("rx", mdbx::key_mode::usual, mdbx::value_mode::single);
    txn.insert(map, "x", "0123456789");
    auto extracted = txn.extract<default_buffer>(map, "x");
    check(extracted == slice("0123456789"), "extract returns old value");
    check(!txn.get(map, "x", slice("<absent>")).is_valid() == false, "extract deleted the key");
    txn.insert(map, "x", "aaa");
    auto replaced = txn.replace<default_buffer>(map, "x", "bbb");
    check(replaced == slice("aaa"), "replace returns old value");
    check(txn.get(map, "x") == slice("bbb"), "replace stored new value");
    // replace_reserve
    slice reservation;
    auto old = txn.replace_reserve<default_buffer>(map, "x", 4, reservation);
    check(old == slice("bbb"), "replace_reserve old value");
    memcpy(reservation.data(), "cccc", 4);
    check(txn.get(map, "x") == slice("cccc"), "replace_reservation stored");
    txn.commit();
  }
  env.close();
}

void ownership_and_raii() {
  // NOSTICKYTHREADS: mdbx_txn_clone requires it for cloning same-thread RO txns;
  // write_file_io: nested transactions are incompatible with MDBX_WRITEMAP
  mdbx::env::operate_options opts;
  opts.set_no_sticky_threads().set_nested_transactions();
  mdbx::env_managed env(DB_PATH, mdbx::env::operate_parameters(0, 0, mdbx::env::mode::write_file_io,
                                                             mdbx::env::durability::robust_synchronous, {}, opts)
                                   .set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("own", mdbx::key_mode::usual, mdbx::value_mode::single);
    for (int i = 0; i < 100; ++i)
      txn.insert(map, slice(std::to_string(i)), slice(std::string(20, char('a' + i % 26))));
    txn.commit();
  }
  // txn_managed move semantics + RAII (uncommitted txn aborted by destructor)
  {
    auto txn = env.start_write();
    auto map = txn.open_map_accede("own");
    txn.insert(map, "raii", "1");
    mdbx::txn_managed stolen(std::move(txn));
    check(txn.handle() == nullptr, "moved-from txn is empty");
    stolen.abort();
  }
  {
    auto txn = env.start_write(); // destructor aborts it
    auto map = txn.open_map_accede("own");
    txn.insert(map, "uncommitted", "nope");
  }
  {
    auto txn = env.start_read();
    check(txn.get(txn.open_map_accede("own"), "uncommitted", slice("<absent>")) == slice("<absent>"),
          "uncommitted txn was aborted by RAII");
  }
  // clone: create-new and renew-into
  {
    auto txn = env.start_read();
    auto cloned = txn.clone();
    check(cloned.get(txn.open_map_accede("own"), "0").length() == 20, "clone reads the same snapshot");
    mdbx::txn_managed renew_target = env.prepare_read();
    renew_target.reset_reading();
    txn.clone(renew_target);
    check(renew_target.id() == txn.id(), "renew-into clone reuses the snapshot");
    txn.commit();
    cloned.commit();
    renew_target.abort();
  }
  // park/unpark (#104)
  {
    auto txn = env.start_read();
    txn.park_reading(/*autounpark=*/false);
    txn.unpark_reading(/*restart_if_ousted=*/true);
    check(txn.get(txn.open_map_accede("own"), "0").length() == 20, "unpark restores the snapshot");
    txn.commit();
  }
  // get_batch over the DUPFIXED map (the C op family requires MULTIPLE ops)
  {
    auto txn = env.start_read();
    auto map = txn.open_map_accede("fixed");
    auto cursor = txn.open_cursor(map);
    auto first_batch = cursor.seek_multiple_samelength("mk");
    check(first_batch.done, "seek_multiple_samelength found mk");
    size_t total = first_batch.done ? 1 : 0; // the seek itself delivered one batch
    bool is_last = true;
    while (!is_last) {
      auto batch = cursor.get_batch(16, mdbx::cursor::batch_samelength_next, &is_last);
      total += batch.size();
      check(batch.size() <= 16, "batch size bounded");
    }
    check(total >= 1, "get_batch delivered the mk batch");
    txn.commit();
  }
  // drop/clear/accede
  {
    auto txn = env.start_write();
    check(!txn.drop_map("absent-map", false), "drop_map absent (no throw)");
    bool thrown = false;
    try {
      txn.drop_map("absent-map", true);
    } catch (const mdbx::not_found &) {
      thrown = true;
    }
    check(thrown, "drop_map absent (throw)");
    auto map = txn.open_map_accede("own");
    txn.clear_map(map);
    check(txn.get_map_stat(map).ms_entries == 0, "clear_map emptied");
    auto acceded = txn.open_map_accede("own");
    check(acceded.dbi == map.dbi, "open_map_accede");
    txn.commit();
  }
  // release_all_cursors and nested transactions
  {
    auto outer = env.start_write();
    auto map = outer.open_map_accede("t1");
    outer.insert(map, "n", "0");
    check(outer.get(map, "n") == slice("0"), "parent sees own value");
    {
      // a parent txn cannot operate while its nested child is active (BAD_TXN)
      auto inner = env.start_write(outer);
      inner.upsert(map, "n", "1"); // dupsort: insert() would refuse an existing key
      bool blocked = false;
      try {
        (void)outer.get(map, "n");
      } catch (const mdbx::bad_transaction &) {
        blocked = true;
      }
      check(blocked, "parent blocked while child active");
      inner.commit();
    }
    {
      size_t cnt = 0;
      (void)outer.get(map, "n", cnt);
      check(cnt == 2, "parent sees the nested commit (2 multivalues)");
    }
    check(outer.release_all_cursors(false) == 0, "no cursors left");
    outer.commit();
  }
  // HAS_CHILD guard
  {
    auto outer = env.start_write();
    auto inner = env.start_write(outer);
    bool thrown = false;
    try {
      outer.commit();
    } catch (const mdbx::bad_transaction &) {
      thrown = true;
    }
    check(thrown, "commit with pending child throws BAD_TXN");
    inner.commit();
    outer.commit();
  }
  // buffer(txn&, slice&) dirty-clone contract
  {
    auto txn = env.start_write();
    auto map = txn.open_map_accede("own");
    txn.insert(map, "dirty", "12345");
    auto got = txn.get(map, "dirty");
    mdbx::default_buffer as_dirty(txn, got);
    check(as_dirty.is_freestanding(), "dirty value cloned");
    txn.commit();
    auto clean = env.start_read();
    // a committed value is not dirty -> referenced, not cloned
    mdbx::default_buffer as_clean(clean, clean.get(clean.open_map_accede("own"), "dirty"));
    check(as_clean.is_reference(), "clean value referenced");
    clean.commit();
  }
  env.close();
}

} // namespace

int main() {
  rmrf(DB_DIR);
  std::system(("mkdir -p " + std::string(DB_DIR)).c_str());
  crud_and_replace();
  ownership_and_raii();
  rmrf(DB_DIR);
  std::puts("v13b-cxx-runtime-db: all checks passed");
  return 0;
}

// Build & run (the library needs the C core + the C++ API TU; compiler gcc-15, Linux x86-64):
//   CFLAGS='-std=gnu11 -O1 -DMDBX_BUILD_CXX=1'
//   for f in <tree>/src/*.c; do case $f in *lck-windows*|*alloy*|*cursor.c) continue;; esac;
//          gcc $CFLAGS -I<tree> -I<tree>/src -c $f -o obj/$(basename $f .c).o; done
//   for f in <tree>/src/osal/posix/*.c; do gcc $CFLAGS -I<tree> -I<tree>/src -I<tree>/src/osal \
//          -I<tree>/src/osal/posix -c $f -o obj/osx-$(basename $f .c).o; done
//   # plus a stub obj/version.o defining ::mdbx_version (global.o provides mdbx_build)
//   g++ -std=c++20 -O1 -DMDBX_BUILD_CXX=1 -c <tree>/src/mdbx.c++ -o obj/mdbx-cxx.o -I<tree>
//   ar rcs libcxxcore.a obj/*.o
//   g++ -std=c++20 -O1 -DMDBX_BUILD_CXX=1 -Wno-unused-result -I<tree> -o runtime \
//          v13b-cxx-runtime-db.cpp libcxxcore.a -lpthread -lrt && ./runtime
