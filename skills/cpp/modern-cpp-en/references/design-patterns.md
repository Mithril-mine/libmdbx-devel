# Modern Design Patterns Reference

Traditional GoF patterns have lighter modern C++ alternatives. Prefer value semantics, function objects, and templates.

## variant + visit — Replaces Visitor Pattern

```cpp
// No base class needed, no accept()
using Expr = std::variant<Literal, BinOp, UnaryOp>;

int eval(const Expr& e) {
    return std::visit(overloaded{
        [](const Literal& l) { return l.value; },
        [](const BinOp& b)   { return eval(b.lhs) + eval(b.rhs); },
        [](const UnaryOp& u) { return -eval(u.operand); },
    }, e);
}

// overloaded helper (C++17 onwards)
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
```

Suitable: type set is known at compile time. Not suitable: plugin systems or runtime extension scenarios.

## Function Object / Lambda — Replaces Strategy Pattern

```cpp
template<typename Policy>
class Compressor {
    Policy compress_;
public:
    explicit Compressor(Policy p) : compress_(std::move(p)) {}

    std::vector<std::byte> run(std::span<const std::byte> data) {
        return compress_(data);
    }
};

auto c1 = Compressor([](auto data) { return zstd_compress(data); });
auto c2 = Compressor([](auto data) { return lz4_compress(data); });
```

## CRTP → Deducing this (C++23 Upgrade)

```cpp
// Old: CRTP (template-heavy, hard to read)
template<typename Derived>
struct Printable {
    void print() const {
        static_cast<const Derived*>(this)->to_string();
    }
};

// New: deducing this (clear and natural)
struct Printable {
    void print(this auto const& self) {
        std::print("{}\n", self.to_string());
    }
};

struct User : Printable {
    std::string name;
    std::string to_string() const { return "User:" + name; }
};
```

## Type Erasure — Runtime Polymorphism Without Inheritance

When you need runtime polymorphism but don't want to expose a base class / virtual functions:

```cpp
class AnyDrawable {
    struct Concept {
        virtual void draw() const = 0;
        virtual ~Concept() = default;
    };
    template<typename T>
    struct Model : Concept {
        T obj;
        Model(T o) : obj(std::move(o)) {}
        void draw() const override { obj.draw(); }
    };
    std::unique_ptr<Concept> impl_;
public:
    template<typename T>
    AnyDrawable(T obj) : impl_(std::make_unique<Model<T>>(std::move(obj))) {}

    void draw() const { impl_->draw(); }
};

// Any type with draw() can be stored — no inheritance required
Circle c; Rect r;
std::vector<AnyDrawable> shapes;
shapes.push_back(c);
shapes.push_back(r);
```

Use for: plugin interfaces, callback storage, heterogeneous containers. `std::function` is this pattern.

## Builder (Deducing this Version)

```cpp
struct QueryBuilder {
    std::string table_;
    std::string where_;
    int limit_ = -1;

    auto&& from(this auto&& self, std::string t) {
        self.table_ = std::move(t);
        return std::forward<decltype(self)>(self);
    }
    auto&& where(this auto&& self, std::string w) {
        self.where_ = std::move(w);
        return std::forward<decltype(self)>(self);
    }
    auto&& limit(this auto&& self, int n) {
        self.limit_ = n;
        return std::forward<decltype(self)>(self);
    }
    std::string build(this auto&& self) {
        return std::format("SELECT * FROM {} WHERE {} LIMIT {}",
            self.table_, self.where_, self.limit_);
    }
};

auto sql = QueryBuilder{}.from("users").where("age > 18").limit(10).build();
```

## Singleton — Avoid If Possible; When Necessary:

```cpp
// Meyer's Singleton — thread-safe (guaranteed since C++11)
class Config {
public:
    static Config& instance() {
        static Config cfg;
        return cfg;
    }
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
private:
    Config() { /* load from file */ }
};
```

Better alternative: construct in main(), pass by reference to modules. Singleton is implicit global state that impedes testing.

## Observer — Modern Lightweight Version

```cpp
template<typename... Args>
class Signal {
    std::vector<std::function<void(Args...)>> slots_;
public:
    void connect(std::function<void(Args...)> slot) {
        slots_.push_back(std::move(slot));
    }
    void emit(Args... args) const {
        for (auto& slot : slots_) slot(args...);
    }
};

Signal<int, std::string> on_message;
on_message.connect([](int id, std::string msg) {
    std::print("#{}: {}\n", id, msg);
});
on_message.emit(42, "hello");
```

## Pattern Selection Quick Reference

| Need | Legacy Pattern | Modern Alternative |
|------|---------------|-------------------|
| Polymorphic dispatch | virtual + inheritance | `variant` + `visit` |
| Algorithm substitution | Strategy base class | Template parameter / lambda |
| Mixin reuse | CRTP | `deducing this` |
| Runtime polymorphism without inheritance | — | Type Erasure |
| Fluent construction | Builder (many overloads) | `deducing this` Builder |
| Event notification | Observer base class | `Signal<Args...>` |
| Global unique | Singleton | Construct + inject by reference |
| Type adaptation | Adapter inheritance | Lambda / function wrapper |
| Object creation | Factory virtual | `variant` / `expected` + function |
| Decorator | Decorator inheritance | Ranges pipeline / function composition |
