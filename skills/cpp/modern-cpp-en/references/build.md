# Build Acceleration & Project Configuration Reference

## PCH (Precompiled Headers)

| Build System | Configuration |
|-------------|---------------|
| CMake | `target_precompile_headers(target PRIVATE <vector> <string>)` |
| Meson | `pch: 'pch/pch.h'` |
| xmake | `set_pcheader("pch.h")` |
| Bazel | `cc_library(copts=["-include", "pch.h"])` |

Only include STL / third-party headers — never project headers.

## Ninja + ccache

```bash
# CMake
cmake -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=ccache ..
# Meson (Ninja by default)
meson setup build
# xmake
xmake f --ccache=y
```

## Unity Build (2-5x improvement)

| Build System | Configuration |
|-------------|---------------|
| CMake | `set_target_properties(target PROPERTIES UNITY_BUILD ON)` |
| Meson | `unity: 'on'` |
| xmake | `add_rules("c++.unity_build")` |

## Reduce Includes

- Use forward declarations in headers; include in .cpp files only
- IWYU (include-what-you-use) for automatic cleanup

## Architectural

- Split targets: core / utils / feature / app
- Upper layers don't include lower layer implementations
- Use pimpl at module boundaries for isolation

## Modules

MSVC usable; GCC/Clang progressing. xmake has early support. Current: mix modules + includes.

---

## Project Configuration Templates

### CMake

```cmake
target_compile_options(target PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -g>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
)
target_link_options(target PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
```

### Meson

```meson
project('app', 'cpp', default_options: ['cpp_std=c++23', 'warning_level=3', 'werror=true'])
if get_option('buildtype') == 'debug'
  add_project_arguments('-fsanitize=address,undefined', language: 'cpp')
  add_project_link_arguments('-fsanitize=address,undefined', language: 'cpp')
endif
```

### xmake

```lua
set_languages("c++23")
set_warnings("all", "extra", "pedantic")
set_policy("build.warning", true)
if is_mode("debug") then
    add_cxflags("-fsanitize=address,undefined")
    add_ldflags("-fsanitize=address,undefined")
end
```

### General Principles

- Debug must enable ASan + UBSan
- Release uses `-O2` (`-O3` has marginal gains and occasional issues)
- Always `-Wall -Wextra -Werror`
- CI adds `-Wconversion -Wshadow`
