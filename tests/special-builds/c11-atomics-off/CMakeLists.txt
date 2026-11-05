cmake_minimum_required(VERSION 3.14)

# Special-build test configuration for the non-C11 atomic fallbacks:
# the library is built with C11 atomics disabled (via __STDC_NO_ATOMICS__),
# so all atomic-like operations run the emulated paths with the fallback
# fences. See the comment in tests/special-builds/README.md and
# https://github.com/Mithril-mine/libmdbx-devel/issues/361 for the rationale.
project(libmdbx-c11-atomics-off C)
enable_testing()

set(LIBMDBX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Path to the libmdbx repository root")

# The library must be built from this repository with C11 atomics disabled,
# and nothing else pulled in (no tests/tools/C++).
add_compile_options(-D__STDC_NO_ATOMICS__)
set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(MDBX_ENABLE_TESTS OFF CACHE BOOL "Build MDBX tests" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "Build MDBX tools" FORCE)
set(MDBX_BUILD_CXX OFF CACHE BOOL "Build MDBX C++ API" FORCE)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build shared library" FORCE)
set(MDBX_INSTALL_MANPAGES OFF CACHE BOOL "Install man-pages" FORCE)
set(MDBX_BUILD_MCP_MEMORY OFF CACHE BOOL "Build MCP shared-graph-memory" FORCE)

add_subdirectory(${LIBMDBX_SOURCE_DIR} mdbx-sdk)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Regression test for issue #361: with C11 atomics disabled, the emulated
# atomic loads must apply the acquire fence after the read (and the emulated
# stores the release fence before the write), otherwise a reader can observe
# a published sequence number with a stale payload on weakly-ordered CPUs.
add_executable(issue_gh0361_publication issue_gh0361_publication.c)
target_include_directories(issue_gh0361_publication PRIVATE ${LIBMDBX_SOURCE_DIR}/src)
target_link_libraries(issue_gh0361_publication PRIVATE mdbx-static Threads::Threads)
add_test(NAME issue_gh0361_publication COMMAND issue_gh0361_publication)
set_tests_properties(issue_gh0361_publication PROPERTIES TIMEOUT 120)
