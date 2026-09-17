# Smart Pointers In-Depth Reference

## Ownership Model

```
unique_ptr  — Exclusive ownership (default choice)
shared_ptr  — Shared ownership (reference counted)
weak_ptr    — Non-owning observer (breaks cycles)
Raw pointer — Borrowing only, does not express ownership
```

## unique_ptr Advanced

### Custom Deleters

```cpp
// Wrapping C resources
auto file = std::unique_ptr<FILE, decltype(&fclose)>(
    fopen("data.txt", "r"), &fclose
);

// Lambda deleter (more flexible)
auto conn = std::unique_ptr<Connection, std::function<void(Connection*)>>(
    db_connect(uri),
    [](Connection* c) { db_disconnect(c); db_free(c); }
);

// Stateless deleter (zero overhead — same size as raw pointer)
struct FileDeleter {
    void operator()(FILE* f) const { if (f != nullptr) fclose(f); }
};
using FilePtr = std::unique_ptr<FILE, FileDeleter>;
```

### unique_ptr for Type Erasure

`unique_ptr<Concept>` is the core building block of the Type Erasure pattern — see `design-patterns.md` for the full pattern and code. `std::function` is a standard library instance of this pattern.

### Factory Functions Returning unique_ptr

```cpp
// Standard return type for factory functions
[[nodiscard]] std::unique_ptr<Shape> create_shape(ShapeType type) {
    switch (type) {
        case ShapeType::Circle: return std::make_unique<Circle>();
        case ShapeType::Rect:   return std::make_unique<Rect>();
    }
    std::unreachable();
}

// Caller can implicitly convert to shared_ptr
std::shared_ptr<Shape> shared = create_shape(ShapeType::Circle);
```

## shared_ptr Considerations

### enable_shared_from_this

```cpp
// When an object needs to hand out shared_ptr to itself
class Session : public std::enable_shared_from_this<Session> {
public:
    void start() {
        // Correct — creates new shared_ptr from existing one
        auto self = shared_from_this();
        async_read([self](auto data) { self->process(data); });
    }
};

// Must be managed by shared_ptr to call shared_from_this()
auto session = std::make_shared<Session>();
session->start();  // OK

// Session s; s.start();  // UB — not managed by shared_ptr
```

### weak_ptr Breaking Circular References

```cpp
struct Node {
    std::shared_ptr<Node> next;
    std::weak_ptr<Node> prev;  // weak breaks cycle, otherwise never freed

    void use_prev() {
        if (auto p = prev.lock(); p != nullptr) {
            // p is a valid shared_ptr
        }
    }
};
```

### weak_ptr as Cache

```cpp
class TextureCache {
    std::unordered_map<std::string, std::weak_ptr<Texture>> cache_;

public:
    std::shared_ptr<Texture> get(const std::string& path) {
        if (auto it = cache_.find(path); it != cache_.end()) {
            if (auto tex = it->second.lock(); tex != nullptr) {
                return tex;  // Cache hit
            }
        }
        auto tex = std::make_shared<Texture>(load_from_disk(path));
        cache_[path] = tex;
        return tex;
    }
};
```

## Common Pitfalls

```cpp
// 1. Creating multiple shared_ptr from raw pointer — double free
auto* raw = new Foo();
auto sp1 = std::shared_ptr<Foo>(raw);
auto sp2 = std::shared_ptr<Foo>(raw);  // UB — double delete

// Correct: create once, then copy
auto sp1 = std::make_shared<Foo>();
auto sp2 = sp1;

// 2. shared_ptr circular reference — memory leak
// Solution: use weak_ptr on one side

// 3. Calling shared_from_this() in constructor — UB
// Solution: use factory function + two-phase init

// 4. make_shared incompatible with custom deleters
// make_shared allocates object + control block together; custom deleters need the constructor
auto p = std::shared_ptr<FILE>(fopen("f", "r"), &fclose);  // Can't use make_shared
```

## Selection Guide

| Scenario | Use |
|----------|-----|
| Default, exclusive ownership | `unique_ptr` |
| Ownership needs sharing | `shared_ptr` (use sparingly) |
| Observe without owning | `weak_ptr` or raw pointer |
| Cache | `weak_ptr` |
| C API wrapping | `unique_ptr` + custom deleter |
| Function parameter, no ownership | Raw pointer or reference |
| Ownership transfer parameter | `unique_ptr` by value |
