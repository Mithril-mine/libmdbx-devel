# Documentation Reference

## Comment Principles

- **Code says what, comments say why** — don't comment the obvious
- Good naming and structure reduce the need for comments
- Comments must stay in sync with code; stale comments are worse than no comments

```cpp
// Bad — comment duplicates code
int count = 0;  // Initialize count to 0

// Good — explains non-obvious decision
int count = 0;  // Start at 0 because protocol requires first packet sequence number to be 0

// Good — explains workaround
// GCC 13 misoptimizes this loop under -O2; manual unrolling as workaround
// See: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=12345
```

## Doxygen Style

### Function Documentation

```cpp
/// @brief Find a user by ID in the user list
///
/// Time complexity O(log n). Thread-safe.
///
/// @param id User ID, must be > 0
/// @return User if found, otherwise NotFound error
/// @throws None (noexcept semantics expressed via expected)
///
/// @code
/// auto user = find_user(42);
/// if (user.has_value()) { process(*user); }
/// @endcode
[[nodiscard]] std::expected<User, Error> find_user(int id);
```

### Class Documentation

```cpp
/// @brief Thread-safe bounded task queue
///
/// Producer-consumer pattern. Push blocks when full; pop blocks when empty.
/// After close() is called, no new tasks are accepted. Consumers drain
/// remaining tasks then exit.
///
/// @tparam T Task type, must be MoveConstructible
///
/// @note Maximum capacity is fixed at construction and cannot change
/// @warning Do not call push/pop while holding an external lock — may deadlock
template<typename T>
class BoundedQueue { /* ... */ };
```

### When to Write / When Not to

| Scenario | Document? |
|----------|----------|
| Public API / library interface | **Must** |
| Non-obvious algorithm / math formula | **Must** |
| Workaround / hack | **Must** (include issue link) |
| Simple internal function | Not needed (good name suffices) |
| Getter / setter | Not needed |
| Test code | Usually not needed |

## File Headers

```cpp
/// @file order_service.hpp
/// @brief Core order service interface
///
/// Handles order creation, queries, and status transitions. Does not include persistence logic.
```

File headers describe **what this file is responsible for**, helping readers decide "is what I'm looking for in here?"

## TODO / FIXME / HACK Tags

```cpp
// TODO(author): Support batch queries — current one-by-one is inefficient
// FIXME(author): May lose updates under concurrency — needs locking
// HACK(author): Working around library bug — remove after upgrading to v2.0
```

Rules:
- Must include author identifier
- TODO = known improvement
- FIXME = known bug
- HACK = temporary solution, must have removal condition

## Doxygen Configuration Essentials

```
# Doxyfile key settings
EXTRACT_ALL            = NO       # Only generate for documented items
INPUT                  = include/ src/
FILE_PATTERNS          = *.hpp *.cpp
RECURSIVE              = YES
EXCLUDE_PATTERNS       = */third_party/* */build/*
GENERATE_LATEX         = NO
WARN_IF_UNDOCUMENTED   = YES      # Warn on undocumented public API
WARN_AS_ERROR          = FAIL_ON_WARNINGS  # Enforce in CI
```

## README Structure (Project-Level)

```
# Project Name
One-sentence description

## Quick Start
Minimum steps to build + run

## Dependencies
List all external deps and versions

## Build
Detailed build steps covering major platforms

## Usage
Core API examples

## Architecture
High-level structure, link to detailed docs

## Contributing
Code style, PR process, testing requirements
```
