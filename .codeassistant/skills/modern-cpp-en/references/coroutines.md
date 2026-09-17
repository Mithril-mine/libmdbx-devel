# Coroutines In-Depth Reference

## Core Mechanism

C++20 coroutines are a **compiler transformation** — the compiler splits a coroutine function into a state machine.

```
Coroutine function
    ↓ compiler transform
Coroutine frame (heap-allocated) + state machine
    ↓ contains
promise_type → controls coroutine behavior
    ↓ returns
Coroutine return type (task / generator / ...)
    ↓ via
co_await / co_yield / co_return to suspend/resume
```

### Three Keywords

| Keyword | Purpose |
|---------|---------|
| `co_await expr` | Suspend, wait for awaitable to complete, then resume |
| `co_yield expr` | Suspend, produce a value (sugar for `co_await promise.yield_value(expr)`) |
| `co_return expr` | End coroutine, return final value |

## Custom task Type

Minimal working task — understand the principles, then use a library (cppcoro, stdexec):

```cpp
#include <coroutine>
#include <optional>

template<typename T>
class Task {
public:
    struct promise_type {
        std::optional<T> result;
        std::exception_ptr exception;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }  // Lazy start
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T value) { result = std::move(value); }
        void unhandled_exception() { exception = std::current_exception(); }
    };

private:
    std::coroutine_handle<promise_type> handle_;

public:
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    ~Task() { if (handle_) handle_.destroy(); }

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&&) = delete;
    Task(const Task&) = delete;

    // Manual drive (in practice, use an executor/scheduler)
    T get() {
        handle_.resume();
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
        return std::move(*handle_.promise().result);
    }
};

// Usage
Task<int> compute() {
    co_return 42;
}

int main() {
    auto task = compute();
    auto result = task.get();  // 42
}
```

## Generator

```cpp
#include <coroutine>

template<typename T>
class Generator {
public:
    struct promise_type {
        T current;

        Generator get_return_object() {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T value) {
            current = std::move(value);
            return {};
        }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    // Iterator support (works with range-for)
    struct iterator {
        std::coroutine_handle<promise_type> handle;

        iterator& operator++() { handle.resume(); return *this; }
        T& operator*() { return handle.promise().current; }
        bool operator==(std::default_sentinel_t) const { return handle.done(); }
    };

    iterator begin() { handle_.resume(); return {handle_}; }
    std::default_sentinel_t end() { return {}; }

private:
    std::coroutine_handle<promise_type> handle_;
    explicit Generator(std::coroutine_handle<promise_type> h) : handle_(h) {}
public:
    ~Generator() { if (handle_) handle_.destroy(); }
    Generator(Generator&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
};

// C++23: use std::generator directly
Generator<int> fibonacci() {
    int a = 0, b = 1;
    while (true) {
        co_yield a;
        auto next = a + b;
        a = b;
        b = next;
    }
}

// Usage
for (auto val : fibonacci()) {
    if (val > 1000) break;
    std::print("{} ", val);
}
```

## co_await Mechanism

`co_await expr` requires expr to be an **awaitable** with three methods:

```cpp
struct MyAwaitable {
    bool await_ready() { return false; }        // true = don't suspend
    void await_suspend(std::coroutine_handle<> h) {
        // What to do after suspending (e.g., submit to thread pool, register IO callback)
        // Options: h.resume() to resume immediately, save h for later, return another handle
    }
    T await_resume() { return result; }         // Return value upon resumption
};
```

### Practical: Switch to Thread Pool

```cpp
struct SwitchToThread {
    bool await_ready() { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        std::jthread([h] { h.resume(); }).detach();
    }
    void await_resume() {}
};

Task<int> work() {
    // Starts on caller's thread
    co_await SwitchToThread{};
    // Now on a new thread
    co_return heavy_compute();
}
```

## Coroutine Heap Allocation Optimization

The compiler may elide heap allocation (HALO — Heap Allocation eLision Optimization), but it's not guaranteed.

```cpp
struct promise_type {
    // Custom operator new to control allocation
    void* operator new(std::size_t size) {
        return my_pool.allocate(size);
    }
    void operator delete(void* ptr, std::size_t size) {
        my_pool.deallocate(ptr, size);
    }
};
```

## Current Ecosystem

| Library | Provides | Status |
|---------|----------|--------|
| `std::generator` | Synchronous generator | C++23 standard |
| cppcoro | task, generator, async_generator | Community maintained |
| stdexec | sender/receiver (not coroutines but interoperable) | NVIDIA maintained |
| Boost.Asio | co_await integration | Production ready |

**Recommendation**: C++23 use `std::generator`; async tasks use Boost.Asio or cppcoro; new projects evaluate stdexec.
