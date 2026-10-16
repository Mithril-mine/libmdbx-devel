cmake_minimum_required(VERSION 3.14)

# Special-build test configuration for the System V IPC locking
# (MDBX_LOCKING=5). See the comment in tests/special-builds/README.md and the
# canary test sources for the rationale.
project(libmdbx-locking-sysv C)
enable_testing()

set(LIBMDBX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Path to the libmdbx repository root")

# The library must be built from this repository with the System V IPC locking
# enabled and nothing else pulled in (no tests/tools/C++).
set(MDBX_LOCKING 5 CACHE STRING "Locking method" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(MDBX_ENABLE_TESTS OFF CACHE BOOL "Build MDBX tests" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "Build MDBX tools" FORCE)
set(MDBX_BUILD_CXX OFF CACHE BOOL "Build MDBX C++ API" FORCE)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build shared library" FORCE)
set(MDBX_INSTALL_MANPAGES OFF CACHE BOOL "Install man-pages" FORCE)
set(MDBX_BUILD_MCP_MEMORY OFF CACHE BOOL "Build MCP shared-graph-memory" FORCE)

add_subdirectory(${LIBMDBX_SOURCE_DIR} mdbx-sdk)

add_executable(issue_gh0080_ftok issue_gh0080_ftok.c)
target_include_directories(issue_gh0080_ftok PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0080_ftok PRIVATE mdbx-static)
add_test(NAME issue_gh0080_ftok COMMAND issue_gh0080_ftok)

# Canary for the deliberately-unfixed collision defect (issue #80): the
# database survives only when the defect is gone, so the expected result is a
# failure. Convert it into a normal test once the defect is fixed.
add_executable(issue_gh0080_collision issue_gh0080_collision.c)
target_include_directories(issue_gh0080_collision PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0080_collision PRIVATE mdbx-static)
add_test(NAME issue_gh0080_collision COMMAND issue_gh0080_collision)
set_tests_properties(issue_gh0080_collision PROPERTIES WILL_FAIL TRUE)