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

  if(MSVC AND MSVC_C11_MAD_ATOMICS)
    # Plain-C test programs compile library sources directly (details_rkl.c pulls
    # in src/rkl.c and src/txl.c) and therefore need C11 atomics on MSVC for
    # non-x86 targets. Use the plain flag, not a COMPILE_LANGUAGE generator
    # expression: the latter was observed to silently no-op for these subdirectory
    # targets under the Visual Studio generator (see TASK-17).
    # C_STANDARD is required too: without /std:c11+ cl.exe keeps __STDC_VERSION__
    # at 199901L, so atomics-types.h does not define MDBX_HAVE_C11ATOMICS and
    # atomics-ops.h:46 aborts with C1189 on non-x86 targets even when
    # /experimental:c11atomics is passed (TASK-17 incomplete for msvc ARM64).
    set_target_properties(${target} PROPERTIES C_STANDARD ${MDBX_C_STANDARD} C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE /experimental:c11atomics)
  endif()

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
                                   --progress --console=no --pathname=smoke-long.db --dont-cleanup-after basic)
  set_tests_properties(smoke_long PROPERTIES LABELS "smoke-long" TIMEOUT 1800 RUN_SERIAL OFF)
  if(MDBX_BUILD_TOOLS)
    add_test(NAME smoke_long_chk COMMAND $<TARGET_FILE:mdbx_chk> -nvv smoke-long.db)
    set_tests_properties(smoke_long_chk PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES smoke-long.db)

    add_test(NAME smoke_long_chk_copy COMMAND $<TARGET_FILE:mdbx_chk> -nvv smoke-long.db-copy)
    set_tests_properties(smoke_long_chk_copy PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      FAIL_REGULAR_EXPRESSION "cooperative mode"
      REQUIRED_FILES smoke-long.db-copy)

    add_test(NAME smoke_long_copy_asis COMMAND $<TARGET_FILE:mdbx_copy> -f smoke-long.db copy_asis.db)
    set_tests_properties(smoke_long_copy_asis PROPERTIES
      LABELS "smoke-long"
      DEPENDS smoke_long
      TIMEOUT 60
      REQUIRED_FILES smoke-long.db)

    add_test(NAME smoke_long_copy_compactify COMMAND $<TARGET_FILE:mdbx_copy> -f -c smoke-long.db copy_compactify.db)
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
    set_tests_properties(smoke_long_chk PROPERTIES RESOURCE_LOCK smoke-long.db)
    set_tests_properties(smoke_long_copy_asis PROPERTIES RESOURCE_LOCK smoke-long.db)
    set_tests_properties(smoke_long_copy_compactify PROPERTIES RESOURCE_LOCK smoke-long.db)
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
      WORKING_DIRECTORY ${MDBX_OUTPUT_DIR}
      RESOURCE_LOCK stochastic)

    add_test(NAME stochastic_single COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stochastic.sh --whole-duration 600
                                     --probe-duration 60 --dont-check-ram-size --single --loops 2 --db-upto-mb 256
                                     --skip-make --taillog)
    set_tests_properties(stochastic_single PROPERTIES
      LABELS "stochastic-single"
      TIMEOUT 10800
      RUN_SERIAL ON
      WORKING_DIRECTORY ${MDBX_OUTPUT_DIR}
      RESOURCE_LOCK stochastic)

    add_test(NAME stochastic_long COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stochastic.sh --loops 42 --db-upto-mb 1024
                                    --extra --skip-make --taillog)
    set_tests_properties(stochastic_long PROPERTIES
      LABELS "stochastic-long"
      TIMEOUT 0
      RUN_SERIAL ON
      WORKING_DIRECTORY ${MDBX_OUTPUT_DIR}
      RESOURCE_LOCK stochastic)
  endif()

  if(NOT SUBPROJECT)
    # Hierarchical test segmentation.
    #
    # Every unit test carries the root label `ut` plus one or more area labels
    # from the tree below, so a domain can be run addressably:
    #
    #   ctest -L 'ut\.'             - all unit tests
    #   ctest -L 'ut\.' -LE 'ut\.heavy' - fast unit tests (default)
    #   ctest -L 'ut\.cxx'          - C++ API layer only
    #   ctest -L 'ut\.dbi'          - DBI area (also covers ut.dbi.* descendants)
    #   ctest -L 'ut\.heavy'        - slow/stress tests only
    #
    # Tree of areas (a test may belong to several branches). Sources are placed
    # into tests/ut/<area>/ subdirectories matching the primary area label:
    #   ut                  - root: all unit tests
    #   |- ut.api           - C API surface (buffers, parameters, stat-info, ...)
    #   |- ut.cxx           - C++ API layer (slice, buffer, txn-metadata, ...)
    #   |- ut.env           - environment lifecycle (open/close, global init)
    #   |- ut.dbi           - DBI lifecycle and operations
    #   |- ut.txn           - transactions (incl. nested, cache, ordering)
    #   |- ut.cursor        - cursors (positioning, iteration, closing)
    #   |- ut.gc            - garbage collection / reclaim
    #   |- ut.issues        - regression tests for reported issues
    #   Attribute labels (orthogonal): ut.heavy for slow/stress tests.
    if(UNIX)
      add_ut(upsert_alldups SOURCE ut/api/upsert_alldups.c++)
      set_tests_properties(upsert_alldups PROPERTIES LABELS "ut;ut.api;ut.dbi")
      add_ut(dupfix_addodd SOURCE ut/api/dupfix_addodd.c++)
      set_tests_properties(dupfix_addodd PROPERTIES LABELS "ut;ut.api;ut.dbi")
      add_ut(dbi_nested_txn SOURCE ut/dbi/dbi_nested_txn.c++)
      set_tests_properties(dbi_nested_txn PROPERTIES LABELS "ut;ut.dbi;ut.txn")
    endif()
    add_extra_test(details_rkl SOURCE ut/api/details_rkl.c)
    set_tests_properties(extra_details_rkl PROPERTIES LABELS "ut;ut.api;ut.gc")

    add_ut(global_init SOURCE ut/api/global_init.c++ LIBRARY mdbx-static)
    set_tests_properties(global_init PROPERTIES LABELS "ut;ut.api;ut.env")

    add_ut(rename_dbi SOURCE ut/dbi/rename_dbi.c++)
    set_tests_properties(rename_dbi PROPERTIES LABELS "ut;ut.dbi")
    add_ut(nested_drop_abort SOURCE ut/dbi/nested_drop_abort.c++)
    set_tests_properties(nested_drop_abort PROPERTIES LABELS "ut;ut.dbi;ut.txn")
    if(MDBX_BUILD_CXX)
      if(NOT WIN32 OR NOT MDBX_CXX_STANDARD LESS 17)
        add_ut(cursor_closing SOURCE ut/cursor/cursor_closing.c++ TIMEOUT 10800)
        set_tests_properties(cursor_closing PROPERTIES LABELS "ut;ut.cursor;ut.heavy")
        add_ut(early_close_dbi SOURCE ut/dbi/early_close_dbi.c++)
        set_tests_properties(early_close_dbi PROPERTIES LABELS "ut;ut.dbi;ut.env")
        add_ut(maindb_ordinal SOURCE ut/dbi/maindb_ordinal.c++)
        set_tests_properties(maindb_ordinal PROPERTIES LABELS "ut;ut.dbi")
        add_ut(dupfix_multiple SOURCE ut/dbi/dupfix_multiple.c++ TIMEOUT 10800)
        set_tests_properties(dupfix_multiple PROPERTIES LABELS "ut;ut.dbi;ut.heavy")
        add_ut(doubtless_positioning SOURCE ut/cursor/doubtless_positioning.c++ TIMEOUT 10800)
        set_tests_properties(doubtless_positioning PROPERTIES LABELS "ut;ut.cursor;ut.heavy")
        add_ut(crunched_delete SOURCE ut/txn/crunched_delete.c++ TIMEOUT 10800)
        set_tests_properties(crunched_delete PROPERTIES LABELS "ut;ut.txn;ut.heavy")
        add_ut(distance_scroll_distribute SOURCE ut/cursor/distance_scroll_distribute.c++ TIMEOUT 10800)
        set_tests_properties(distance_scroll_distribute PROPERTIES LABELS "ut;ut.cursor;ut.heavy")
        add_ut(dbi SOURCE ut/dbi/dbi.c++)
        set_tests_properties(dbi PROPERTIES LABELS "ut;ut.dbi")
        add_ut(open SOURCE ut/env/open.c++)
        set_tests_properties(open PROPERTIES LABELS "ut;ut.env;ut.txn")
        add_ut(txn SOURCE ut/txn/txn.c++)
        set_tests_properties(txn PROPERTIES LABELS "ut;ut.txn")
        add_ut(buffers SOURCE ut/api/buffers.c++)
        set_tests_properties(buffers PROPERTIES LABELS "ut;ut.api")
        add_ut(bunches_removal SOURCE ut/gc/bunches_removal.c++)
        set_tests_properties(bunches_removal PROPERTIES LABELS "ut;ut.gc")
        add_ut(get_cached SOURCE ut/api/get_cached.c++)
        set_tests_properties(get_cached PROPERTIES LABELS "ut;ut.api;ut.txn")
        add_ut(reverse_insertions SOURCE ut/txn/reverse_insertions.c++)
        set_tests_properties(reverse_insertions PROPERTIES LABELS "ut;ut.txn")
        add_ut(parameters SOURCE ut/api/parameters.c++)
        set_tests_properties(parameters PROPERTIES LABELS "ut;ut.api")
        add_ut(tables-and-chk SOURCE ut/api/tables-and-chk.c++)
        set_tests_properties(tables-and-chk PROPERTIES LABELS "ut;ut.api")
        add_ut(stat-info SOURCE ut/api/stat-info.c++)
        set_tests_properties(stat-info PROPERTIES LABELS "ut;ut.api;ut.gc")
        add_ut(cursor-batch SOURCE ut/api/cursor-batch.c++)
        set_tests_properties(cursor-batch PROPERTIES LABELS "ut;ut.api;ut.cursor")
        add_ut(cache-get SOURCE ut/api/cache-get.c++)
        set_tests_properties(cache-get PROPERTIES LABELS "ut;ut.api;ut.txn")
        add_ut(transforms SOURCE ut/api/transforms.c++)
        set_tests_properties(transforms PROPERTIES LABELS "ut;ut.api;ut.cxx")
        add_ut(defrag-cursors SOURCE ut/api/defrag-cursors.c++)
        set_tests_properties(defrag-cursors PROPERTIES LABELS "ut;ut.api;ut.cursor")
        add_ut(slice-view SOURCE ut/cxx/slice-view.c++)
        set_tests_properties(slice-view PROPERTIES LABELS "ut;ut.cxx")
        add_ut(buffer-extra SOURCE ut/cxx/buffer-extra.c++)
        set_tests_properties(buffer-extra PROPERTIES LABELS "ut;ut.cxx")
        add_ut(txn-metadata SOURCE ut/cxx/txn-metadata.c++)
        set_tests_properties(txn-metadata PROPERTIES LABELS "ut;ut.cxx;ut.txn")
        add_ut(env-params SOURCE ut/api/env-params.c++)
        set_tests_properties(env-params PROPERTIES LABELS "ut;ut.api")
        add_ut(slice-pod SOURCE ut/cxx/slice-pod.c++)
        set_tests_properties(slice-pod PROPERTIES LABELS "ut;ut.cxx")
        add_ut(txn-dataops SOURCE ut/txn/txn-dataops.c++)
        set_tests_properties(txn-dataops PROPERTIES LABELS "ut;ut.txn")
        add_ut(cursor-api SOURCE ut/cursor/cursor-api.c++)
        set_tests_properties(cursor-api PROPERTIES LABELS "ut;ut.cursor")
        add_ut(txn-park SOURCE ut/txn/txn-park.c++)
        set_tests_properties(txn-park PROPERTIES LABELS "ut;ut.txn")
        add_ut(multivalue-nav SOURCE ut/cursor/multivalue-nav.c++)
        set_tests_properties(multivalue-nav PROPERTIES LABELS "ut;ut.cursor")
        add_ut(env-maintenance SOURCE ut/api/env-maintenance.c++)
        set_tests_properties(env-maintenance PROPERTIES LABELS "ut;ut.api;ut.env")
        add_ut(put-flags SOURCE ut/cxx/put-flags.c++)
        set_tests_properties(put-flags PROPERTIES LABELS "ut;ut.cxx;ut.txn;ut.dbi")
        add_ut(estimate SOURCE ut/cxx/estimate.c++)
        set_tests_properties(estimate PROPERTIES LABELS "ut;ut.cxx;ut.cursor")
        add_ut(value2key SOURCE ut/api/value2key.c++)
        set_tests_properties(value2key PROPERTIES LABELS "ut;ut.api;ut.cxx")
        add_ut(dbi-defer SOURCE ut/dbi/dbi-defer.c++)
        set_tests_properties(dbi-defer PROPERTIES LABELS "ut;ut.dbi")
        add_ut(cursor-multis SOURCE ut/cursor/cursor-multis.c++)
        set_tests_properties(cursor-multis PROPERTIES LABELS "ut;ut.cursor")
        add_ut(fluent-params SOURCE ut/api/fluent-params.c++)
        set_tests_properties(fluent-params PROPERTIES LABELS "ut;ut.api;ut.env")
        add_ut(copy-ops SOURCE ut/api/copy-ops.c++)
        set_tests_properties(copy-ops PROPERTIES LABELS "ut;ut.api;ut.env")
        add_ut(managed-lifecycle SOURCE ut/api/managed-lifecycle.c++)
        set_tests_properties(managed-lifecycle PROPERTIES LABELS "ut;ut.api;ut.cxx")
        add_ut(put-matrix SOURCE ut/cxx/put-matrix.c++)
        set_tests_properties(put-matrix PROPERTIES LABELS "ut;ut.cxx;ut.txn;ut.dbi")
        add_ut(get-ex SOURCE ut/cxx/get-ex.c++)
        set_tests_properties(get-ex PROPERTIES LABELS "ut;ut.cxx;ut.txn")
        add_subdirectory(ut/issues)
      endif()
      add_ut(hex_base64_base58 SOURCE ut/api/hex_base64_base58.c++)
      set_tests_properties(hex_base64_base58 PROPERTIES LABELS "ut;ut.api")
    endif()
  endif()

endif()
