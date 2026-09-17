# C++20 Modules Reference

## Current Status

| Compiler | Named Modules | Header Units | Partitions |
|----------|--------------|-------------|------------|
| MSVC 19.28+ | Full | Full | Full |
| GCC 14+ | Usable | Experimental | Usable |
| Clang 16+ | Usable | Experimental | Usable |

| Build System | Support |
|-------------|---------|
| CMake 3.28+ | `FILE_SET CXX_MODULES` (requires Ninja ≥1.11) |
| CMake 3.25–3.27 | Experimental API |
| xmake | Early support, `add_rules("c++.modules")` |
| Meson | Experimental |
| Bazel | Limited |

**Strategy**: New projects can adopt; existing projects mix modules + includes, migrate gradually.

## Module Types

```
├── Named module interface unit (.cppm/.ixx)  — export declarations
├── Module implementation unit (.cpp)          — definitions, no export
├── Module partition (.cppm)                   — internal subdivision
└── Header units                               — bridge legacy headers
```

## Basic Syntax

### Named Module

```cpp
// math.cppm — interface unit
export module math;

export int add(int a, int b) { return a + b; }
export constexpr double pi = 3.14159;

int internal_helper() { return 42; }  // Module-private, not visible externally
```

```cpp
// main.cpp — consumer
import math;

int main() {
    auto result = add(2, 3);  // OK
    // internal_helper();     // Compile error — not exported
}
```

### Interface + Implementation Separation

```cpp
// math.cppm — interface
export module math;
export int add(int a, int b);

// math.cpp — implementation
module math;
int add(int a, int b) { return a + b; }
```

### Module Partitions

Split large modules internally while presenting a single module externally:

```cpp
// math-core.cppm — partition
export module math:core;
export int add(int a, int b) { return a + b; }

// math-types.cppm — partition
export module math:types;
export struct Vec2 { double x, y; };

// math.cppm — primary interface, aggregates partitions
export module math;
export import :core;   // Re-export partition
export import :types;
```

### Global Module Fragment — Bridging Macros & Legacy Headers

`import` **does not propagate macros**. When macros are needed, use the global module fragment:

```cpp
module;                    // Global module fragment begins
#include <cassert>         // Macros available here
#include <cjson/cJSON.h>   // C library headers

export module mymod;

export void check(int x) {
    assert(x > 0);         // Macro is available
}
```

### Header Units — Gradual Migration

```cpp
import <vector>;       // Standard library header unit
import <string>;
import "legacy.h";     // Project header as header unit

// Note: header units do NOT propagate macros
```

## Build System Configuration

### CMake 3.28+

```cmake
cmake_minimum_required(VERSION 3.28)
project(myproject LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

add_library(math)
target_sources(math
    PUBLIC
        FILE_SET CXX_MODULES FILES
            src/math.cppm
            src/math-core.cppm
    PRIVATE
        src/math-impl.cpp
)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE math)
```

Requires Ninja ≥1.11 or MSBuild as generator.

### xmake

```lua
target("math")
    set_kind("static")
    set_languages("c++20")
    add_rules("c++.modules")
    add_files("src/math.cppm", "src/math-core.cppm")
    add_files("src/math-impl.cpp")

target("myapp")
    set_kind("binary")
    add_deps("math")
    add_files("main.cpp")
```

### Manual Compilation (for debugging)

```bash
# Clang
clang++ -std=c++20 --precompile math.cppm -o math.pcm
clang++ -std=c++20 -fmodule-file=math=math.pcm -c math.cpp -o math.o
clang++ -std=c++20 -fmodule-file=math=math.pcm main.cpp math.o -o prog

# GCC (≥14)
g++ -std=c++20 -fmodules-ts math.cppm -c -o math.o
g++ -std=c++20 -fmodules-ts main.cpp math.o -o prog
```

## Wrapping C Libraries

```cpp
module;
#include <cjson/cJSON.h>

export module cjson;
export using ::cJSON;
export using ::cJSON_Parse;
export using ::cJSON_Delete;
```

## Common Errors

| Error | Cause | Solution |
|-------|-------|----------|
| `module 'X' not found` | BMI (.pcm/.gcm) missing | Compile interface unit first; check `-fmodule-file=` |
| `cannot import header in module` | `#include` inside module scope | Move to global module fragment or use `import` |
| `redefinition of module` | Multiple files declare same module | Only one primary interface unit per module |
| Macros unavailable | `import` doesn't propagate macros | Use global module fragment with `#include` |
| ODR violation | Duplicate exports across partitions | Export each name exactly once |
| Stale BMI cache | .pcm/.gcm not rebuilt | Clean build or verify dependency chain |

## Migration Strategy

1. **Don't migrate everything at once** — mixing modules + includes is the normal state
2. Start with leaf modules (no dependencies or fewest dependencies)
3. Convert stable public interfaces to modules first; keep internal implementation as includes
4. Wrap C libraries via global module fragment
5. Maintain CI builds with both module and non-module configurations until migration completes

## When to Use / When Not to

| Scenario | Recommendation |
|----------|---------------|
| New project, MSVC or latest GCC/Clang | Use named modules |
| Cross-platform, multiple compilers | Include-based primarily, pilot modules |
| Large legacy codebase | Gradually migrate leaf modules |
| Third-party lib doesn't support modules | Wrap via global module fragment or keep includes |
| Header-only library | Don't migrate yet (low benefit) |
