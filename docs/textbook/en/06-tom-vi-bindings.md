# Volume VI. Bindings

> **Level:** for developers working in languages other than C/C++.
> **Volume goal:** you know how to use libmdbx from your language and understand the specifics and
> limitations of each binding.
> **Honesty:** facts about specific wrappers (repository, version, bugs) require analysis of their
> repositories and issues. Architectural consequences (NoStickyThreads, Send/Sync,
> GC/pinning, GIL) are given here; positions that need to be checked against the repository are
> marked explicitly.

---

## Chapter 34. Overview of the bindings ecosystem

Officially tracked bindings: **Rust, Go, Node.js, Zig, Python, .NET (C#),
C++, Dart, Nim, Java, Haskell, Ruby, Scala**. Plus unofficial ones (openresty/lua and others).

| Binding                | Language       | Status                                | Details  |
| ---------------------- | -------------- | ---------------------------------- | --------- |
| mdbx.h++               | C++            | Mature (official C++ API)          | Ch. 40   |
| mdbx-rs / mdbx-sys     | Rust           | Mature/active                      | Ch. 35   |
| mdbx-go                | Go             | Active                             | Ch. 36   |
| python-lmdbx / mdbx-py | Python         | Active                             | Ch. 37   |
| node-mdbx              | Node.js        | Active                             | Ch. 38   |
| mdbx-zig (lmdbx-zig)   | Zig            | Officially tracked                 | §34.2    |
| libmdbx-dotnet         | .NET / C#      | Active                             | Ch. 39   |
| mdbx-dart / Isar       | Dart (Flutter) | Active (Isar uses libmdbx)         | Ch. 41   |
| Nim                    | Nim            | —                                  | Ch. 42   |
| Java                   | Java           | —                                  | Ch. 43   |
| Haskell                | Haskell        | —                                  | Ch. 44   |
| Ruby                   | Ruby           | —                                  | Ch. 45   |
| Scala                  | Scala          | —                                  | Ch. 46   |

> **Note:** the cards for Ch. 42–46 (Nim, Java, Haskell, Ruby, Scala) are brief; repository details
> require analysis (see the notes in each card).

### 34.2. Zig (brief reference)

`mdbx-zig` (repository `lmdbx-zig`) is an officially tracked binding for Zig.
Known specifics: Zig is actively used for cross-compiling C/C++; `MDBX_HAVE_BUILTIN_CPU_SUPPORTS=0`
may be needed to work around toolchain bugs. No separate card is dedicated to it —
repository and version details require analysis.

### 34.1. What each binding covers

A typical picture: a binding wraps the **C API** (`mdbx_env_*`, `mdbx_txn_*`, `mdbx_dbi_*`,
`mdbx_cursor_*`, `MDBX_val`) into the idiom of the language. Coverage completeness differs: the core
(env/txn/get/put/del/cursor) is covered everywhere; advanced APIs (cache_get, defrag, clone,
embark_read, estimate_*) — not always. Check the documentation of the specific binding.

---

## Chapter 35. Rust (mdbx-sys / mdbx-rs)

**Overview.** `mdbx-sys` — low-level FFI bindings to the C API; `mdbx-rs` — a safe wrapper
(idiomatic Rust). Status — active/mature. _(Repository details require verification.)_

**Installation.** Via cargo: `mdbx-sys = "..."`, `mdbx-rs = "..."`.

**Basic example** (simplified, generic for the kind):

```rust
use mdbx::{Env, Environment};

fn main() -> mdbx::Result<()> {
    let env = unsafe { Env::builder().open("db.mdbx")? };
    let txn = env.begin_rw_txn()?;
    txn.put(b"greeting", b"hello, libmdbx")?;
    txn.commit()?;
    Ok(())
}
```

_(The exact syntax depends on the binding version — check its README.)_

**Critical nuances (from the libmdbx architecture):**

- **Send/Sync.** A transaction and a cursor are bound to a thread (sticky). A safe wrapper must
  reflect this in its types: a transaction is `!Sync` (use from two threads = `THREAD_MISMATCH`).
  For async runtimes (tokio and others), `MDBX_NOSTICKYTHREADS` is mandatory, since a future may
  continue on another OS thread.
- **Use-after-free after commit/abort.** `MDBX_val` points into mmap; after the transaction ends
  the pointers are invalid. The wrapper must restrict the lifetime of values to the transaction's
  lifetime rather than handing `&[u8]` out.
- **Blocking calls in an async context.** Commit and writes block the thread; in an async runtime —
  only via `spawn_blocking`, otherwise the whole worker is blocked.

**Performance.** The binding's overhead is minimal (FFI + ownership checks); reads remain close to C.
_(Check the binding's benchmarks.)_

**Gaps.** Version-dependent: cache_get, defrag, clone and others may be missing.

---

## Chapter 36. Go (mdbx-go)

**Overview.** `mdbx-go` — Go binding via CGO. Status — active.

**Installation.** `go get github.com/.../mdbx-go`.

**Basic example:**

```go
import "github.com/.../mdbx-go"

env, _ := mdbx.EnvCreate()
env.Open("db.mdbx", mdbx.CREATE|mdbx.NOSUBDIR, 0664)
txn, _ := env.BeginTxn(nil, 0)
dbi, _ := txn.DBIOpen(nil, 0)
txn.Put(dbi, []byte("k"), []byte("v"), 0)
txn.Commit()
```

_(Verify the signatures against the repository.)_

**Critical nuances (from the architecture):**

- **NoStickyThreads is always set.** A Go goroutine can migrate between OS threads, so the binding
  opens the environment with `MDBX_NOSTICKYTHREADS`.
- **One transaction object cannot be used from two goroutines simultaneously** — that is a race.
- **Deadlock risk:** functions requiring the environment write lock (`Env.SetOption`, `Env.Sync`,
  `Env.Stat`, `Env.Defrag`, `Env.Close`) can deadlock while an active write transaction runs in
  another goroutine. Pattern: one transaction — one goroutine, or explicit serialization via a
  channel/mutex.
- **`runtime.LockOSThread`** is needed if you want to pin a goroutine to an OS thread (reduces risks
  when working with the reader TLS slot).
- **CGO overhead** — CGO boundary on every operation; for hot paths group operations into batches.

**Gaps.** Completeness of the advanced API — check the repository.

---

## Chapter 37. Python (python-lmdbx / mdbx-py)

**Overview.** Python binding over the C API. Status — active _(verification)_.

**Critical nuances (from the architecture):**

- **GIL and blocking calls.** Long read transactions and bulk writes hold the GIL → other Python
  threads are blocked. Release the GIL in the binding during long operations.
- **Pointers into mmap are valid as long as the transaction is alive.** Python GC does not manage
  this — an explicit context manager (`with env.begin(...)`) is needed to guarantee commit/abort in
  time.
- **fork() after import.** With `multiprocessing`, call `resurrect_after_fork` in the child
  process.
- **ctypes vs CFFI vs pybind.** The choice affects overhead; for batches minimize the number of
  boundary calls.

**Gaps.** Verify against the repository (Python 3.x versions, wheels, advanced API).

---

## Chapter 38. Node.js (node-mdbx)

**Overview.** Node.js binding (N-API). Status — active _(verification)_.

**Critical nuances (from the architecture):**

- **Event loop and blocking calls.** The synchronous C API blocks the event loop. The wrapper should
  offer async variants (worker threads / libuv) for large operations.
- **Buffer ↔ MDBX_val.** A Node.js Buffer is an owning wrapper; when passing it to libmdbx watch the
  lifetime (do not pass a buffer that may be freed).
- **NoStickyThreads** — same as Go, if worker threads are used.

**Gaps.** Verify against the repository.

---

## Chapter 39. .NET / C# (libmdbx-dotnet)

**Overview.** .NET binding over the C API. An alternative in the ecosystem is LightningDB (not
libmdbx! — check exactly what you are using). Status — active _(verification)_.

**Critical nuances (from the architecture):**

- **Pinning.** `MDBX_val` points into mmap; if the value is a managed buffer, the .NET GC may move
  it. Solution: `fixed`/pinned, or a marshal copy, or `Span<T>`.
- **SafeHandle for env/txn/cursor.** Correct release via SafeHandle/finalizer.
- **GC finalizer and ordering.** Close in the correct order: env → txn → cursor; dangling handles
  give `BAD_DBI`/`BAD_TXN`.
- **Unpredictable lifetime** due to GC — do not hold long transactions between collections.

**Gaps.** Verify against the repository (versions, platforms, advanced API).

---

## Chapter 40. C++ (mdbx.h++)

**Overview.** The official C++ API (`mdbx.h++`), canon v0.15.0-263. Mature.

**Class hierarchy:**

- `env` / `env_managed` — environment; `txn` / `txn_managed` — transactions; `cursor` /
  `cursor_managed` — cursors; `map_handle` — table; `slice` / `buffer` — keys/values with
  ownership.
- The managed variants (`*_managed`) are RAII: automatic closing on scope exit.

**Typed operations.** Map-over-keys/values via type translation; parameter structs with
fluent setters (geometry, mode, durability, reclaiming).

**Exceptions.** Hierarchy `mdbx::error` → `mdbx::exception` (from `std::runtime_error`) →
`mdbx::fatal`; 35 typed exceptions (`bad_map_id`, `db_corrupted`, `db_full`,
`key_exists`, `not_found`, `transaction_ousted` and others). Fatal errors lead to termination.

**Fragment from [`examples/c++/47-cpp-api.c++`](examples/c++/47-cpp-api.c++)** — typed map operations (`insert`/`upsert` via `map_handle`) and typed exceptions (`mdbx::key_exists` and others):

```cpp
    {
      auto txn = env.start_write(); // RAII: will close/roll back itself on exception
      auto ordinal = txn.create_map("ordinal", mdbx::key_mode::ordinal, mdbx::value_mode::single);
      auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

      txn.insert(ordinal, buffer::key_from_u64(42), "answer");
      txn.upsert(multi, mdbx::slice("tag"), mdbx::slice("x"));
      txn.upsert(multi, mdbx::slice("tag"), mdbx::slice("y"));

      // Typed exceptions by error code.
      try {
        txn.insert(ordinal, buffer::key_from_u64(42), "again");
        std::cerr << "FAIL: duplicate insert did not throw\n";
        return EXIT_FAILURE;
      } catch (const mdbx::key_exists &) {
        std::cout << "duplicate -> mdbx::key_exists as expected\n";
      }

      txn.commit();
    }
```

Full code: [47-cpp-api.c++](examples/c++/47-cpp-api.c++).

**C++20.** Concepts, allocators (`buffer<>` with ownership policies), `inplace_storage_size_rounding`.

**Example:**

```cpp
#include <mdbx.h++>

int main() {
    auto env = mdbx::env_managed::create("db.mdbx",
                 /* geometry */ {}, 1, mdbx::env_operate_param{});
    auto txn = env.start_write();
    auto db = txn.open_map("kv", mdbx::key_mode::usual,
                           mdbx::value_mode::single);
    db.put(txn, "greeting", "hello, libmdbx");
    txn.commit();
}
```

_Check the exact syntax against your version of mdbx.h++._

**Critical nuances:**

- The `upper` value for a new database is chosen by the engine itself (~the golden section of RAM,
  bounded by the mmap limit ≈140 TB on 64-bit); `TOO_LARGE`/`ENOMEM` are possible with an explicitly
  excessive upper (e.g., under ASAN/Valgrind). Always specify the geometry explicitly.
- Exceptions vs return codes: pick a single style; `mdbx::error` is caught by type.

**Gaps.** `key_mode::msgpack` is declared but not implemented.

---


> **Examples for the chapter:** [`examples/c++/47-cpp-api.c++`](examples/c++/47-cpp-api.c++).

## Chapter 41. Dart (mdbx-dart / Isar)

**Overview.** `mdbx-dart` — a binding for Dart/Flutter; **Isar** — a popular local Flutter database
using libmdbx. Status — active.

**Critical nuances (from the architecture):**

- **Isolate specifics.** Dart isolates are separate threads; transactions do not cross isolates
  (sticky thread). Each isolate has its own transactions.
- **Mobile platforms.** An embedded database on Android/iOS: be careful with parking long reads and
  with low WAF (extending flash lifetime).

**Gaps.** Verify against the repository.

---

## Chapter 42. Nim

**Overview.** Nim binding. _(Repository and version data require analysis of the binding's repository.)_

**Expected nuances (from the architecture):** memory management (Nim GC does not own mmap pointers —
manual lifetime restrictions are needed); thread affinity (sticky).

---

## Chapter 43. Java

**Overview.** Java binding (JNI). _(Data requires repository analysis.)_

**Expected nuances (from the architecture):** pinning critical buffers (JNI `GetByteArrayElements`
or direct ByteBuffer); releasing native handles (env/txn/cursor) in the correct order;
multithreading (each thread has its own transactions).

---

## Chapter 44. Haskell

**Overview.** Haskell binding (FFI). _(Data requires repository analysis.)_

**Expected nuances (from the architecture):** purity and effects (STM vs IO); binding transactions to
OS threads; the lifetime of pointers into mmap.

---

## Chapter 45. Ruby

**Overview.** Ruby binding. _(Data requires repository analysis.)_

**Expected nuances (from the architecture):** GVL (analogous to GIL) and blocking calls; releasing
native handles via finalizer/ensure.

---

## Chapter 46. Scala

**Overview.** Scala binding (via JVM). _(Data requires repository analysis.)_

**Expected nuances (from the architecture):** the same as Java (JNI, pinning, handles), plus Scala
idiom specifics (futures — be careful with threads).

---

## Volume summary

- The binding core (env/txn/CRUD/cursor) is covered everywhere; the advanced API — not always.
- Critical architectural consequences repeat themselves: NoStickyThreads (Go/async), Send/Sync and
  use-after-free (Rust), pinning/SafeHandle (Java/.NET), GIL (Python/Ruby), the lifetime of pointers
  into mmap (all).
- The Nim/Java/Haskell/Ruby/Scala cards require repository analysis — marked explicitly.

---

## Textbook conclusion (volumes I–VI)

You have walked the path from "what a key-value database is" to "why the GC cycle degrades with long
readers" and "how to use libmdbx from any language". Refresh the concepts as needed, check your
configurations against the checklists of Volume V, chapter 30, and measure — numbers without context
are meaningless.