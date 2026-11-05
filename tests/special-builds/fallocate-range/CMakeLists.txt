cmake_minimum_required(VERSION 3.14)

# Special-build test configuration for the fallocate(FALLOC_FL_ALLOCATE_RANGE)
# branch of osal_fsetsize(), which is compiled only when the C headers define
# FALLOC_FL_ALLOCATE_RANGE (recent kernel UAPI headers). See
# tests/special-builds/README.md and the test source for the rationale.
project(libmdbx-fallocate-range C)
enable_testing()

set(LIBMDBX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
    CACHE PATH "Path to the libmdbx repository root")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The fallocate-range special build is Linux-only")
endif()

# Older headers lack FALLOC_FL_ALLOCATE_RANGE: define it with its UAPI value
# (0x00, i.e. the default mode of fallocate()) so the branch is compiled.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(FALLOC_FL_ALLOCATE_RANGE "fcntl.h" HAVE_FALLOC_FL_ALLOCATE_RANGE)
unset(CMAKE_REQUIRED_DEFINITIONS)
if(NOT HAVE_FALLOC_FL_ALLOCATE_RANGE)
  add_compile_definitions(FALLOC_FL_ALLOCATE_RANGE=0x00)
endif()

# The library must be built from this repository with the probe-bus enabled
# (the test makes fallocate() fail via the fsetsize_fallocate_errno FAULT site)
# and nothing else pulled in (no tests/tools/C++).
set(MDBX_PROBES ON CACHE BOOL "Enable probe-bus instrumentation" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
set(MDBX_ENABLE_TESTS OFF CACHE BOOL "Build MDBX tests" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "Build MDBX tools" FORCE)
set(MDBX_BUILD_CXX OFF CACHE BOOL "Build MDBX C++ API" FORCE)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build shared library" FORCE)
set(MDBX_INSTALL_MANPAGES OFF CACHE BOOL "Install man-pages" FORCE)
set(MDBX_BUILD_MCP_MEMORY OFF CACHE BOOL "Build MCP shared-graph-memory" FORCE)

add_subdirectory(${LIBMDBX_SOURCE_DIR} mdbx-sdk)

# Regression test for issue 320: a failed fallocate() must be reported by
# osal_fsetsize() (EIO, ENOSPC), while "not supported" is still ignored.
add_executable(issue_gh0320 issue_gh0320.c)
target_include_directories(issue_gh0320 PRIVATE ${LIBMDBX_SOURCE_DIR})
target_link_libraries(issue_gh0320 PRIVATE mdbx-static)
add_test(NAME issue_gh0320 COMMAND issue_gh0320)
set_tests_properties(issue_gh0320 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
