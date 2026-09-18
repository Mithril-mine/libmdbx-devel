# Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
#
# THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
# UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
# VIA ANY MEDIUM IS STRICTLY PROHIBITED.
#
# The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
# for any purpose other than the purpose for which they were provided to you.
#
# The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
# the warranties of merchantability, fitness for a particular purpose and non infringement.
# In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
# whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
# or the use or other dealings in the software.
#
# The above copyright notice and this permission notice shall be included in all copies
# or substantial portions of the software.
#

if(NOT CMAKE_CXX_COMPILER_LOADED)
  enable_language(CXX)
  include(../cmake/compiler.cmake)
endif()

function(add_simple_test name)
  set(options DISABLED)
  set(oneValueArgs TIMEOUT SOURCE_PREFIX TARGET_PREFIX MDBX_LIB)
  set(multiValueArgs SOURCE LIBRARY DEPEND DLLPATH)
  cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(params_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown keywords given to add_simple_test(): \"${params_UNPARSED_ARGUMENTS}\".")
  endif()

  macro(oops)
    message(FATAL_ERROR "add_simple_test(): Opps, " ${ARGV})
  endmacro()

  if(NOT params_SOURCE_PREFIX)
    set(params_SOURCE_PREFIX "ut/")
  endif()

  if(NOT params_SOURCE)
    set(params_SOURCE "${params_SOURCE_PREFIX}${name}.c++")
  endif()

  if(NOT params_MDBX_LIB)
    set(params_MDBX_LIB ${TOOL_MDBX_LIB})
  endif()

  set(target "${params_TARGET_PREFIX}${name}")
  add_executable(${target} ${params_SOURCE})
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}")
  target_link_libraries(${target} ${params_MDBX_LIB})
  set_target_properties(${target} PROPERTIES SKIP_BUILD_RPATH FALSE BUILD_WITH_INSTALL_RPATH FALSE)

  if(MDBX_BUILD_CXX AND MDBX_CXX_STANDARD)
    set_target_properties(${target} PROPERTIES CXX_STANDARD ${MDBX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)
  endif()

  if(params_DEPEND)
    add_dependencies(${target} ${params_DEPEND})
  endif()

  if(TOOL_MDBX_DLLCRUTCH)
    if(CMAKE_BUILD_TYPE)
      string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPERCASE)
    else()
      set(CMAKE_BUILD_TYPE_UPPERCASE "")
    endif()
    foreach(dep IN LISTS params_LIBRARY)
      get_target_property(type ${dep} TYPE)
      if(type STREQUAL SHARED_LIBRARY)
        # Windows don't have RPATH feature, therefore we should prepare PATH or copy DLL(s)...
        if(CMAKE_CONFIGURATION_TYPES)
          # Could not provide static ENVIRONMENT property with configuration-depended path
          set(dir FALSE)
        else(CMAKE_CONFIGURATION_TYPES)
          get_target_property(filename ${dep} IMPORTED_LOCATION_${CMAKE_BUILD_TYPE_UPPERCASE})
          if(NOT filename)
            get_target_property(filename ${dep} IMPORTED_LOCATION)
          endif()
          if(filename)
            get_filename_component(dir ${filename} DIRECTORY)
          else(filename)
            get_target_property(dir ${dep} LIBRARY_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE_UPPERCASE})
            if(NOT dir)
              get_target_property(dir ${dep} RUNTIME_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE_UPPERCASE})
            endif()
            if(NOT dir)
              get_target_property(dir ${dep} LIBRARY_OUTPUT_DIRECTORY)
            endif()
            if(NOT dir)
              get_target_property(dir ${dep} RUNTIME_OUTPUT_DIRECTORY)
            endif()
          endif(filename)
        endif(CMAKE_CONFIGURATION_TYPES)
        if(dir)
          list(APPEND params_DLLPATH ${dir})
        else(dir)
          # Path is configuration-depended or not available, should copy dll
          add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND if exist "$<TARGET_PDB_FILE:${dep}>" ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_PDB_FILE:${dep}>" "$<TARGET_FILE_DIR:${target}>")
          add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${dep}>" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "${TOOL_MDBX_DLLCRUTCH}: Copy shared library ${dep} for test ${target}")
        endif(dir)
      endif()
    endforeach(dep)
  endif(TOOL_MDBX_DLLCRUTCH)

  if(NOT params_DISABLED AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
    add_test(NAME extra_${name} COMMAND $<TARGET_FILE:${target}>)
    set_tests_properties(extra_${name} PROPERTIES LABELS "ut")
    if(params_TIMEOUT)
      if(MEMORYCHECK_COMMAND
         OR CMAKE_MEMORYCHECK_COMMAND
         OR ENABLE_MEMCHECK)
        # FIXME: unless there are any other ideas how to fix the timeouts problem when testing under Valgrind.
        math(EXPR params_TIMEOUT "${params_TIMEOUT} * 42")
      endif()
      set_tests_properties(extra_${name} PROPERTIES TIMEOUT ${params_TIMEOUT})
    endif()
    if(params_DLLPATH)
      # Compose DLL's path in the ENVIRONMENT property
      if(WIN32)
        set(params_DLLPATH_ENV "${params_DLLPATH};$ENV{PATH}")
      else()
        set(params_DLLPATH_ENV "${params_DLLPATH}:$ENV{PATH}")
        string(REPLACE ":" ";" params_DLLPATH_ENV "${params_DLLPATH_ENV}")
      endif()
      list(REMOVE_DUPLICATES params_DLLPATH_ENV)
      if(WIN32)
        string(REPLACE ";" "\\;" params_DLLPATH_ENV "${params_DLLPATH_ENV}")
      else()
        string(REPLACE ";" ":" params_DLLPATH_ENV "${params_DLLPATH_ENV}")
      endif()
      set_tests_properties(extra_${name} PROPERTIES ENVIRONMENT "PATH=${params_DLLPATH_ENV}")
    endif()
  endif()
endfunction(add_simple_test)

macro(add_extra_test)
  add_simple_test(${ARGV} TARGET_PREFIX "test_extra_")
endmacro(add_extra_test)

add_subdirectory(framework)

# ######################################################################################################################

if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  message(WARNING "No emulator to run cross-compiled tests")
  add_test(NAME fake_since_no_crosscompiling_emulator COMMAND ${CMAKE_COMMAND} -E echo
                                                              "No emulator to run cross-compiled tests")
else()

  string(
    RANDOM
    LENGTH 9
    ALPHABET "1234567890" test_seed)
  message(STATUS "The ${test_seed} will be used for seeding tests. Re-run cmake to re-seed it.")

  if(WIN32 OR APPLE)
    set(MDBX_TEST_ITER 2)
  elseif(DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
    set(MDBX_TEST_ITER 3)
  else()
    set(MDBX_TEST_ITER 7)
  endif()

  add_test(NAME smoke_long COMMAND $<TARGET_FILE:mdbx_test> --duration=5m --loglevel=notice --prng-seed=${test_seed}
                                   --progress --console=no --pathname=smoke.db --dont-cleanup-after basic)
  set_tests_properties(smoke_long PROPERTIES LABELS "smoke-long" TIMEOUT 1800 RUN_SERIAL OFF)
  if(MDBX_BUILD_TOOLS)
    add_test(NAME smoke_long_chk COMMAND $<TARGET_FILE:mdbx_chk> -nvv smoke.db)
    set_tests_properties(smoke_long_chk PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES smoke.db)

    add_test(NAME smoke_long_chk_copy COMMAND $<TARGET_FILE:mdbx_chk> -nvv smoke.db-copy)
    set_tests_properties(smoke_long_chk_copy PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES smoke.db-copy)

    add_test(NAME smoke_long_copy_asis COMMAND $<TARGET_FILE:mdbx_copy> -f smoke.db copy_asis.db)
    set_tests_properties(smoke_long_copy_asis PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      REQUIRED_FILES smoke.db)

    add_test(NAME smoke_long_copy_compactify COMMAND $<TARGET_FILE:mdbx_copy> -f -c smoke.db copy_compactify.db)
    set_tests_properties(smoke_long_copy_compactify PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      REQUIRED_FILES smoke.db)
  endif()

  add_test(
    NAME dupsort_writemap
    COMMAND
      $<TARGET_FILE:mdbx_test> --duration=5m --loglevel=notice --prng-seed=${test_seed} --mode=+writemap
      --table=+data.fixed --keygen.split=29 --datalen=rnd --progress --console=no --repeat=2
      --pathname=dupsort_writemap.db --dont-cleanup-after basic)
  set_tests_properties(dupsort_writemap PROPERTIES LABELS "smoke-long" TIMEOUT 1800 RUN_SERIAL OFF)

  if(MDBX_BUILD_TOOLS)
    add_test(NAME dupsort_writemap_chk COMMAND $<TARGET_FILE:mdbx_chk> -nvvwc dupsort_writemap.db)
    set_tests_properties(dupsort_writemap_chk PROPERTIES
      LABELS "smoke-long"
      DEPENDS dupsort_writemap
      TIMEOUT 60
      REQUIRED_FILES dupsort_writemap.db)

    add_test(NAME dupsort_writemap_chk_copy COMMAND $<TARGET_FILE:mdbx_chk> -nvvc dupsort_writemap.db-copy)
    set_tests_properties(dupsort_writemap_chk_copy PROPERTIES
      LABELS "smoke-long"
      DEPENDS dupsort_writemap
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "monopolistic mode"
      REQUIRED_FILES dupsort_writemap.db-copy)
  endif()

  add_test(NAME uniq_nested
           COMMAND $<TARGET_FILE:mdbx_test> --duration=5m --loglevel=notice --mode=-writemap,-nosync-safe,-lifo
                   --progress --console=no --repeat=2 --pathname=uniq_nested.db --dont-cleanup-after basic)
  set_tests_properties(uniq_nested PROPERTIES LABELS "smoke-long" TIMEOUT 1800 RUN_SERIAL OFF)
  if(MDBX_BUILD_TOOLS)
    add_test(NAME uniq_nested_chk COMMAND $<TARGET_FILE:mdbx_chk> -nvvw uniq_nested.db)
    set_tests_properties(uniq_nested_chk PROPERTIES
      LABELS "smoke-long"
      DEPENDS uniq_nested
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES uniq_nested.db)

    add_test(NAME uniq_nested_chk_copy COMMAND $<TARGET_FILE:mdbx_chk> -nvv uniq_nested.db-copy)
    set_tests_properties(uniq_nested_chk_copy PROPERTIES
      LABELS "smoke-long"
      DEPENDS uniq_nested
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES uniq_nested.db-copy)
  endif()

  if(WIN32 AND MDBX_BUILD_TOOLS)
    set_tests_properties(smoke_long_chk PROPERTIES RESOURCE_LOCK smoke.db)
    set_tests_properties(smoke_long_copy_asis PROPERTIES RESOURCE_LOCK smoke.db)
    set_tests_properties(smoke_long_copy_compactify PROPERTIES RESOURCE_LOCK smoke.db)
  endif()

  # Quick smoke scenarios (a-la `make smoke`).
  set(MDBX_SMOKE_EXTRA "" CACHE STRING "Extra command-line arguments for the quick smoke scenarios")

  add_test(NAME smoke_basic COMMAND $<TARGET_FILE:mdbx_test> --duration 100 --table=+data.integer
                                   --keygen.split=29 --datalen.min=min --datalen.max=max --progress --console=no
                                   --repeat=${MDBX_TEST_ITER} --pathname=smoke.db --dont-cleanup-after
                                   ${MDBX_SMOKE_EXTRA} basic)
  set_tests_properties(smoke_basic PROPERTIES LABELS "smoke" TIMEOUT 600 RUN_SERIAL OFF)

  add_test(NAME smoke_writemap COMMAND $<TARGET_FILE:mdbx_test> --duration 100 --mode=-writemap,-nosync-safe,-lifo
                                    --progress --console=no --repeat=${MDBX_TEST_ITER} --pathname=smoke.db
                                    --dont-cleanup-after ${MDBX_SMOKE_EXTRA} basic)
  set_tests_properties(smoke_writemap PROPERTIES LABELS "smoke" TIMEOUT 600 DEPENDS smoke_basic RUN_SERIAL OFF)

  if(MDBX_BUILD_TOOLS)
    add_test(NAME smoke_chk COMMAND $<TARGET_FILE:mdbx_chk> -vvn smoke.db)
    set_tests_properties(smoke_chk PROPERTIES
      LABELS "smoke"
      DEPENDS smoke_writemap
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES smoke.db)
    add_test(NAME smoke_chk_copy COMMAND $<TARGET_FILE:mdbx_chk> -vvn smoke.db-copy)
    set_tests_properties(smoke_chk_copy PROPERTIES
      LABELS "smoke"
      DEPENDS smoke_writemap
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES smoke.db-copy)
  endif()

  if(WIN32)
    set_tests_properties(smoke_basic smoke_writemap PROPERTIES RESOURCE_LOCK smoke.db)
  endif()

  add_test(NAME smoke_fault COMMAND $<TARGET_FILE:mdbx_test> --duration 300 --progress --console=no --pathname=fault.db
                                   --inject-writefault=42 --dump-config --dont-cleanup-after ${MDBX_SMOKE_EXTRA} basic)
  set_tests_properties(smoke_fault PROPERTIES
    LABELS "smoke-fault"
    TIMEOUT 1800
    RUN_SERIAL OFF
    WILL_FAIL TRUE)
  if(MDBX_BUILD_TOOLS)
    add_test(NAME smoke_fault_chk COMMAND $<TARGET_FILE:mdbx_chk> -vvnw fault.db)
    set_tests_properties(smoke_fault_chk PROPERTIES
      LABELS "smoke-fault"
      DEPENDS smoke_fault
      TIMEOUT 60
      REQUIRED_FILES fault.db)
  endif()

  add_test(NAME smoke_sp_hill COMMAND $<TARGET_FILE:mdbx_test> --duration 100 --table=+data.integer --keygen.split=29
                                  --datalen.min=min --datalen.max=max --progress --console=no --repeat=42
                                  --pathname=singleprocess.db --dont-cleanup-after ${MDBX_SMOKE_EXTRA} --hill)
  set_tests_properties(smoke_sp_hill PROPERTIES
    LABELS "smoke-singleprocess"
    TIMEOUT 600
    RUN_SERIAL OFF)
  add_test(NAME smoke_sp_copy COMMAND $<TARGET_FILE:mdbx_test> --duration 100 --progress --console=no --repeat=2
                                 --pathname=singleprocess.db --dont-cleanup-before --dont-cleanup-after ${MDBX_SMOKE_EXTRA} --copy)
  set_tests_properties(smoke_sp_copy PROPERTIES
    LABELS "smoke-singleprocess"
    DEPENDS smoke_sp_hill
    TIMEOUT 600
    RUN_SERIAL OFF)
  add_test(NAME smoke_sp_nested COMMAND $<TARGET_FILE:mdbx_test> --duration 100 --mode=-writemap,-nosync-safe,-lifo
                                   --progress --console=no --repeat=42 --pathname=singleprocess.db --dont-cleanup-before
                                   --dont-cleanup-after ${MDBX_SMOKE_EXTRA} --nested)
  set_tests_properties(smoke_sp_nested PROPERTIES
    LABELS "smoke-singleprocess"
    DEPENDS smoke_sp_copy
    TIMEOUT 600
    RUN_SERIAL OFF)
  if(MDBX_BUILD_TOOLS)
    add_test(NAME smoke_sp_chk COMMAND $<TARGET_FILE:mdbx_chk> -vvn singleprocess.db)
    set_tests_properties(smoke_sp_chk PROPERTIES
      LABELS "smoke-singleprocess"
      DEPENDS smoke_sp_nested
      TIMEOUT 60
      REQUIRED_FILES singleprocess.db)
    add_test(NAME smoke_sp_chk_copy COMMAND $<TARGET_FILE:mdbx_chk> -vvn singleprocess.db-copy)
    set_tests_properties(smoke_sp_chk_copy PROPERTIES
      LABELS "smoke-singleprocess"
      DEPENDS smoke_sp_nested
      TIMEOUT 60
      REQUIRED_FILES singleprocess.db-copy)
  endif()

  if(WIN32)
    set_tests_properties(smoke_sp_hill smoke_sp_copy smoke_sp_nested PROPERTIES RESOURCE_LOCK singleprocess.db)
  endif()

  option(MDBX_ENABLE_LONG_TESTS
         "Register long-running test scenarios (stochastic, etc.) as CTest entries" OFF)
  if(UNIX AND MDBX_ENABLE_LONG_TESTS)
    add_test(NAME stochastic COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stochastic.sh --whole-duration 600 --probe-duration 60
                                    --dont-check-ram-size --loops 2 --db-upto-mb 256 --skip-make --taillog)
    set_tests_properties(stochastic PROPERTIES
      LABELS "stochastic"
      TIMEOUT 10800
      RUN_SERIAL ON
      WORKING_DIRECTORY ${MDBX_OUTPUT_DIR})

    add_test(NAME stochastic_single COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stochastic.sh --whole-duration 600
                                     --probe-duration 60 --dont-check-ram-size --single --loops 2 --db-upto-mb 256
                                     --skip-make --taillog)
    set_tests_properties(stochastic_single PROPERTIES
      LABELS "stochastic-single"
      TIMEOUT 10800
      RUN_SERIAL ON
      WORKING_DIRECTORY ${MDBX_OUTPUT_DIR})

    add_test(NAME stochastic_long COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stochastic.sh --loops 42 --db-upto-mb 1024
                                    --extra --skip-make --taillog)
    set_tests_properties(stochastic_long PROPERTIES
      LABELS "stochastic-long"
      TIMEOUT 0
      RUN_SERIAL ON
      WORKING_DIRECTORY ${MDBX_OUTPUT_DIR})
  endif()

  if(NOT SUBPROJECT)
    if(UNIX)
      add_extra_test(upsert_alldups SOURCE ut/upsert_alldups.c)
      add_extra_test(dupfix_addodd SOURCE ut/dupfix_addodd.c)
      add_extra_test(dbi_nested_txn SOURCE ut/dbi_nested_txn.c)
    endif()
    add_extra_test(details_rkl SOURCE ut/details_rkl.c)
    if(MSVC_C11_MAD_ATOMICS)
      target_compile_options(test_extra_details_rkl
                             PRIVATE "$<$<COMPILE_LANGUAGE:CXX,C>:/std:clatest;/experimental:c11atomics>")
    endif()

    add_extra_test(global_init SOURCE ut/global_init.c MDBX_LIB mdbx-static)

    add_extra_test(rename_dbi SOURCE ut/rename_dbi.c)
    add_extra_test(nested_drop_abort SOURCE ut/nested_drop_abort.c)
    if(MDBX_BUILD_CXX)
      if(NOT WIN32 OR NOT MDBX_CXX_STANDARD LESS 17)
        add_extra_test(cursor_closing TIMEOUT 10800)
        add_extra_test(early_close_dbi)
        add_extra_test(maindb_ordinal)
        add_extra_test(dupfix_multiple TIMEOUT 10800)
        add_extra_test(doubtless_positioning TIMEOUT 10800)
        add_extra_test(crunched_delete TIMEOUT 10800)
        add_extra_test(distance_scroll_distribute TIMEOUT 10800)
        add_extra_test(dbi)
        add_extra_test(open)
        add_extra_test(txn)
        add_extra_test(buffers)
        add_extra_test(bunches_removal)
        add_extra_test(get_cached)
        add_extra_test(reverse_insertions)
        add_subdirectory(issues)
      endif()
      add_extra_test(hex_base64_base58)
    endif()
  endif()

endif()
