# Project Architecture Reference

## Typical Directory Structure

```
project/
├── src/                  # Source code
│   ├── core/             # Core business logic (pure logic, no IO)
│   ├── infra/            # Infrastructure (network, database, filesystem)
│   ├── api/              # External interface layer
│   └── main.cpp
├── include/project/      # Public headers (externally visible)
├── tests/
│   ├── unit/
│   └── integration/
├── benchmarks/
├── third_party/ or deps/
├── tools/                # Scripts, code generators
└── CMakeLists.txt / meson.build / xmake.lua
```

## Layering Principles

```
┌─────────────┐
│    api       │  ← External interface, type conversion, serialization
├─────────────┤
│   service    │  ← Business orchestration, combines core logic
├─────────────┤
│    core      │  ← Pure business logic, no side effects, testable
├─────────────┤
│   infra      │  ← IO / DB / network / filesystem
├─────────────┤
│   common     │  ← Type definitions, utilities, error types
└─────────────┘
```

**Dependencies strictly flow downward**: upper layers depend on lower layers; lower layers are unaware of upper layers.
**Core does not depend on infra**: invert via interfaces (concept / function object / template).

## Dependency Inversion Example

```cpp
// core/order_service.hpp — no dependency on concrete database
template<typename Repository>
concept OrderRepo = requires(Repository r, int id) {
    { r.find(id) } -> std::same_as<std::expected<Order, Error>>;
    { r.save(Order{}) } -> std::same_as<std::expected<void, Error>>;
};

class OrderService {
    std::function<std::expected<Order, Error>(int)> find_;
    std::function<std::expected<void, Error>(Order)> save_;
public:
    OrderService(auto find, auto save) : find_(find), save_(save) {}

    std::expected<Order, Error> process(int id) {
        return find_(id).transform([](Order o) {
            o.status = Status::Processed;
            return o;
        });
    }
};

// main.cpp — assembly
auto repo = PgRepository(conn_string);
auto svc = OrderService(
    [&](int id) { return repo.find(id); },
    [&](Order o) { return repo.save(o); }
);
```

## Build Target Splitting

Each layer is an independent target with explicit dependencies:

```
common  ← no dependencies
core    ← common
infra   ← common
service ← core, infra
api     ← service
app     ← api
tests   ← core, service (test directly, bypassing api)
```

Benefit: changing infra doesn't rebuild core; changing core doesn't rebuild infra.

## Header Management

```cpp
// Public header (include/project/order.hpp)
// Expose only interfaces, use forward declarations
#pragma once
#include <expected>
#include <string>

namespace project {
class Order;  // Forward declaration
std::expected<Order, std::string> create_order(int user_id);
}

// Private header (src/core/order_impl.hpp)
// Implementation details; heavy includes go here
#include <vector>
#include <map>
#include "project/order.hpp"
```
