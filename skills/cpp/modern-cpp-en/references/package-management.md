# Package Management Reference

## Four Major Approaches Compared

| Tool | Mode | Strengths | Weaknesses |
|------|------|-----------|------------|
| **vcpkg** | Manifest (`vcpkg.json`) | Microsoft-maintained, large package count, excellent CMake integration | Classic/global mode causes conflicts (use manifest) |
| **Conan** | `conanfile.py` / `conanfile.txt` | Flexible, multi-build-system support, strong version management | Slightly steeper learning curve |
| **FetchContent** | CMake built-in | Zero extra tools, source integration | CMake only; large deps compile slowly |
| **xmake** | `add_requires()` | Built-in package management, cleanest syntax, interops with vcpkg/Conan | Smaller ecosystem |

**Selection guide**: Already using CMake → vcpkg manifest or FetchContent; multi-build-system needs → Conan; small deps → FetchContent is simplest.

## vcpkg (Manifest Mode)

```json
// vcpkg.json
{
    "name": "myproject",
    "version-string": "1.0.0",
    "dependencies": [
        "fmt",
        "spdlog",
        { "name": "boost-asio", "version>=": "1.83.0" }
    ]
}
```

```cmake
# CMakeLists.txt — set toolchain
cmake_minimum_required(VERSION 3.20)
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Vcpkg toolchain file")
project(myproject LANGUAGES CXX)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE fmt::fmt spdlog::spdlog)
```

```bash
# Dependencies installed automatically
cmake -B build -S .
# vcpkg reads vcpkg.json and installs during configure
```

## Conan

```ini
# conanfile.txt
[requires]
fmt/10.2.1
spdlog/1.13.0
boost/1.83.0

[generators]
CMakeDeps
CMakeToolchain

[layout]
cmake_layout
```

```bash
conan install . --output-folder=build --build=missing
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake
cmake --build build
```

```python
# conanfile.py — more flexible
from conan import ConanFile

class MyProject(ConanFile):
    requires = "fmt/10.2.1", "spdlog/1.13.0"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        if self.settings.os == "Windows":
            self.requires("wil/1.0.230629.1")
```

## FetchContent (CMake Built-in)

```cmake
include(FetchContent)

FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)

FetchContent_MakeAvailable(fmt json)

target_link_libraries(myapp PRIVATE fmt::fmt nlohmann_json::nlohmann_json)
```

Suitable for small dependencies. Large deps (Boost, LLVM) are too slow to compile via FetchContent.

## xmake Package Management

```lua
add_requires("fmt 10.2.1", "spdlog 1.13.0")
add_requires("boost", {configs = {asio = true}})

target("myapp")
    set_kind("binary")
    add_packages("fmt", "spdlog", "boost")
    add_files("src/*.cpp")
```

xmake has its own package repository and can also interface with vcpkg and Conan.

## Best Practices

- **Pin versions**: Lock dependency versions in production; never use `latest` or `*`
- **Manifest mode**: Use vcpkg's `vcpkg.json`, not global installs
- **CI caching**: Cache vcpkg/Conan downloads and build artifacts (`~/.cache/vcpkg`, `~/.conan2`)
- **Minimal dependencies**: If the standard library can solve it, don't introduce a third-party lib
- **Security audits**: Regularly check dependencies for CVEs (`conan audit`, vcpkg port versions)
- **Vendoring strategy**: Consider vendoring critical deps (FetchContent + pinned hash)
