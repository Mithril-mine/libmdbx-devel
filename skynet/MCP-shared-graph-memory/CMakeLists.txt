# CMake integration for the shared-graph-memory Python module (dev-only).
#
# Included from the top-level CMakeLists.txt guarded by MDBX_BUILD_MCP_MEMORY
# inside a dist-cutoff region, so it never ships in the amalgamated dist/.
# Independent of MDBX_ENABLE_TESTS: the module builds its own host libmdbx
# (see tools/build_libmdbx.py) and runs a pytest suite via CTest fixtures.

find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND AND MDBX_CAN_RUN_HOST_TESTS)
  # Fixture setup: build host libmdbx into mcp_memory/_lib (idempotent).
  add_test(NAME mcp_memory_prepare
           COMMAND ${CMAKE_COMMAND} -E env
                   PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
                   ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_libmdbx.py
                   --src ${CMAKE_CURRENT_SOURCE_DIR}/../..
                   --build-dir ${CMAKE_CURRENT_BINARY_DIR}/_build-mcp-memory
                   --output ${CMAKE_CURRENT_SOURCE_DIR}/mcp_memory/_lib)
  set_tests_properties(mcp_memory_prepare PROPERTIES
                       FIXTURES_SETUP mcp_memory_fixture
                       LABELS "mcp-memory")

  # The actual module test-suite.
  add_test(NAME mcp_memory_pytest
           COMMAND ${CMAKE_COMMAND} -E env
                   PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
                   ${Python3_EXECUTABLE} -m pytest
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/)
  set_tests_properties(mcp_memory_pytest PROPERTIES
                       FIXTURES_REQUIRED mcp_memory_fixture
                       ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
                       LABELS "mcp-memory"
                       TIMEOUT 600)
else()
  if(NOT Python3_FOUND)
    message(WARNING "shared-graph-memory: Python3 not found, mcp-memory tests skipped")
  elseif(NOT MDBX_CAN_RUN_HOST_TESTS)
    message(WARNING "shared-graph-memory: cannot run tests in host environment (cross-compile without emulator), mcp-memory tests skipped")
  endif()
endif()

# Convenient build target (e.g. `cmake --build <dir> --target mcp_memory_prepare`)
# even when ctest fixtures are not exercised.
if(Python3_FOUND)
  add_custom_target(mcp_memory_prepare
                    COMMAND ${CMAKE_COMMAND} -E env
                            PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
                            ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_libmdbx.py
                            --src ${CMAKE_CURRENT_SOURCE_DIR}/../..
                            --build-dir ${CMAKE_CURRENT_BINARY_DIR}/_build-mcp-memory
                            --output ${CMAKE_CURRENT_SOURCE_DIR}/mcp_memory/_lib)
endif()