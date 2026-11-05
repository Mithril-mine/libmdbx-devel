cmake_minimum_required(VERSION 3.14)

# Special-build test configuration for the POSIX-1988 semaphore locking
# (MDBX_LOCKING=1988), the default on FreeBSD, NetBSD, Solaris and others.
# See tests/special-builds/README.md.
project(libmdbx-locking-posix1988 C)
enable_testing()

set(LIBMDBX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Path to the libmdbx repository root")

# The library must be built from this repository with the POSIX-1988
# semaphore locking enabled and nothing else pulled in (no tests/tools/C++).
set(MDBX_LOCKING 1988 CACHE STRING "Locking method" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(MDBX_ENABLE_TESTS OFF CACHE BOOL "Build MDBX tests" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "Build MDBX tools" FORCE)
set(MDBX_BUILD_CXX OFF CACHE BOOL "Build MDBX C++ API" FORCE)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build shared library" FORCE)
set(MDBX_INSTALL_MANPAGES OFF CACHE BOOL "Install man-pages" FORCE)
set(MDBX_BUILD_MCP_MEMORY OFF CACHE BOOL "Build MCP shared-graph-memory" FORCE)

add_subdirectory(${LIBMDBX_SOURCE_DIR} mdbx-sdk)

# Regression test for issue 323: a signal delivered to a thread that waits for
# the write lock must not interrupt sem_wait() into a fatal environment. The
# source is shared with tests/special-builds/locking-sysv, which covers semop().
add_executable(issue_gh0323_eintr ../locking-sysv/issue_gh0323_eintr.c)
target_include_directories(issue_gh0323_eintr PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0323_eintr PRIVATE mdbx-static)
add_test(NAME issue_gh0323_eintr COMMAND issue_gh0323_eintr)
set_tests_properties(issue_gh0323_eintr PROPERTIES TIMEOUT 60)
