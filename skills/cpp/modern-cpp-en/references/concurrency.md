# Concurrency Reference

## Lightweight Synchronization: atomic wait/notify

```cpp
std::atomic<int> flag{0};

// Waiting side
flag.wait(0);  // Blocks until value changes

// Notifying side
flag.store(1);
flag.notify_one();
```

Lighter than mutex + condition_variable; suitable for simple signaling scenarios.

## Correct Lock Usage

```cpp
// Single lock — lock_guard
{
    std::lock_guard lock(mutex_);
    data_.push_back(value);
}  // Auto-unlocks

// Multiple locks — scoped_lock (deadlock-free)
void transfer(Account& from, Account& to, double amount) {
    std::scoped_lock lock(from.mtx_, to.mtx_);  // Locks both, no deadlock risk
    from.balance_ -= amount;
    to.balance_ += amount;
}

// Manual control needed — unique_lock
std::unique_lock lock(mutex_);
cv_.wait(lock, [&] { return queue_.empty() == false; });
```

Rule: **never manually `lock()` / `unlock()`** — always use RAII.

## Coroutines (C++20)

```cpp
task<string> fetch_data(string url) {
    auto response = co_await http_get(url);
    co_return response.body;
}
```

## std::execution (P2300) — Sender/Receiver Model

The most important concurrency feature in C++26. Core idea: **describe "what to do", not "how to do it"**.

**Three core concepts:**

| Concept | Role | Analogy |
|---------|------|---------|
| Sender | Describes an async operation (lazy, not immediately executed) | Producer side of Promise/Future |
| Receiver | Receives operation result (value / error / stopped) | Type-safe version of callbacks |
| Scheduler | Decides where to execute (thread pool / GPU / IO thread) | Executor |

**Fundamental difference from ASIO:**

```cpp
// ASIO — callback-driven: "call me when done"
socket.async_read(buf, [](error_code ec, size_t n) {
    if (ec == error_code{}) async_write(buf, [](error_code ec2, size_t) { /*...*/ });
});

// stdexec — dataflow-driven: "read, then write"
auto work = just(socket)
    | let_value([](auto& s) { return async_read(s); })
    | then([](auto data) { return process(data); })
    | let_value([&](auto result) { return async_write(socket, result); });

sync_wait(on(pool.get_scheduler(), std::move(work)));
```

**Core sender algorithms (stdexec library):**

```cpp
#include <stdexec/execution.hpp>
namespace ex = stdexec;

// just — immediately produce a value
auto s1 = ex::just(42);

// then — transform the value (like transform)
auto s2 = ex::just(3, 4) | ex::then([](int a, int b) { return a + b; });

// let_value — return a new sender (like and_then / flatmap)
auto s3 = ex::just(42) | ex::let_value([](int x) {
    return ex::just(x * 2);
});

// when_all — execute senders in parallel, merge results when all complete
auto s4 = ex::when_all(
    ex::just(1) | ex::then([](int x) { return x + 10; }),
    ex::just(2) | ex::then([](int x) { return x + 20; })
);
// Result: (11, 22)

// on — specify which scheduler to execute on
auto s5 = ex::on(pool.get_scheduler(),
    ex::just() | ex::then([] { return heavy_compute(); })
);

// sync_wait — block until sender completes (entry point)
auto [result] = ex::sync_wait(s2).value();
```

**Error handling — three-channel model:**

Every sender can produce three signals:
- `set_value` — success, carries result
- `set_error` — failure, carries error
- `set_stopped` — cancelled

```cpp
auto work = ex::just(input)
    | ex::then([](auto x) -> int {
        if (x < 0) throw std::runtime_error("negative");
        return x * 2;
    })
    | ex::upon_error([](auto err) {
        log_error(err);
        return 0;  // fallback
    })
    | ex::upon_stopped([] {
        return -1;  // cancelled
    });
```

**Practical example: thread pool + parallel tasks:**

```cpp
exec::static_thread_pool pool(4);
auto sched = pool.get_scheduler();

auto pipeline = ex::when_all(
    ex::on(sched, ex::just() | ex::then([] { return fetch_users(); })),
    ex::on(sched, ex::just() | ex::then([] { return fetch_orders(); }))
) | ex::then([](auto users, auto orders) {
    return merge(users, orders);
});

auto [merged] = ex::sync_wait(std::move(pipeline)).value();
```

**Current implementations:**

| Implementation | Status | Notes |
|---------------|--------|-------|
| [stdexec](https://github.com/NVIDIA/stdexec) | Reference implementation | Maintained by NVIDIA, most complete |
| libunifex | Maintained by Meta | Predecessor to stdexec, gradually migrating |
| Boost.Asio | Partially aligned | `co_await` as transition path |

**Current strategy:**
- New projects: use stdexec library directly (header-only; CMake/Meson/Bazel integrable)
- Legacy ASIO projects: transition via `co_await` + `use_awaitable`
- Track GCC/Clang/MSVC `<execution>` standard library landing progress
