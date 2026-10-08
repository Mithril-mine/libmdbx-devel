# ABI Stability Reference

## What Is the ABI Problem

ABI (Application Binary Interface) determines how compiled binaries interact. Breaking ABI = recompile all dependents.

**ABI includes**: function signature mangling, class layout (member offsets, vtable), exception passing, calling conventions.

## Common ABI-Breaking Operations

| Operation | Breaks ABI? | Reason |
|-----------|------------|--------|
| Add public member variable | Yes | Changes `sizeof` and member offsets |
| Add virtual function | Yes | Changes vtable layout |
| Reorder virtual functions | Yes | vtable indices change |
| Remove/rename public member | Yes | Symbol disappears |
| Add non-virtual member function | No | Doesn't affect layout |
| Modify function implementation | No | Binary compatible |
| Add enum value (at end) | Usually safe | Doesn't change existing values |
| Change default parameter value | No (runtime) | Default params compiled at call site |

## Pimpl — ABI Firewall

```cpp
// widget.hpp — public header (ABI stable)
#pragma once
#include <memory>

class Widget {
public:
    Widget();
    ~Widget();
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;

    void do_stuff();
    int get_value() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;  // Fixed size: one pointer
};

// widget.cpp — implementation (change freely, no ABI impact)
#include "widget.hpp"
#include <vector>
#include <string>

struct Widget::Impl {
    std::vector<int> data;   // Adding members doesn't break ABI
    std::string name;
    int cached_value = 0;

    void compute() { cached_value = /* ... */ 42; }
};

Widget::Widget() : impl_(std::make_unique<Impl>()) {}
Widget::~Widget() = default;
Widget::Widget(Widget&&) noexcept = default;
Widget& Widget::operator=(Widget&&) noexcept = default;

void Widget::do_stuff() { impl_->compute(); }
int Widget::get_value() const { return impl_->cached_value; }
```

### Pimpl Costs

- One extra indirection per call (pointer dereference)
- Heap allocation (mitigable with PMR)
- Cannot inline Impl methods

**Suitable for**: library public interfaces, compilation firewalls, plugin boundaries.
**Not suitable for**: performance-critical internal classes, header-only libraries.

## inline namespace Versioning

```cpp
namespace mylib {

// Current version (callers use this by default)
inline namespace v2 {
    struct Config {
        std::string host;
        int port;
        int timeout;  // Added in v2
    };

    void connect(const Config& cfg);
}

// Old version still accessible (backward compatible)
namespace v1 {
    struct Config {
        std::string host;
        int port;
    };

    void connect(const Config& cfg);
}

}  // namespace mylib

// User code
mylib::Config cfg;         // Automatically uses v2::Config
mylib::v1::Config old_cfg; // Explicitly use old version
```

## Symbol Visibility

```cpp
// Default: hide all symbols, only export marked ones
// Compile flag: -fvisibility=hidden

#if defined(_WIN32)
    #define MY_API __declspec(dllexport)
#else
    #define MY_API __attribute__((visibility("default")))
#endif

class MY_API PublicClass {  // Exported
    // ...
};

class InternalClass {       // Hidden (default)
    // ...
};
```

Benefits:
- Smaller symbol table → faster linking and loading
- Prevents internal symbols from being accidentally depended upon
- Better optimization opportunities (compiler knows symbols won't be overridden)

## Library Author Checklist

- [ ] Public headers contain only stable forward declarations and pure interfaces
- [ ] Pimpl isolates implementation that may change
- [ ] Virtual functions not added carelessly (changing vtable = breaking ABI)
- [ ] inline namespace for version management
- [ ] `-fvisibility=hidden` + explicit exports
- [ ] Don't expose STL types in public headers (different compilers/versions have different STL ABIs)
- [ ] Consider C interface as the most stable ABI boundary
