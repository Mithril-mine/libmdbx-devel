# API Design Reference

## Parameter Passing Rules

| Type Size | Passing | Examples |
|-----------|---------|----------|
| Small & cheap to copy (≤2 words) | By value | `int`, `double`, `char`, `string_view`, `span` |
| Large & read-only | `const&` | `const string&`, `const vector<T>&` |
| Sink (consumed) | By value + move | `void set_name(string name) { name_ = move(name); }` |
| Optional / nullable | `optional<T>` or `T*` (non-owning) | `optional<int> limit` |
| Read-only contiguous data | `span<const T>` | Replaces `const vector<T>&` / `T* + size` |

## class vs struct

```cpp
// struct — all members independent, no invariants, pure data aggregate
struct Point { double x; double y; };
struct Config { int timeout; string host; };

// class — has invariants to maintain, members are constrained
class Date {
    int year_, month_, day_;  // must ensure valid date
public:
    Date(int y, int m, int d);
};
```

Rule: **struct = data bag; class = abstraction with behavior/constraints**.

## explicit Single-Argument Constructors

```cpp
struct Meter {
    explicit Meter(double v) : value(v) {}
    double value;
};

Meter m = 3.0;     // Compile error — prevents accidental implicit conversion
Meter m(3.0);      // OK
Meter m{3.0};      // OK
```

Unless you **intentionally** support implicit conversion (e.g., `string_view` from `const char*`), single-argument constructors must be `explicit`.

## Rule of Zero / Five

```cpp
// Rule of Zero — preferred. No resource management; let the compiler generate all special members.
struct Employee {
    string name;
    int id;
    // Compiler auto-generates: destructor, copy ctor/assign, move ctor/assign
};

// Rule of Five — when managing resources, define all five.
class Buffer {
    unique_ptr<char[]> data_;
    size_t size_;
public:
    ~Buffer();
    Buffer(const Buffer&);
    Buffer& operator=(const Buffer&);
    Buffer(Buffer&&) noexcept;              // Move ctor must be noexcept
    Buffer& operator=(Buffer&&) noexcept;   // Move assign must be noexcept
};
```

Note: Move ctor/assign **must be `noexcept`** — otherwise `vector` will copy instead of move during reallocation.

## Parameter Type Selection

| Scenario | Use | Avoid |
|----------|-----|-------|
| Read-only contiguous data | `std::span<const T>` | `const vector<T>&` / `T* + size` |
| Read-only string | `std::string_view` | `const string&` |
| Nullable value | `std::optional<T>` | `T*` / `bool + T` |
| Ownership transfer | `std::unique_ptr<T>` | Raw pointer |
| Shared ownership | `std::shared_ptr<T>` (use sparingly) | Global variables |

## Type Safety: Strong Type Wrappers

```cpp
struct UserId { int value; };
struct OrderId { int value; };

// Parameters can't be confused
void process(UserId user, OrderId order);
```

## Deducing this (C++23) — Eliminate Redundant Overloads

When you need const / non-const / rvalue support simultaneously, use deducing this instead of 4 overloads:

```cpp
struct Container {
    std::vector<int> data;

    // One function replaces four overloads
    decltype(auto) get(this auto&& self, int i) {
        return std::forward<decltype(self)>(self).data[i];
    }

    // Fluent API
    auto&& add(this auto&& self, int x) {
        self.data.push_back(x);
        return std::forward<decltype(self)>(self);
    }
};
```

Suitable for: getters, fluent APIs, wrappers, mixins, iterator facades.
Not needed for: simple functions, cases where const/ref distinction is irrelevant.

## Recursive Lambda

```cpp
auto fib = [](this auto&& self, int n) -> int {
    if (n <= 1) return n;
    return self(n - 1) + self(n - 2);
};
```

## Type Aliases (using)

Use `using` declarations for readable aliases when complex type names are unclear. **Use `using`, not `typedef`.**

```cpp
// Make callback types readable
using OnComplete = std::function<void(std::expected<Response, Error>)>;
using UserId = std::int64_t;
using Timestamp = std::chrono::steady_clock::time_point;
using Headers = std::flat_map<std::string, std::string>;

// Template aliases
template<typename T>
using Vec = std::vector<T>;

template<typename T>
using Result = std::expected<T, Error>;
```

### Project-Level Type Alias Management

Centralize in a single header with namespace isolation:

```cpp
// include/project/types.hpp
#pragma once
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

namespace project {

// Base type aliases
using Id = std::int64_t;
using Bytes = std::vector<std::byte>;

// Error handling
enum class Error { NotFound, InvalidInput, Timeout };

template<typename T>
using Result = std::expected<T, Error>;

// Callbacks
using Callback = std::function<void(Result<void>)>;

}  // namespace project
```

Rules:
- One `types.hpp` per project; shared aliases go here
- Module-private aliases stay in their own headers, not in `types.hpp`
- Aliases must be namespaced — never define at global scope

### std:: Prefix

> See `naming-style.md` — fixed-width types like `std::size_t`, `std::int32_t` must always carry the `std::` prefix.
