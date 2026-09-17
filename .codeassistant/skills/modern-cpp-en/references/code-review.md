# Code Review & clang-tidy Reference

## Review Checklist

### Types & Safety

- [ ] All variables initialized at declaration
- [ ] Variables `const` / `constexpr` by default
- [ ] Single-argument constructors marked `explicit`
- [ ] `enum class` instead of plain `enum`
- [ ] `nullptr` instead of `0` / `NULL`
- [ ] No C-style casts (use `static_cast` etc.)
- [ ] No narrowing conversions
- [ ] Rule of Zero or Rule of Five
- [ ] Conditionals use `==` / `!=` explicit comparisons — no `!` or implicit bool

### Interfaces & Resources

- [ ] Functions returning expected/optional marked `[[nodiscard]]`
- [ ] Interfaces use span/string_view, not const vector&/const string&
- [ ] No raw `new`/`delete` — use smart pointers / RAII
- [ ] No `using namespace` in headers
- [ ] Headers have include guards or `#pragma once`
- [ ] Headers have no unnecessary includes (use forward declarations)

### Move Semantics & Lifetime

- [ ] Move ctor/assign marked `noexcept`
- [ ] Source object not used after move
- [ ] No dangling string_view / span / reference
- [ ] Lambda captured references don't escape their scope

### Concurrency

- [ ] All locks use RAII (lock_guard / scoped_lock / unique_lock)
- [ ] lock_guard variables are named (not temporaries)
- [ ] Multiple locks use scoped_lock
- [ ] No volatile used for synchronization

### Performance

- [ ] No unnecessary allocations on hot paths
- [ ] Multi-threaded shared data avoids false sharing (`alignas(64)`)
- [ ] Large objects passed by reference
- [ ] `'\n'` instead of `std::endl`

## clang-tidy Configuration

**Key principle: only check your own code; filter out noise from third-party / generated code.**

`.clang-tidy` (project root):
```yaml
Checks: >
  -*,
  bugprone-*,
  cppcoreguidelines-*,
  modernize-*,
  performance-*,
  readability-*,
  -modernize-use-trailing-return-type,
  -readability-magic-numbers,
  -cppcoreguidelines-avoid-magic-numbers

# Only check project source — exclude third-party / generated files
HeaderFilterRegex: '^(src|include)/.*'

WarningsAsErrors: >
  bugprone-use-after-move,
  bugprone-dangling-handle,
  cppcoreguidelines-pro-type-cstyle-cast,
  modernize-use-nullptr
```

**`HeaderFilterRegex` is the most critical line** — without it, clang-tidy checks all included headers, including STL, Boost, protobuf-generated code, producing unmanageable noise.

Exclusion strategies (by build system):

```bash
# Run only on project files (recommended, cleanest)
find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-tidy -p build

# Exclude specific directories
clang-tidy --header-filter='^(?!.*(third_party|build|generated)).*' src/*.cpp

# CMake integration
set(CMAKE_CXX_CLANG_TIDY "clang-tidy;--header-filter=src/.*")

# xmake
add_rules("plugin.compile_commands.autoupdate")
-- then use clang-tidy -p .xmake/compile_commands.json
```

Common noise sources and handling:

| Source | Symptom | Fix |
|--------|---------|-----|
| STL headers | Many modernize suggestions | Exclude via `HeaderFilterRegex` |
| protobuf / flatbuffers generated code | Naming / style warnings | Exclude `generated/` directory |
| Third-party library headers | cppcoreguidelines warnings | Exclude via `HeaderFilterRegex` |
| Macro expansions | False warnings | Suppress with `// NOLINT` per line |

`// NOLINT` suppression (only for confirmed false positives):
```cpp
auto* p = reinterpret_cast<char*>(buf);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
```

| Category | Strategy |
|----------|----------|
| Bug / UB | Required; set as WarningsAsErrors |
| Performance suggestions | Evaluate case by case |
| Style suggestions | Follow team conventions; selectively disable |
