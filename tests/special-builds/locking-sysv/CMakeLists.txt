cmake_minimum_required(VERSION 3.14)

# Special-build test configuration for the System V IPC locking
# (MDBX_LOCKING=5). See the comment in tests/special-builds/README.md and the
# canary test sources for the rationale.
project(libmdbx-locking-sysv C)
enable_testing()

set(LIBMDBX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Path to the libmdbx repository root")

# The library must be built from this repository with the System V IPC locking
# enabled, the probe-bus enabled (the collision test injects a colliding key
# via the ftok_key_collide FAULT site), and nothing else pulled in
# (no tests/tools/C++).
set(MDBX_LOCKING 5 CACHE STRING "Locking method" FORCE)
set(MDBX_PROBES ON CACHE BOOL "Enable probe-bus instrumentation" FORCE)
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

# Regression test for the ftok() key collision defect (issue #80): a colliding
# open of a live database must be refused instead of reclaiming (removing) the
# live semaphore set. The collision is injected via the ftok_key_collide FAULT
# site, so the probe-bus must be active at run time (MDBX_PROBES=1).
add_executable(issue_gh0080_collision issue_gh0080_collision.c)
target_include_directories(issue_gh0080_collision PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0080_collision PRIVATE mdbx-static)
add_test(NAME issue_gh0080_collision COMMAND issue_gh0080_collision)
set_tests_properties(issue_gh0080_collision PROPERTIES ENVIRONMENT "MDBX_PROBES=1")

# Regression test for the reclaim path of the same defect: reopening a database
# whose semaphore set was left behind by a crashed owner must reclaim the stale
# set and succeed (otherwise a legitimate reopen would be a denial of service).
add_executable(issue_gh0080_reclaim issue_gh0080_reclaim.c)
target_include_directories(issue_gh0080_reclaim PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0080_reclaim PRIVATE mdbx-static)
add_test(NAME issue_gh0080_reclaim COMMAND issue_gh0080_reclaim)
set_tests_properties(issue_gh0080_reclaim PROPERTIES ENVIRONMENT "MDBX_PROBES=1")

# Regression test for issue #159: a database that is open but idle, whose last
# semop() owner process is dead, must not lose its semaphore set to a colliding
# open of another database. The signature-based ownership check (not the owner
# pid) is what protects it, so the probe-bus must be active at run time.
add_executable(issue_gh0159_idle_holder issue_gh0159_idle_holder.c)
target_include_directories(issue_gh0159_idle_holder PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0159_idle_holder PRIVATE mdbx-static)
add_test(NAME issue_gh0159_idle_holder COMMAND issue_gh0159_idle_holder)
set_tests_properties(issue_gh0159_idle_holder PROPERTIES ENVIRONMENT "MDBX_PROBES=1")