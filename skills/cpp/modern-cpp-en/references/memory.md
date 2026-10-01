# Memory Management Reference

## Allocator Hierarchy

```
std::allocator (default)
    ↓ when insufficient
std::pmr::polymorphic_allocator (runtime-swappable)
    ↓ performance-critical
Custom arena / pool allocator
```

## PMR (Polymorphic Memory Resource)

Standard library allocator framework with **runtime-swappable** memory sources, no template parameters needed:

```cpp
#include <memory_resource>

// Stack buffer → monotonic allocation (allocate only, never deallocate — fastest)
std::array<std::byte, 4096> buf;
std::pmr::monotonic_buffer_resource pool(buf.data(), buf.size());

// Use pmr containers
std::pmr::vector<int> vec(&pool);
vec.reserve(100);
for (int i = 0; i < 100; ++i) {
    vec.push_back(i);  // Allocates from stack buf, zero malloc
}
```

### PMR Memory Resource Types

| Resource | Characteristics | Use Case |
|----------|----------------|----------|
| `monotonic_buffer_resource` | Allocate only, bulk reclaim on destruction | Frame allocation, request processing |
| `unsynchronized_pool_resource` | Fixed-size block pool, single-threaded | Frequent alloc/dealloc of same-size objects |
| `synchronized_pool_resource` | Same, thread-safe | Multi-threaded scenarios |
| `null_memory_resource` | Always throws | Testing, ensuring no allocation |

### Nested Container Propagation

```cpp
std::pmr::monotonic_buffer_resource pool(1024 * 1024);

// String's internal allocation also goes through pool
std::pmr::vector<std::pmr::string> names(&pool);
names.emplace_back("alice");  // String content allocated from pool
```

### Chained Fallback

```cpp
std::array<std::byte, 1024> stack_buf;
std::pmr::monotonic_buffer_resource stack_pool(stack_buf.data(), stack_buf.size());

// Fall back to heap when stack buffer is exhausted
std::pmr::monotonic_buffer_resource pool(
    1024 * 1024,
    std::pmr::new_delete_resource()  // upstream fallback
);
```

## Arena Allocator Concept

```cpp
// Core idea: bulk pre-allocate, linear allocate, bulk reclaim
class Arena {
    std::vector<std::byte> buf_;
    std::size_t offset_ = 0;

public:
    explicit Arena(std::size_t size) : buf_(size) {}

    void* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
        auto aligned = (offset_ + align - 1) & ~(align - 1);
        if (aligned + bytes > buf_.size()) throw std::bad_alloc();
        offset_ = aligned + bytes;
        return buf_.data() + aligned;
    }

    void reset() { offset_ = 0; }  // Bulk "free" everything
    // Individual deallocate is a no-op
};
```

Use cases: game frame allocation, request processing, compiler AST nodes.

## SSO (Small String Optimization)

```cpp
// Most std::string implementations store small strings inline (typically 15-22 bytes)
std::string s1 = "hello";          // On stack, no heap allocation
std::string s2 = "this is a much longer string";  // Heap allocation

// Key insights:
// - Short strings (< ~22 bytes) are essentially "free"
// - std::string move for short strings is as fast as copy (both memcpy stack data)
// - If you need many small strings, std::string is already efficient
```

## Memory Alignment

```cpp
// alignas — specify alignment requirements
struct alignas(64) CacheLine {  // Align to cache line
    int data[16];
};

// Avoid false sharing
struct alignas(64) Counter {
    std::atomic<int> value{0};
    // Padding automatically fills to 64 bytes
};
std::array<Counter, 4> counters;  // Each on a different cache line

// Aligned allocation
auto* p = static_cast<CacheLine*>(
    std::aligned_alloc(alignof(CacheLine), sizeof(CacheLine))
);
// C++17: new automatically respects alignas
auto* p2 = new CacheLine;  // Automatically 64-byte aligned
```

## When to Care About Memory Management

| Scenario | Strategy |
|----------|----------|
| General business code | `std::allocator` is sufficient — don't optimize prematurely |
| Frame/request lifetime | `pmr::monotonic_buffer_resource` |
| Frequent alloc/dealloc of same-size objects | `pmr::pool_resource` |
| Ultra-low latency | Custom arena + pre-allocation |
| Embedded / no heap | `pmr` + `null_memory_resource` as upstream guard |
| Multi-threaded shared data | `alignas(64)` to avoid false sharing |
