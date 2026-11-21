# The TIMEOUT scale factor: the per-test ctest TIMEOUT values (the manifest in
# tests/test-timeouts.cmake and the explicit ones in the smoke/examples blocks)
# are calibrated for the reference x86-64 Linux configuration ("~3x the runtime
# in the slowest configuration", see tests/AGENTS.md, "Test timeouts"), so the
# setups whose runners are systematically slower get their budgets scaled up
# here, rather than committing per-platform manifests. The factors compose
# multiplicatively (the macOS runners are ARM64 and their hosted storage crawls
# under load, the Debug builds slow every scenario down by several times). A
# cell or a local run may pin the factor via the MDBX_TEST_TIMEOUT_SCALE cache
# variable, e.g. -DMDBX_TEST_TIMEOUT_SCALE=4.
if(NOT DEFINED MDBX_TEST_TIMEOUT_SCALE)
  set(MDBX_TEST_TIMEOUT_SCALE 1)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv[67]l?)$")
    math(EXPR MDBX_TEST_TIMEOUT_SCALE "${MDBX_TEST_TIMEOUT_SCALE} * 2")
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    math(EXPR MDBX_TEST_TIMEOUT_SCALE "${MDBX_TEST_TIMEOUT_SCALE} * 2")
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    math(EXPR MDBX_TEST_TIMEOUT_SCALE "${MDBX_TEST_TIMEOUT_SCALE} * 2")
  endif()
endif()

# Scales the TIMEOUT property of every test registered in the CURRENT
# directory. Call it at the end of a CMakeLists.txt that registers tests:
# the newer CMake versions scope the tests to their directories, so a
# top-level pass cannot see (let alone modify) the subdirectories' tests.
macro(mdbx_scale_dir_test_timeouts)
  get_property(mdbx_scale_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
  foreach(mdbx_scale_t IN LISTS mdbx_scale_tests)
    get_property(mdbx_scale_v TEST "${mdbx_scale_t}" PROPERTY TIMEOUT)
    if(mdbx_scale_v)
      math(EXPR mdbx_scale_v "${mdbx_scale_v} * ${MDBX_TEST_TIMEOUT_SCALE}")
      set_property(TEST "${mdbx_scale_t}" PROPERTY TIMEOUT "${mdbx_scale_v}")
    endif()
  endforeach()
  unset(mdbx_scale_tests)
  unset(mdbx_scale_t)
  unset(mdbx_scale_v)
endmacro()
