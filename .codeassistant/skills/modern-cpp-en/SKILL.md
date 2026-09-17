---
name: modern-cpp-en
description: >
  Modern C++ (C++20/23/26) engineering guide. Use when writing, reviewing, or
  refactoring C++ code. Covers: error handling, API design, containers, ranges,
  compile-time, attributes, concurrency, architecture, naming, design patterns,
  build acceleration, clang-tidy, and common pitfalls.
  Consult this guide whenever writing C++ code, including the libmdbx C++ API layer.
license: MIT
---

# Modern C++ Engineering Guide

Adapted from [`huxint/cpp-standing-skill`](https://github.com/huxint/cpp-standing-skill) (MIT);
see [LICENSE](LICENSE) and the per-topic references in [`references/`](references/).

Principles: **Confine complexity to boundaries; keep internals simple.** Priority: readable > correct > performant > clever.

**Cross-platform first**: Prefer standard C++ and cross-platform libraries over platform-specific APIs (`Win32`, POSIX-only, etc.) unless the user explicitly requests otherwise.

**Correct → concise + efficient**: Correctness is non-negotiable. Beyond that, pursue conciseness and efficiency equally — neither is sacrificed for the other. Prefer newer standard facilities over legacy — they are typically safer, cleaner, and faster.

**Complexity is a signal**: When implementation grows complex (deep nesting, many conditionals, excessive boilerplate), stop and consider whether a better design pattern or abstraction exists before writing more code. See `references/design-patterns.md`.

**Don't optimize blindly**: The compiler is already smart (inlining, constant folding, loop unrolling, RVO/NRVO) — most "manual optimizations" are redundant or even harmful. Before every optimization, ask three questions: **What** (where is the bottleneck?), **Why** (do you have profiling data?), **How** (can you measure the improvement after the change?). Optimization without profiling data is guessing, not engineering.

> See the "Modern Replacements" table below.

## references/

| File | Contents |
|------|----------|
| `api-design.md` | Parameter passing, Rule of 0/5, deducing this, type aliases, `std::` prefix |
| `architecture.md` | Directory layout, layer diagram, dependency inversion, target splitting |
| `code-review.md` | 25+ review checklist items, clang-tidy config & noise filtering |
| `concurrency.md` | Locks, coroutines, stdexec sender/receiver full examples |
| `design-patterns.md` | 7 GoF → modern replacements with complete code |
| `naming-style.md` | Naming table, explicit comparisons, C++ casts, `std::` prefix |
| `pitfalls.md` | 17 common pitfalls (move semantics, lifetime, init, concurrency, const) |
| `build.md` | PCH/Unity/ccache multi-build-system config, project config templates |
| `tips.md` | Unsigned-safe comparisons, exchange, IILE, erase_if, fold expressions & more |
| `testing.md` | TDD loop, test layers, isolation/determinism, Mock vs Fake, flaky prevention, sanitizers |
| `modules.md` | C++20 module syntax, partitions, build system config, compiler status, migration strategy |
| `package-management.md` | vcpkg / Conan / FetchContent / xmake package management comparison |
| `smart-pointers.md` | Custom deleters, enable_shared_from_this, weak_ptr cache, Type Erasure |
| `memory.md` | PMR, arena allocator, SSO, memory alignment, false sharing |
| `coroutines.md` | promise_type, custom task/generator, co_await mechanism, HALO optimization |
| `debugging.md` | GDB/LLDB commands, sanitizer output reading, perf flame graphs, Valgrind |
| `abi.md` | Pimpl ABI firewall, inline namespace versioning, symbol visibility |
| `serialization.md` | bit_cast, byte order, binary protocols, cross-platform serialization |
| `documentation.md` | Comment principles, Doxygen style, TODO/FIXME conventions |

---

## 1. Error Handling

- **Boundaries**: `std::expected<T, E>` — explicit, composable
- **Internals**: Exceptions allowed — never propagated across boundaries
- **Never mix** bool / error_code / exception / expected within the same layer

```cpp
std::expected<User, Error> find_user(int id) {
    if (id <= 0) return std::unexpected(Error::InvalidId);
    // ...
}

// Chaining (C++23)
find_user(id)
    .transform([](User u) { return u.name; })
    .or_else([](Error e) -> std::expected<std::string, Error> {
        log(e); return std::unexpected(e);
    });
```

---

## 2. API Design

> See `references/api-design.md`

- Small types by value; large types `const&`; sink parameters by value + move
- Interfaces use `span` / `string_view`, not `const vector&` / `const string&`
- Single-argument constructors must be `explicit`; Rule of Zero preferred; Rule of Five moves must be `noexcept`
- `struct` = pure data; `class` = has invariants
- `using` aliases for readable complex types; centralize in `types.hpp`
- Deducing this (C++23) eliminates const/ref overloads

---

## 3. Containers & Data Structures

| Scenario | Recommendation |
|----------|---------------|
| Small key-value (<1000) | `std::flat_map` |
| Large key-value | `std::unordered_map` |
| Ordered + frequent insert/delete | `std::map` |
| Multi-dimensional array view | `std::mdspan` |
| Fixed size | `std::array` |
| Default | `std::vector` |

Prefer SoA on hot paths:

```cpp
// AoS — poor cache         // SoA — good cache
struct Order {               struct Orders {
  int id;                      vector<int> ids;
  string name;                 vector<string> names;
  double price;                vector<double> prices;
};                           };
```

Alignment: place large members first to reduce padding. Use `-Wpadded` to check.

---

## 4. Ranges

Pipelines replace hand-written loops (C++20; completed in C++23):

```cpp
auto result = data
    | views::filter([](int x) { return x % 2 == 0; })
    | views::transform([](int x) { return x * 10; })
    | ranges::to<vector>();
```

C++23 additions: `zip`, `chunk`, `slide`, `join`, `std::generator`.

---

## 5. Compile-Time

```cpp
// constexpr — computed at compile time, zero runtime cost
constexpr auto lookup = [] {
    std::array<int, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = i * i;
    return t;
}();

// consteval — forced compile-time evaluation
consteval int must_ct(int x) { return x * 2; }

// if consteval — branch between compile-time and runtime
constexpr int f(int x) {
    if consteval { return precise(x); }
    else { return fast(x); }
}

// concepts — replace SFINAE with clear error messages
template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;
```

---

## 6. Attributes

```cpp
// Safety
[[nodiscard("handle error")]] expected<int, Error> parse(string_view s);
[[maybe_unused]] int debug_counter = 0;
[[fallthrough]];

// Performance
if (error != 0) [[unlikely]] { handle_error(); }
[[no_unique_address]] Empty alloc;

// Advanced (incorrect use is UB)
[[assume(n > 0)]];
void f(int* __restrict a, int* __restrict b, int n) noexcept;
```

---

## 7. Concurrency

> See `references/concurrency.md`

- RAII locks: `lock_guard` (single) / `scoped_lock` (multiple) / `unique_lock` (condition wait)
- Lightweight signaling: `atomic::wait/notify`
- stdexec (P2300): `just | then | when_all | on(scheduler)`
- New projects: use NVIDIA/stdexec; legacy: Boost.Asio + `co_await` as bridge

---

## 8. Architecture

> See `references/architecture.md`

- IO/logic separation: core logic as pure functions; IO at boundaries
- Pimpl compilation firewall: changes to Impl don't trigger project-wide rebuilds
- `variant` + `visit` over inheritance (when the type set is closed)
- Template layering: non-template core + thin template shell to reduce instantiations

---

## 9. Naming & Style

> See `references/naming-style.md`

- Types `PascalCase`; variables/functions `snake_case`; members `name_`
- **Explicit comparisons**: `ptr != nullptr`, `opt.has_value()` — no `!` or implicit bool
- **C++ casts**: `static_cast > dynamic_cast > const_cast > reinterpret_cast` — no C-style casts
- **`std::` prefix**: `std::size_t`, `std::int32_t`, `std::uint64_t`
- Short namespaces (`net`, `db`); files as `snake_case.hpp`

---

## 10. Project Architecture

> See `references/architecture.md`

Layers: `api → service → core ← infra ← common`. Dependencies flow downward; core inverts via concepts. Each layer is an independent build target; public headers expose only interfaces + forward declarations.

---

## 11. Design Patterns

> See `references/design-patterns.md`

| Need | Modern Alternative |
|------|--------------------|
| Polymorphism | `variant` + `visit` |
| Strategy | Templates / lambdas |
| Mixin | `deducing this` |
| Runtime polymorphism without inheritance | Type Erasure |
| Fluent construction | `deducing this` Builder |
| Events | `Signal<Args...>` |
| Singleton | Construct + inject by reference |

---

## 12. Build Acceleration

> See `references/build.md`

Essential: **PCH** (STL/third-party) + **Ninja** + **ccache** + parallel builds.
High-impact: **Unity Build** (2-5x), reduce includes (forward declarations + IWYU).
Architectural: split targets, pimpl isolation, Modules (see `references/modules.md`).

---

## 13. Code Review & clang-tidy

> See `references/code-review.md`

Review: initialization / const / explicit / enum class / explicit comparisons / Rule of 0|5 / `[[nodiscard]]` / span / no raw new / noexcept moves / RAII locks.

clang-tidy: **`HeaderFilterRegex: '^(src|include)/.*'`** — only check project code. Set bug/UB checks as WarningsAsErrors.

---

## 14. Output

`std::print` replaces iostream / printf. Use spdlog / fmt for logging. When using iostream, prefer `'\n'` over `endl`.

---

## 15. Project Configuration

> See `references/build.md`

Defaults: Debug enables ASan+UBSan; Release uses `-O2`; always `-Wall -Wextra -Werror`; CI adds `-Wconversion -Wshadow`.

---

## 16. Testing

> See `references/testing.md`

- TDD: **always write a reproduction test before fixing a bug**
- Isolation: dependency injection, no global state, each test independent
- Determinism: no `sleep`, no real time/network/filesystem dependencies
- Mocks in moderation: ≤3 per test; more indicates the code under test is too coupled
- CI must run ASan + UBSan; add TSan for concurrent code

---

## 17. Pitfalls

> See `references/pitfalls.md`

- **Use after move** / **move const** (silent copy) / **missing noexcept on move** (vector falls back to copy)
- **Dangling view/span** / **lambda reference escape** / **range-for temporaries**
- **Unnamed lock_guard** (destroyed immediately) / **volatile != synchronization**
- **const members block move** / **cast away const = UB**

---

## Modern Replacements

| Legacy | Prefer | Why |
|--------|--------|-----|
| `std::thread` | `std::jthread` | Auto-join + `stop_token` cooperative cancellation |
| `std::function` | `std::move_only_function` | Supports move-only callables, no copy overhead |
| `union` | `std::variant` | Type-safe; compiler checks visit exhaustiveness |
| `void*` | `std::any` | Type-safe erasure; `any_cast` is checked |
| `new` / `delete` | `std::unique_ptr` / `make_unique` | RAII, zero overhead, no leaks |
| `T* + size` | `std::span<T>` | Bounds-safe, carries size |
| `const string&` | `std::string_view` | Zero-copy; accepts literals/string/substrings |
| `const vector<T>&` | `std::span<const T>` | Accepts array/vector/C arrays |
| C arrays | `std::array` | Value semantics, works with STL algorithms |
| `printf` / `cout` | `std::print` / `std::format` | Type-safe + performant |
| `atoi` / `strtol` | `std::from_chars` | No allocation, no locale, clear error reporting |
| `sprintf` | `std::format_to` | No buffer overflow |
| `__FILE__` / `__LINE__` | `std::source_location` | Standard, passable, consteval |
| Platform backtrace | `std::stacktrace` | Cross-platform (C++23) |
| `reinterpret_cast` for bit conversion | `std::bit_cast` | constexpr, no UB |
| Hand-written bit ops | `<bit>`: `popcount`/`countl_zero`/`byteswap` | Standard, compiler-optimizable |
| Runtime endian detection | `std::endian` | Compile-time constant |
| `static_cast<int>(enum)` | `std::to_underlying` | Clearer intent |
| `find() != end()` | `.contains()` | One line instead of two (C++20 set/map, C++23 string) |
| `std::sort(v.begin(), v.end())` | `std::ranges::sort(v)` | Cleaner, stronger constraints |
| `std::map` (small collections) | `std::flat_map` | Cache-friendly (C++23) |
| Hand-written `ssize` | `std::ssize(container)` | Signed size, avoids mixed-sign warnings |
| `lock` + `unlock` | `std::scoped_lock` | RAII, prevents forgotten unlock |
| `std::bind` | Lambda | More readable, more efficient, inlinable |
| `std::auto_ptr` | `std::unique_ptr` | Long deprecated; unambiguous ownership |
| `__builtin_unreachable()` | `std::unreachable()` | Standard, cross-platform (C++23) |

---

## C++23 Quick Reference

| Feature | Purpose |
|---------|---------|
| `expected` | Error handling |
| `print` | Replaces cout/printf |
| `deducing this` | Eliminates overloads |
| `string::contains` | Replaces find!=npos |
| `mdspan` | Multi-dimensional views |
| `zip/chunk` | Ranges enhancements |
| `generator` | Coroutine sequences |
| `flat_map` | Cache-friendly dictionary |
| `if consteval` | Compile/runtime branching |
| `auto(x)` | Explicit decay-copy |

## C++26 Quick Reference

| Feature | Action |
|---------|--------|
| `std::execution` | Use stdexec library now |
| Reflection / Pattern Matching / Contracts | Wait and see |

---

## Project Notes (libmdbx)

Apply this guide to the C++ API layer (`mdbx.h++`, `mdbx++/`, `src/mdbx.c++`) with these project-specific adjustments:

1. **Header-first, single-header design**: The public C++ API is header-heavy by design — `mdbx.h++` pulls in `mdbx++/decl_*.h++` then `impl_*.h++` (decl/impl split), with only 39 non-inline `__cold` methods in `src/mdbx.c++`. Topology, class inventory and build/amalgamation specifics: [`skynet/cxx-api.md`](../../../skynet/cxx-api.md) §1–§5.
2. **Exceptions never cross the C boundary**: The guide's "exceptions allowed internally, never across boundaries" is *mandatory* here — `error::success_or_throw` converts C `MDBX_result` into C++ exceptions, and `exception_thunk` converts them back at the boundary. See [`skynet/cxx-api.md`](../../../skynet/cxx-api.md) §4 before touching the bridge.
3. **Compiler matrix constrains "modern" features**: Supported compilers include Elbrus LCC, GCC ≥ 11.3, Clang ≥ 14, MSVC ≥ 19.44. Keep public headers conservative (C++17 baseline, C++20/23 features guarded), avoid C++23-only constructs in shared headers, and keep code portable across the matrix. See [`skynet/cxx-api.md`](../../../skynet/cxx-api.md) §7.
4. **Style override**: Project mandates LLVM code style (`clang-format`, `.clang-format`) per `AGENTS.md`. Where the guide's naming table conflicts with the existing codebase style, the existing style wins — consistency beats preference.
5. **Core is C11, not C++**: The engine (`src/*.c`) is C11. Do not apply C++-only guidance (STL, exceptions, `std::` replacements) to the C core. Logging uses the project's own macros (`NOTICE`/`WARNING`/`ERROR` in `src/logging_and_debug.h`), not `std::print`/spdlog.
6. **ABI stability**: The C++ API is stable across releases; public class layout must not change. The wrappers hold `MDBX_env`/`MDBX_txn`/cursor handles (no Pimpl firewall in the classic sense) — changing public members is a breaking change requiring explicit user opt-in.
7. **`std::filesystem` caveat**: The project probes `std::filesystem` availability (`LIB_STDCXXFS`); do not assume it links unconditionally on all toolchains.
8. **Testing discipline**: Follow the repo's verification levels (L1 specific test → L7 human-controlled battery; see `AGENTS.md` and [`skynet/test-coverage.md`](../../../skynet/test-coverage.md)). Sanitizer builds (ASan/UBSan/TSan) are part of the standard pipeline, matching the guide's §16 testing rules.