# Testing Reference

Framework is project-specific (GoogleTest, Catch2, doctest, Boost.Test are all viable). This document focuses on **testing thinking**.

## TDD Loop

```
RED → GREEN → REFACTOR
 ↑                 ↓
 └─────────────────┘
```

1. **RED**: Write a failing test that describes expected behavior
2. **GREEN**: Write the minimum code to make it pass
3. **REFACTOR**: Clean up under test protection, keeping green

Strict TDD isn't always required, but **always write a reproduction test before fixing a bug** — otherwise you don't know if it's actually fixed.

## Test Layers

```
tests/
├── unit/            # Pure logic, no IO, millisecond-level
├── integration/     # Involves files/network/database
├── fuzz/            # Fuzz testing (optional)
└── testdata/        # Test data/fixtures
```

| Layer | Characteristics | Run Frequency |
|-------|----------------|---------------|
| Unit | No external deps, deterministic, fast (<1s) | Every commit |
| Integration | Real IO, potentially slow | CI / pre-merge |
| Fuzz | Random input, boundary exploration | Periodic / targeted |

## Core Principles

### Isolation

```cpp
// Bad — depends on global state, tests pollute each other
static Database& global_db();

// Good — dependency injection, each test gets independent instance
class OrderService {
    std::function<expected<Order, Error>(int)> find_;
public:
    explicit OrderService(auto find) : find_(std::move(find)) {}
};

// Inject a fake during testing
auto svc = OrderService([](int id) -> expected<Order, Error> {
    return Order{.id = id, .status = Status::Active};
});
```

### Determinism

Tests must **produce the same result every run**. Common determinism violations:

| Problem | Solution |
|---------|----------|
| System clock dependency | Inject clock interface or fake time |
| Filesystem dependency | Unique temp directory per test, clean up after |
| Network dependency | Fakes for unit tests; isolated env for integration |
| Random input | Fixed seed, reproducible |
| Concurrency timing | Use latch / condition_variable, not `sleep` |

### Mock vs Fake

```
Mock  — Verify interactions: "Was this function called? With correct args?"
Fake  — Provide behavior: "I'm an in-memory DB, same API but faster"
Stub  — Return fixed values: "Always returns 42 regardless of input"
```

**Selection criteria:**
- Verify "what was done" → Mock
- Need real behavior without external deps → Fake
- Simple value objects → Don't mock; use the real thing
- **Over-mocking is a code smell** — if a test needs 5 mocks, the code under test is too coupled

## Test Design

### Arrange-Act-Assert (AAA)

```cpp
// Arrange — set up data and dependencies
auto store = InMemoryUserStore();
store.add(User{.name = "alice"});

// Act — execute the operation under test
auto result = store.find("alice");

// Assert — verify results
ASSERT(result.has_value());
EXPECT(result->name == "alice");
```

Each test covers **one behavior**. Multiple assertions are fine, but they must all relate to the same behavior.

### Test Naming

```
Test_<Unit>_<Scenario>_<ExpectedResult>

FindUser_ExistingUser_ReturnsUser
FindUser_NonExistent_ReturnsNotFound
Parse_EmptyInput_ReturnsError
Transfer_InsufficientBalance_DoesNotModifyAccounts
```

Names should tell you **what broke** when they fail.

### Always Test Boundary Conditions

```
- Empty input / empty container
- Single element
- Max / min values / overflow boundaries
- Negative numbers / zero
- Duplicate values
- Race conditions under concurrency
- Error paths (not just the happy path)
```

## Preventing Flaky Tests

| Rule | Explanation |
|------|-------------|
| No `sleep` | Use condition variables / latch / future |
| Unique temp directories | Avoid inter-test file conflicts |
| No execution order dependency | Each test runs independently |
| No real time dependency | Inject controllable clock |
| No hidden global state | Reset in fixtures or eliminate entirely |

## Sanitizers with Testing

Debug builds **must** run tests with sanitizers:

| Sanitizer | Detects | Mutual Exclusion |
|-----------|---------|-----------------|
| ASan | Out-of-bounds, use-after-free, leaks | Cannot combine with TSan |
| UBSan | Undefined behavior (overflow, null ref, etc.) | Can combine with ASan |
| TSan | Data races | Cannot combine with ASan |
| MSan | Uninitialized memory reads | Clang only |

CI should run at least ASan + UBSan. Add TSan for concurrent code.

## Coverage

Coverage is a **necessary but insufficient** metric — 100% coverage doesn't mean no bugs, but 20% coverage means large swaths of code are untested.

Focus:
- Branch coverage is more meaningful than line coverage
- Core business logic > 80%
- Utility / glue code can be lower
- Don't write meaningless tests just to hit coverage targets

## Fuzz Testing (Optional)

Worthwhile when functions accept **external input** (parsers, protocol handlers, serialization):

```cpp
// libFuzzer entry point (framework-agnostic concept)
// Input: random byte stream
// Goal: no crashes, no sanitizer triggers
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto input = std::string_view(reinterpret_cast<const char*>(data), size);
    parse(input);  // Must not crash
    return 0;
}
```

## Test Review Checklist

- [ ] Tested happy path **and** error path
- [ ] Boundary conditions covered
- [ ] No `sleep` synchronization
- [ ] No shared global state
- [ ] Test names explain what failed
- [ ] Reasonable mock count (≤3; otherwise refactor the code under test)
- [ ] CI runs with sanitizers
