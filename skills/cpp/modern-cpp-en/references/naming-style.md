# Naming Conventions & Code Style Reference

## General

Pick one style and apply it consistently across the entire project. The following reflects consensus across major C++ projects (STL, Boost, Google, LLVM):

## Types & Variables

| Element | Style | Example |
|---------|-------|---------|
| Class / struct / enum | `PascalCase` | `HttpClient`, `OrderStatus` |
| Function / method | `snake_case` or `camelCase` | `find_user()` / `findUser()` |
| Variable | `snake_case` | `user_count`, `max_retry` |
| Member variable | `snake_case` + `_` suffix | `name_`, `data_` |
| Constant / constexpr | `kPascalCase` or `ALL_CAPS` | `kMaxRetry` / `MAX_RETRY` |
| Template parameter | `PascalCase` | `typename ValueType` |
| Concept | `PascalCase` | `Hashable`, `Serializable` |
| Namespace | `snake_case`, short | `net`, `io`, `detail` |
| Macro (avoid if possible) | `ALL_CAPS` + project prefix | `MYLIB_ASSERT(x)` |
| Enum value | `PascalCase` | `Color::DarkRed` |
| Filename | `snake_case` | `http_client.hpp`, `http_client.cpp` |

## Key Rules

```cpp
// Type vs variable — distinguishable at a glance
class UserService {};       // PascalCase = type
UserService user_service;   // snake_case = variable

// Member suffix — no confusion with locals
class Foo {
    int count_;             // member
    void bar(int count) {   // parameter, no name collision
        count_ = count;
    }
};

// Concepts: uppercase + adjective form
template<typename T>
concept Printable = requires(T t) { std::print("{}", t); };

// Namespaces: short and precise — avoid junk-drawer names like utils / misc / common
namespace http {}
namespace db {}
namespace detail {}  // internal implementation only
```

## Don't

- `m_` prefix (Java convention; C++ community prefers `_` suffix)
- Hungarian notation (`iCount`, `strName`) — the type system is strong enough
- Abbreviations unless domain-consensus (`ctx` OK; `usrSvc` not OK)
- Single-letter variables except loop counters `i,j,k` and short lambda parameters

## Header File Extensions

| Extension | Meaning |
|-----------|---------|
| `.hpp` | C++ header (recommended; distinguishes from C `.h`) |
| `.h` | Also acceptable; many projects use it |
| `.ipp` / `.tpp` | Template implementation file (optional) |

## Explicit Comparisons (No Implicit bool / Negation)

**Do not** use `!` negation or implicit bool conversions. **Always use `==` / `!=` for explicit comparisons**:

```cpp
// Pointers
if (ptr != nullptr) {}     // OK
if (ptr == nullptr) {}     // OK
if (ptr) {}                // Avoid — implicit bool
if (!ptr) {}               // Avoid — negation

// optional
if (opt.has_value()) {}              // OK
if (opt == std::nullopt) {}          // OK
if (!opt) {}                         // Avoid

// expected
if (result.has_value()) {}           // OK
if (result.has_value() == false) {}  // OK
if (!result) {}                      // Avoid

// Containers
if (vec.empty() == false) {}  // OK
if (vec.size() != 0) {}      // OK
if (!vec.empty()) {}          // Avoid

// error_code
if (ec == std::error_code{}) {}  // OK — explicitly compare against no-error
if (!ec) {}                      // Avoid

// Integers / enums
if (count != 0) {}    // OK
if (count) {}         // Avoid
```

Rationale: explicit comparisons convey intent clearly, are instantly readable in code review, and eliminate type conversion ambiguity.

## Type Casting: No C-Style Casts — Use C++ Casts

```cpp
// C-style — forbidden (bypasses the type system, converts anything, no warnings)
int x = (int)3.14;
void* p = (void*)&obj;

// C++ casts — each has clear semantics, compiler can check
int x = static_cast<int>(3.14);          // Value conversion (known-safe types)
Base* b = dynamic_cast<Base*>(derived);  // Polymorphic downcast (runtime check)
const int* cp = &x;
int* mp = const_cast<int*>(cp);          // Remove const (rarely needed, usually a design issue)
auto* rp = reinterpret_cast<char*>(&x);  // Low-level bit reinterpretation (only for low-level scenarios)
```

Preference order: `static_cast` > `dynamic_cast` > `const_cast` > `reinterpret_cast`.
If `static_cast` works, don't use anything after it. `reinterpret_cast` is only for serialization / networking / hardware interaction.

## std:: Prefix

Fixed-width types like `size_t`, `int32_t`, `uint64_t` **must always carry the `std::` prefix**:

```cpp
std::size_t count = vec.size();      // OK
std::int32_t code = get_code();      // OK
std::uint64_t id = get_id();         // OK

size_t count = vec.size();           // Avoid — relies on global namespace leak
int32_t code = get_code();           // Avoid
```

Reason: `size_t` etc. appearing in the global namespace via C headers is an implementation detail, not a standard guarantee. `std::size_t` is the correct, portable form.
