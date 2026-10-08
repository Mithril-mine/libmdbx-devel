# Practical Tips Reference

## Unsigned Integer Safe Comparison

```cpp
// Dangerous — when val.size() == 0, 0 - 1 wraps to size_t max
for (std::size_t i = 0; i < val.size() - 1; ++i) {}

// Safe — addition never underflows
for (std::size_t i = 0; i + 1 < val.size(); ++i) {}

// Same principle for distance checks
if (a + margin < b) {}   // Safe
if (a - margin > 0) {}   // Dangerous (unsigned underflow)
```

## std::midpoint — Overflow-Safe Midpoint

```cpp
// Dangerous — a + b may overflow
auto mid = (a + b) / 2;

// Safe
auto mid = std::midpoint(a, b);
```

## std::exchange — Move and Reset in One

```cpp
// Manual approach
auto old = ptr_;
ptr_ = nullptr;
return old;

// One line
return std::exchange(ptr_, nullptr);

// Particularly useful in move constructors
Node(Node&& other) noexcept
    : data_(std::exchange(other.data_, nullptr))
    , size_(std::exchange(other.size_, 0)) {}
```

## IILE — Complex const Initialization

```cpp
// When a const object needs complex initialization, use immediately invoked lambda
const auto config = [&] {
    Config c;
    c.host = get_env("HOST");
    c.port = parse_port(get_env("PORT"));
    c.timeout = std::chrono::seconds(30);
    return c;
}();
// config is const, but construction can be arbitrarily complex
```

## if-Statement Initializer (C++17)

```cpp
// Old: variable leaks into outer scope
auto it = map.find(key);
if (it != map.end()) { use(it->second); }

// New: variable scoped to the if block
if (auto it = map.find(key); it != map.end()) {
    use(it->second);
}

// Works with expected / optional
if (auto result = parse(input); result.has_value()) {
    process(*result);
}
```

## Structured Bindings (C++17)

```cpp
// Map iteration
for (const auto& [key, value] : my_map) {
    std::print("{}: {}\n", key, value);
}

// Multiple return values
auto [iter, inserted] = map.try_emplace(key, value);

// Pair / tuple unpacking
auto [x, y, z] = get_coordinates();
```

## Designated Initializers (C++20)

```cpp
struct Options {
    int timeout = 30;
    bool verbose = false;
    std::string host = "localhost";
};

// Clear, self-documenting, order-safe
auto opts = Options{.timeout = 60, .verbose = true};
```

## std::erase_if (C++20) — Replaces Erase-Remove

```cpp
// Old: erase-remove idiom
v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());

// New: one line
std::erase_if(v, [](int x) { return x < 0; });

// Also works on map / set / unordered_map etc.
std::erase_if(my_map, [](const auto& pair) { return pair.second == 0; });
```

## reserve Pre-Allocation

```cpp
// Poor — multiple reallocations
std::vector<int> result;
for (auto& item : source) {
    result.push_back(transform(item));
}

// Good — single allocation
std::vector<int> result;
result.reserve(source.size());
for (auto& item : source) {
    result.push_back(transform(item));
}

// Better — use ranges directly
auto result = source | views::transform(transform_fn) | ranges::to<std::vector>();
```

## emplace_back vs push_back

```cpp
// push_back — constructs temporary, then moves
vec.push_back(MyStruct{arg1, arg2});

// emplace_back — constructs in-place, no temporary
vec.emplace_back(arg1, arg2);
```

## std::as_const — Add const Without const_cast

```cpp
// Scenario: force the const overload
for (auto& item : std::as_const(container)) {
    // Calls const begin() / end(), won't trigger COW or mutation
}
```

## Literal Suffixes

```cpp
using namespace std::literals;

auto sv = "hello"sv;              // std::string_view
auto s  = "hello"s;               // std::string
auto dur = 100ms;                 // std::chrono::milliseconds
auto complex = 3.0 + 4.0i;       // std::complex

// Custom strong types
consteval Meter operator""_m(long double v) { return Meter{static_cast<double>(v)}; }
auto height = 1.75_m;
```

## Fold Expressions (C++17)

```cpp
// Sum
template<typename... Args>
auto sum(Args... args) { return (args + ...); }

// Print all
template<typename... Args>
void print_all(Args&&... args) {
    (std::print("{} ", args), ...);
}

// All satisfy condition
template<typename... Args>
bool all_positive(Args... args) { return ((args > 0) && ...); }
```

## if constexpr — Compile-Time Branch Elimination

```cpp
template<typename T>
auto serialize(const T& value) {
    if constexpr (std::is_arithmetic_v<T>) {
        return std::to_string(value);
    } else if constexpr (requires { value.to_string(); }) {
        return value.to_string();
    } else {
        static_assert(false, "Unsupported type");
    }
}
```

## static_assert — Compile-Time Constraint Checking

```cpp
template<typename T>
class Buffer {
    static_assert(std::is_trivially_copyable_v<T>, "Buffer requires trivially copyable type");
    static_assert(sizeof(T) <= 64, "Element too large for Buffer");
    // ...
};
```

## auto&& in Range-For

```cpp
// When unsure of container's return type, auto&& is safest
for (auto&& item : get_items()) {
    // Correctly binds whether the return is T&, const T&, or T&&
}
```

## std::clamp — Replaces Hand-Written min/max

```cpp
// Manual
int clamped = std::max(lo, std::min(val, hi));

// Clear
int clamped = std::clamp(val, lo, hi);
```

## try_emplace — Conditional Insert Without Wasted Construction

```cpp
// emplace — constructs value even if key exists (wasteful)
map.emplace(key, expensive_value());

// try_emplace — doesn't construct value if key exists
map.try_emplace(key, arg1, arg2);

// With structured bindings
auto [it, inserted] = map.try_emplace(key, create_value());
if (inserted == false) {
    // key already existed, it->second is the old value
}
```

## Scope Guard — Ensure Cleanup

```cpp
// C++26 will have std::scope_exit; for now, simple implementation
struct ScopeGuard {
    std::function<void()> fn;
    ~ScopeGuard() { fn(); }
} scope_guard{[&] { close(fd); }};
```

Also possible with `unique_ptr` + custom deleter — see `smart-pointers.md`.
