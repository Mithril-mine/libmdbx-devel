# Common Pitfalls Reference

## Move Semantics Pitfalls

```cpp
// 1. Use after move — UB or unspecified state
string s = "hello";
auto s2 = std::move(s);
s.size();  // Unspecified state — don't touch

// 2. Moving a const object — silently degrades to copy
const string cs = "hello";
auto s2 = std::move(cs);  // Actually copies! const blocks the move

// 3. Move ctor/assign missing noexcept — vector won't use move
struct Bad {
    Bad(Bad&&);  // No noexcept → vector copies during reallocation
};
struct Good {
    Good(Good&&) noexcept;  // vector will move during reallocation
};
```

## Lifetime Pitfalls

```cpp
// 4. Dangling string_view / span
string_view dangling() {
    string s = "hello";
    return s;  // s destroyed, view dangles
}

// 5. Temporary destroyed in range-for
for (auto x : get_vector()) {}  // OK — temporary lives until loop ends
for (auto& x : get_obj().get_vec()) {}  // Dangerous! Intermediate temporary may be destroyed early

// 6. Lambda captures reference that escapes scope
auto make_lambda() {
    int x = 42;
    return [&x] { return x; };  // x destroyed — dangling
}
```

## Initialization Pitfalls

```cpp
// 7. {} rejects narrowing conversions (this is good, but be aware)
int x{3.14};   // Compile error — narrowing
int x = 3.14;  // Silent truncation — no error

// 8. auto + {} deduction is surprising
auto x = {1, 2, 3};  // initializer_list<int>, not vector

// 9. Uninitialized local variables
int x;
if (x > 0) {}  // UB — x has indeterminate value
```

## optional / variant Pitfalls

```cpp
// 10. Accessing optional without checking
optional<int> opt;
*opt;        // UB
opt.value(); // Throws bad_optional_access

// 11. variant type mismatch
variant<int, string> v = "hello";
get<int>(v);  // Throws bad_variant_access
```

## Concurrency Pitfalls

```cpp
// 12. Detach loses object lifetime control
{
    vector<int> data = {1,2,3};
    thread t([&data] { process(data); });
    t.detach();
}  // data destroyed, thread still using it — UB

// 13. volatile != thread safety
volatile int counter = 0;
// Two threads doing ++counter — still a data race

// 14. Unnamed lock_guard destroyed immediately
{
    lock_guard(mutex_);  // Temporary — locks then immediately unlocks
    data_.push_back(x);  // No lock held here
}
// Correct: lock_guard lock(mutex_);
```

## const Pitfalls

```cpp
// 15. const member blocks move
struct Bad {
    const string name;  // Prevents move ctor/assign
};
// Use private + getter instead

// 16. Casting away const — UB if original object is const
const int x = 42;
int* p = const_cast<int*>(&x);
*p = 100;  // UB — x itself is const

// 17. Lambda initialization for const — correct approach (see tips.md IILE section)
const auto config = [&] {
    Config c;
    c.timeout = 30;
    c.host = get_host();
    return c;
}();
```
