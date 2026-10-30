# Copyright (c) 2012-2024 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> ###############################################
# SPDX-License-Identifier: Apache-2.0

if(CMAKE_VERSION VERSION_LESS 3.8.2)
  cmake_minimum_required(VERSION 3.0.2)
elseif(CMAKE_VERSION VERSION_LESS 3.12)
  cmake_minimum_required(VERSION 3.8.2)
else()
  cmake_minimum_required(VERSION 3.12)
endif()

cmake_policy(PUSH)
cmake_policy(VERSION ${CMAKE_MINIMUM_REQUIRED_VERSION})
if(NOT CMAKE_VERSION VERSION_LESS 3.21)
  cmake_policy(SET CMP0126 NEW)
endif()
if(NOT CMAKE_VERSION VERSION_LESS 3.17)
  cmake_policy(SET CMP0102 NEW)
endif()
if(NOT CMAKE_VERSION VERSION_LESS 3.15)
  cmake_policy(SET CMP0091 NEW)
endif()
if(NOT CMAKE_VERSION VERSION_LESS 3.13)
  cmake_policy(SET CMP0077 NEW)
endif()
if(NOT CMAKE_VERSION VERSION_LESS 3.12)
  cmake_policy(SET CMP0075 NEW)
endif()
if(NOT CMAKE_VERSION VERSION_LESS 3.9)
  cmake_policy(SET CMP0068 NEW)
  cmake_policy(SET CMP0069 NEW)
endif()

include(CTest)
if(BUILD_TESTING)

  if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    message(WARNING "No emulator to run cross-compiled tests")
    add_test(NAME fake_since_no_crosscompiling_emulator COMMAND ${CMAKE_COMMAND} -E echo
                                                                "No emulator to run cross-compiled tests")
  endif()

  if(NOT GTEST_FOUND AND NOT (DEFINED BUILD_GTEST AND BUILD_GTEST))
    # Expected GTest was already found and/or pointed via ${gtest_root}, otherwise will search at ${gtest_paths}
    # locations, if defined or default ones.
    if(DEFINED CONAN_RUNTIME_LIB_DIRS)
      message(STATUS "Peek GTest from \"${CONAN_RUNTIME_LIB_DIRS}\"")
      find_package(GTest REQUIRED)
      set(GTEST_FOUND TRUE)
    else()
      find_package(GTest)
    endif()
  endif()

  if(NOT GTEST_FOUND)
    message(STATUS "Lookup GoogleTest sources...")
    if(NOT DEFINED BUILD_GTEST)
      set(BUILD_GTEST
          ON
          CACHE BOOL "Builds the googletest subproject")
    endif()
    if(NOT DEFINED BUILD_GMOCK)
      set(BUILD_GMOCK
          OFF
          CACHE BOOL "Builds the googlemock subproject")
    endif()
    if(NOT DEFINED INSTALL_GTEST)
      set(INSTALL_GTEST
          OFF
          CACHE BOOL "Enable installation of googletest")
    endif()

    if(NOT gtest_paths)
      if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
        set(gtest_paths
            $ENV{SystemDrive}/
            $ENV{SystemDrive}/Source
            $ENV{USERPROFILE}/Source
            $ENV{PUBLIC}/Source
            $ENV{SystemDrive}/Source/Repos
            $ENV{USERPROFILE}/Source/Repos
            $ENV{PUBLIC}/Source/Repos)
      else()
        set(gtest_paths /usr/src /usr/local /usr/local/src)
      endif()
    endif()
    # message(STATUS "gtest_paths = ${gtest_paths}")

    find_path(
      gtest_root
      NAMES CMakeLists.txt
      PATHS ${gtest_paths} "${CMAKE_BINARY_DIR}/googletest-src"
      PATH_SUFFIXES googletest/googletest gtest/googletest gtest
      NO_DEFAULT_PATH NO_CMAKE_PATH)

    if(gtest_root)
      message(STATUS "Found GoogleTest sources at ${gtest_root}")
    elseif(CMAKE_VERSION VERSION_LESS 3.5)
      message(STATUS "GoogleTest not found, but to build it from source required CMake >= 3.5")
    else()
      if(NOT DEFINED GTEST_USE_VERSION
         OR GTEST_USE_VERSION STREQUAL "master"
         OR GTEST_USE_VERSION STREQUAL "main"
         OR GTEST_USE_VERSION STREQUAL "LAST_RELEASE")
        set(GTEST_CLONE_TAG "main")
      else()
        set(GTEST_CLONE_TAG "origin/${GTEST_USE_VERSION}")
      endif()
      if(NOT DEFINED GTEST_CLONE_URL)
        set(GTEST_CLONE_URL https://github.com/google/googletest.git)
      endif()

      message(
        STATUS
          "Not found GoogleTest sources, downloading it (GTEST_CLONE_URL: ${GTEST_CLONE_URL}, GTEST_CLONE_TAG: ${GTEST_CLONE_TAG})..."
      )
      configure_file(${CMAKE_CURRENT_LIST_DIR}/googletest-download.cmake.in
                     ${CMAKE_BINARY_DIR}/googletest-download/CMakeLists.txt)
      execute_process(
        COMMAND ${CMAKE_COMMAND} -G "${CMAKE_GENERATOR}" .
        RESULT_VARIABLE result
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/googletest-download)
      if(result)
        message(FATAL_ERROR "Prepare step for GoogleTest failed: ${result}")
      else()
        execute_process(
          COMMAND ${CMAKE_COMMAND} --build .
          RESULT_VARIABLE result
          WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/googletest-download)
        if(result)
          message(FATAL_ERROR "Download step for GoogleTest failed: ${result}")
        else()
          set(gtest_root "${CMAKE_BINARY_DIR}/googletest-src")
        endif()
      endif()
    endif()

    if(gtest_root)
      get_filename_component(gtest_root "${gtest_root}" REALPATH)
      if(EXISTS "${gtest_root}/../googlemock/CMakeLists.txt" AND EXISTS "${gtest_root}/../CMakeLists.txt")
        get_filename_component(gtest_root "${gtest_root}/.." ABSOLUTE)
      endif()

      if(DEFINED GTEST_USE_VERSION AND IS_DIRECTORY "${gtest_root}/.git")
        find_program(GIT git)
        if(GIT)
          if(GTEST_USE_VERSION STREQUAL "LAST_RELEASE")
            # get list of remote branches like 'v1.10.x'
            execute_process(
              COMMAND "${GIT}" branch --remote --sort=-version:refname --list "origin/v[0-9.]*.x" --no-color
              WORKING_DIRECTORY "${gtest_root}"
              RESULT_VARIABLE result
              OUTPUT_VARIABLE branch_list
              OUTPUT_STRIP_TRAILING_WHITESPACE)
            set(branch_last_release "")
            if(result EQUAL 0)
              string(REGEX REPLACE "\n" ";" branch_list "${branch_list}")
              list(LENGTH branch_list length)
              if(length GREATER 0)
                list(GET branch_list 0 branch_last_release)
                string(REGEX REPLACE "^  " "" branch_last_release "${branch_last_release}")
                message(STATUS "GoogleTest last like-release branch: ${branch_last_release}")
                execute_process(
                  COMMAND "${GIT}" checkout --detach "${branch_last_release}"
                  WORKING_DIRECTORY "${gtest_root}"
                  RESULT_VARIABLE result)
              endif()
            endif()
            if(branch_last_release STREQUAL "")
              # no suitable branch, get list of tags
              set(tag_last_release "")
              execute_process(
                COMMAND ${GIT} tag --sort=-version:refname
                WORKING_DIRECTORY "${gtest_root}"
                OUTPUT_VARIABLE tag_list
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE result)
              if(result EQUAL 0)
                string(REGEX REPLACE "\n" ";" tag_list "${tag_list}")
                list(LENGTH tag_list length)
                if(length GREATER 0)
                  list(GET tag_list 0 tag_last_release)
                  message(STATUS "GoogleTest last like-release tag: ${tag_last_release}")
                  execute_process(COMMAND "${GIT}" checkout --detach "${tag_last_release}"
                                  WORKING_DIRECTORY "${gtest_root}")
                endif()
              endif()
            endif()
          else()
            execute_process(COMMAND "${GIT}" checkout --detach "${GTEST_USE_VERSION}" WORKING_DIRECTORY "${gtest_root}")
          endif()
        endif(GIT)
      endif()

      unset(GTEST_INCLUDE_DIR CACHE)
      unset(GTEST_LIBRARY CACHE)
      unset(GTEST_LIBRARY_DEBUG CACHE)
      unset(GTEST_MAIN_LIBRARY CACHE)
      unset(GTEST_MAIN_LIBRARY_DEBUG CACHE)
      unset(GTEST_BOTH_LIBRARIES CACHE)
      unset(GTEST_FOUND CACHE)

      # Prevent overriding the parent project's compiler/linker settings on Windows
      set(gtest_force_shared_crt
          ON
          CACHE BOOL "" FORCE)

      # GoogleTest must NOT be gcov-instrumented: our global ENABLE_GCOV flags
      # (-fprofile-arcs -ftest-coverage, see cmake/compiler.cmake) would make the
      # gtest target pull its own __gcov_init/__gcov_exit (libgcov.a), which then
      # conflicts with the same symbols when the test executables link against
      # libmdbx + libgtest.dll ("multiple definition of '__gcov_init'").
      # Safely drop the flags for the duration of add_subdirectory(gtest).
      if(ENABLE_GCOV AND (MINGW OR MSYS OR CYGWIN))
        set(_saved_c_cxx_flags "${CMAKE_C_FLAGS}|${CMAKE_CXX_FLAGS}")
        string(REPLACE "-fprofile-arcs" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
        string(REPLACE "-ftest-coverage" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
        string(REPLACE "-fprofile-arcs" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        string(REPLACE "-ftest-coverage" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
      endif()

      # Add googletest directly to our build. This defines the gtest and gtest_main targets.
      add_subdirectory(${gtest_root} ${CMAKE_BINARY_DIR}/googletest-build EXCLUDE_FROM_ALL)

      if(ENABLE_GCOV AND (MINGW OR MSYS OR CYGWIN))
        string(REPLACE "|" ";" _saved_c_cxx "${_saved_c_cxx_flags}")
        list(GET _saved_c_cxx 0 CMAKE_C_FLAGS)
        list(GET _saved_c_cxx 1 CMAKE_CXX_FLAGS)
        unset(_saved_c_cxx)
        unset(_saved_c_cxx_flags)
      endif()
      if(CMAKE_INTERPROCEDURAL_OPTIMIZATION AND NOT CMAKE_VERSION VERSION_LESS 3.9)
        file(READ ${gtest_root}/CMakeLists.txt variable gtest_cmake_content)
        string(TOLOWER "${gtest_cmake_content}" gtest_cmake_content)
        string(FIND "${gtest_cmake_content}" "check_ipo_supported" gtest_ipo_supported)
        if(NOT gtest_ipo_supported OR gtest_ipo_supported LESS 1)
          message(
            STATUS
              "Disable INTERPROCEDURAL_OPTIMIZATION for GoogleTest (CMake-3.9's check_ipo_supported NOT FOUND inside)")
          if(TARGET gtest)
            set_property(TARGET gtest gtest_main PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
          endif()
          if(TARGET gmock)
            set_property(TARGET gmock gmock_main PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
          endif()
        endif()
      endif()
      if(TARGET gtest)
        set_target_properties(gtest gtest_main PROPERTIES SKIP_BUILD_RPATH FALSE MACOSX_RPATH TRUE)
      endif()
      if(TARGET gmock)
        set_target_properties(gmock gmock_main PROPERTIES SKIP_BUILD_RPATH FALSE MACOSX_RPATH TRUE)
      endif()

      list(FIND CMAKE_CXX_COMPILE_FEATURES cxx_std_11 local_HAS_CXX11)
      list(FIND CMAKE_CXX_COMPILE_FEATURES cxx_std_14 local_HAS_CXX14)
      list(FIND CMAKE_CXX_COMPILE_FEATURES cxx_std_17 local_HAS_CXX17)
      list(FIND CMAKE_CXX_COMPILE_FEATURES cxx_std_20 local_HAS_CXX20)
      list(FIND CMAKE_CXX_COMPILE_FEATURES cxx_std_23 local_HAS_CXX23)
      if(NOT DEFINED GTEST_CXX_STANDARD)
        if(DEFINED CMAKE_CXX_STANDARD)
          set(GTEST_CXX_STANDARD ${CMAKE_CXX_STANDARD})
        elseif(NOT local_HAS_CXX23 LESS 0 AND NOT (CMAKE_COMPILER_IS_CLANG AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS
                                                                               12))
          set(GTEST_CXX_STANDARD 23)
        elseif(NOT local_HAS_CXX20 LESS 0 AND NOT (CMAKE_COMPILER_IS_CLANG AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS
                                                                               10))
          set(GTEST_CXX_STANDARD 20)
        elseif(NOT local_HAS_CXX17 LESS 0 AND NOT (CMAKE_COMPILER_IS_CLANG AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 5
                                                  ))
          set(GTEST_CXX_STANDARD 17)
        elseif(NOT local_HAS_CXX14 LESS 0)
          set(GTEST_CXX_STANDARD 14)
        elseif(
          NOT local_HAS_CXX11 LESS 0
          OR CXX_FALLBACK_GNU11
          OR CXX_FALLBACK_11)
          set(GTEST_CXX_STANDARD 11)
        endif()
      endif()
      if(GTEST_CXX_STANDARD)
        message(STATUS "Use C++${GTEST_CXX_STANDARD} for GoogleTest")
        if(CMAKE_VERSION VERSION_LESS 3.1)
          if(CXX_FALLBACK_GNU11)
            target_compile_options(gtest PRIVATE "-std=gnu++${GTEST_CXX_STANDARD}")
            target_compile_options(gtest_main PRIVATE "-std=gnu++${GTEST_CXX_STANDARD}")
          else()
            target_compile_options(gtest PRIVATE "-std=c++${GTEST_CXX_STANDARD}")
            target_compile_options(gtest_main PRIVATE "-std=c++${GTEST_CXX_STANDARD}")
          endif()
        else()
          target_compile_features(gtest PRIVATE "cxx_std_${GTEST_CXX_STANDARD}")
          target_compile_features(gtest_main PRIVATE "cxx_std_${GTEST_CXX_STANDARD}")
        endif()
      endif()

      if(CC_HAS_WERROR)
        if(MSVC)
          set(local_warn_no_error "/WX-")
        else()
          set(local_warn_no_error "-Wno-error")
        endif()
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug"
           AND NOT CMAKE_INTERPROCEDURAL_OPTIMIZATION
           AND NOT LTO_ENABLED)
          if(CC_HAS_OMINIMAL)
            set(local_minimal_optimize "-Ominimal")
          elseif(CMAKE_COMPILER_IS_CLANG OR CMAKE_COMPILER_IS_GNUCC)
            set(local_minimal_optimize "-O1")
          else()
            set(local_minimal_optimize "")
          endif()
        endif()
        target_compile_options(gtest PRIVATE ${local_warn_no_error} ${local_minimal_optimize})
        target_compile_options(gtest_main PRIVATE ${local_warn_no_error} ${local_minimal_optimize})
        unset(local_warn_no_error)
        unset(local_minimal_optimize)
      endif()

      set(GTEST_BOTH_LIBRARIES gtest_main gtest)
      add_library(GTest::GTest ALIAS gtest)
      add_library(GTest::Main ALIAS gtest_main)
      set(GTEST_FOUND TRUE)
    endif()
  endif()

  if(GTEST_FOUND)
    if(CMAKE_VERSION VERSION_LESS 3.8)
      set(UT_LIBRARIES gtest_main gtest ${CMAKE_THREAD_LIBS_INIT})
    else()
      set(UT_LIBRARIES GTest::Main GTest::GTest ${CMAKE_THREAD_LIBS_INIT})
    endif()

    # ------------------------------------------------------------------------
    # GoogleTest advertises its own minimum standard as INTERFACE_COMPILE_FEATURES
    # `cxx_std_NN` (e.g. `cxx_std_17` for v1.15+). CMake raises a client target to
    # the maximum of its CXX_STANDARD and every linked INTERFACE feature, which
    # would silently build the tests at a newer standard than the library and,
    # worse, make the probe below measure that raised standard instead of the real
    # requirement.
    #
    # The imported GoogleTest targets are read-only and their feature demand cannot
    # be removed, so a private INTERFACE target is created instead: it carries the
    # include directories and links the real library files, but declares no
    # compile-feature requirement. Both the probe and the gtest test targets link
    # this wrapper, so the requested CXX_STANDARD pins the actual standard.
    # ------------------------------------------------------------------------
    if(GTEST_FOUND)
      # The system (imported) GoogleTest targets are read-only and publish
      # INTERFACE_COMPILE_FEATURES "cxx_std_NN" (17 for v1.15+, 14 for 1.14).
      # Leaving them linked as targets would silently raise every client that
      # requests C++11/14 to the GoogleTest minimum, desynchronizing the tests
      # from the library. They are already compiled, so only the feature demand
      # matters for clients: a wrapper INTERFACE target links the real library
      # files via "$<TARGET_FILE:...>", which carries the link but no feature
      # demand, and pins the requested standard.
      #
      # The bundled (add_subdirectory) GoogleTest is built with
      # GTEST_CXX_STANDARD = the requested standard (see below) and is only
      # enabled when the requested standard is at least GoogleTest's minimum
      # (the probe gates it). Its own PUBLIC cxx_std_NN then matches or is
      # below the requested standard, so the plain targets can be linked
      # directly without a wrapper or a double -l.
      foreach(_gt_candidate gtest_main gtest)
        if(TARGET ${_gt_candidate})
          set(_gtest_is_bundled TRUE)
          break()
        endif()
      endforeach()
      unset(_gt_candidate)

      if(_gtest_is_bundled)
        # Bundled mode: plain target link, no wrapper needed.
        # gtest_main нет: общий main — в tests/ut/test-env.c++ (см. UT_LIBRARIES).
        set(UT_LIBRARIES gtest ${CMAKE_THREAD_LIBS_INIT})
      else()
        # System mode: wrapper over the real library files.
        add_library(mdbx_gtest_link INTERFACE)
        set(gtest_link_files "")
        set(gtest_include_dirs "")
        foreach(_gt_candidate GTest::gtest_main GTest::gtest gtest_main gtest)
          if(TARGET ${_gt_candidate})
            list(APPEND gtest_link_files "$<TARGET_FILE:${_gt_candidate}>")
          endif()
        endforeach()
        foreach(_gt_candidate GTest::gtest GTest::GTest gtest)
          if(TARGET ${_gt_candidate})
            get_target_property(_gt_includes ${_gt_candidate} INTERFACE_INCLUDE_DIRECTORIES)
            if(_gt_includes)
              list(APPEND gtest_include_dirs ${_gt_includes})
            endif()
            unset(_gt_includes)
          endif()
        endforeach()
        if(gtest_include_dirs)
          target_include_directories(mdbx_gtest_link INTERFACE ${gtest_include_dirs})
        endif()
        if(gtest_link_files)
          list(REMOVE_DUPLICATES gtest_link_files)
          target_link_libraries(mdbx_gtest_link INTERFACE ${gtest_link_files} ${CMAKE_THREAD_LIBS_INIT})
        endif()
        unset(gtest_link_files)
        unset(gtest_include_dirs)
        set(UT_LIBRARIES mdbx_gtest_link ${CMAKE_THREAD_LIBS_INIT})
      endif()
      unset(_gtest_is_bundled)
    endif()

    # ------------------------------------------------------------------------
    # Discover the minimum C++ standard the *already built* GoogleTest accepts.
    #
    # GoogleTest 1.15+ hard-requires C++17 (`#error C++ versions less than
    # C++17 are not supported` in gtest-port.h) and publishes that as an
    # INTERFACE_COMPILE_FEATURES `cxx_std_17` requirement, which CMake silently
    # raises every client target to — even when the user asked for C++11/14.
    # That desynchronized the library (built at the requested standard) from
    # the tests (built at C++17), so `mdbx::default_allocator` and
    # `mdbx::path` differed between them and linking failed with undefined
    # `buffer<polymorphic_allocator>` / `env::remove(filesystem::path)` symbols.
    #
    # The version is therefore not assumed but probed from the real headers, so
    # an older GoogleTest (1.14 needs C++14, <=1.12 needs C++11) is used to its
    # full extent. Nothing is downloaded: only the GoogleTest that was found.
    # ------------------------------------------------------------------------
    set(gtest_probe_target "")
    foreach(_gt_candidate GTest::gtest GTest::GTest gtest)
      if(TARGET ${_gt_candidate})
        set(gtest_probe_target ${_gt_candidate})
        break()
      endif()
    endforeach()

    if(gtest_probe_target AND CMAKE_CXX_COMPILER_LOADED)
      get_target_property(gtest_probe_includes ${gtest_probe_target} INTERFACE_INCLUDE_DIRECTORIES)
      include(CMakePushCheckState)
      include(CheckCXXSourceCompiles)

      # The probe sources are tiny; compile them quietly to keep the configure
      # log readable.
      set(gtest_probe_code [[
#include <gtest/gtest.h>
/* Defines exactly the compile-time requirement of GoogleTest's headers; the
 * program intentionally never references any testing::* symbol, so this probe
 * does not need to link against the prebuilt GoogleTest libraries. */
int main() { return 0; }
]])

      set(GTEST_MIN_CXX_STANDARD "")
      # Probe the user-requested standard first (the common case), then walk up
      # to report an accurate minimum in the diagnostic message.
      set(gtest_probe_standards "")
      if(MDBX_CXX_STANDARD AND NOT MDBX_CXX_STANDARD LESS 11)
        list(APPEND gtest_probe_standards ${MDBX_CXX_STANDARD})
      endif()
      foreach(_std 11 14 17 20 23)
        list(FIND gtest_probe_standards ${_std} _already)
        if(_already LESS 0)
          list(APPEND gtest_probe_standards ${_std})
        endif()
      endforeach()

      foreach(_std IN LISTS gtest_probe_standards)
        if(NOT GTEST_MIN_CXX_STANDARD)
          # check_cxx_source_compiles() ignores CMAKE_CXX_STANDARD, so the
          # standard must be spelled out as an explicit flag — otherwise the
          # probe runs at the compiler default and reports a false success.
          # For GCC/Clang the explicit flag is mandatory: an empty (default)
          # flag would compile at gnu++17 and wrongly "pass" C++11. Only MSVC
          # may fall back to no flag, because it has no /std:c++11 and CMake
          # relies on its default level (C++14) for the older standards.
          set(gtest_probe_flags "")
          if(MSVC)
            if(_std EQUAL 23)
              set(gtest_probe_flags "/std:c++latest")
            elseif(NOT _std LESS 14)
              set(gtest_probe_flags "/std:c++${_std}")
            else()
              set(gtest_probe_flags "/std:c++14")
            endif()
            # MSVC default level is the last-resort candidate for C++11/14.
            list(APPEND gtest_probe_flags "")
          elseif(_std EQUAL 23)
            set(gtest_probe_flags "-std=gnu++23;-std=gnu++2b;-std=c++23;-std=c++2b")
          elseif(CXX_FALLBACK_GNU11)
            set(gtest_probe_flags "-std=gnu++${_std}")
          else()
            set(gtest_probe_flags "-std=c++${_std}")
          endif()

          cmake_push_check_state()
          set(CMAKE_CXX_STANDARD ${_std})
          set(CMAKE_CXX_STANDARD_REQUIRED ON)
          set(CMAKE_CXX_EXTENSIONS ON)
          if(gtest_probe_includes)
            set(CMAKE_REQUIRED_INCLUDES ${gtest_probe_includes})
          endif()
          # No linking needed: the probe only references GoogleTest's headers.

          set(gtest_probe_ok "")
          foreach(_flag IN LISTS gtest_probe_flags)
            if(NOT gtest_probe_ok)
              # Build a cache-safe variable name from the flag.
              string(REGEX REPLACE "[^A-Za-z0-9]" "" _flag_id "${_flag}")
              if(NOT _flag_id)
                set(_flag_id "compilerdefault")
              endif()
              set(CMAKE_REQUIRED_FLAGS ${_flag})
              check_cxx_source_compiles("${gtest_probe_code}" GTEST_WORKS_AT_CXX${_std}_${_flag_id})
              if(GTEST_WORKS_AT_CXX${_std}_${_flag_id})
                set(gtest_probe_ok TRUE)
              endif()
            endif()
          endforeach()
          cmake_pop_check_state()

          if(gtest_probe_ok)
            set(GTEST_MIN_CXX_STANDARD ${_std})
          endif()
        endif()
      endforeach()
      unset(gtest_probe_standards)
      unset(gtest_probe_flags)
      unset(gtest_probe_ok)
      unset(gtest_probe_code)
      unset(gtest_probe_includes)
      unset(gtest_probe_target)
      unset(_std)
      unset(_flag)
      unset(_flag_id)

      if(NOT GTEST_MIN_CXX_STANDARD)
        message(
          WARNING
            "GoogleTest is unusable with any C++ standard from 11 to 23; the gtest-based tests will not be built.")
      elseif(MDBX_CXX_STANDARD AND MDBX_CXX_STANDARD LESS GTEST_MIN_CXX_STANDARD)
        message(
          STATUS
            "GoogleTest requires C++${GTEST_MIN_CXX_STANDARD}, but C++${MDBX_CXX_STANDARD} was requested: "
            "the gtest-based tests are skipped, while the library, the C++ API, the examples and the "
            "`mdbx_test` framework are still built and tested at C++${MDBX_CXX_STANDARD}.")
        set(GTEST_FOUND FALSE)
      else()
        message(STATUS "GoogleTest is usable at C++${GTEST_MIN_CXX_STANDARD}")
      endif()
    endif()
  endif()

  if(GTEST_FOUND)
    if(MEMORYCHECK_COMMAND OR CMAKE_MEMORYCHECK_COMMAND)
      add_custom_target(
        test_memcheck
        COMMAND ${CMAKE_CTEST_COMMAND} --force-new-ctest-process --test-action memcheck
        COMMAND ${CAT} "${CMAKE_BINARY_DIR}/Testing/Temporary/MemoryChecker.*.log")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      # Windows don't have RPATH feature, therefore we should prepare PATH or copy DLL(s)
      set(UT_NEED_DLLCRUTCH "Crutch for ${CMAKE_SYSTEM_NAME}")
    else()
      set(UT_NEED_DLLCRUTCH FALSE)
    endif()

    # The googletest DLLs are placed into ${CMAKE_BINARY_DIR}/bin while the test
    # executables live in ${MDBX_OUTPUT_DIR}. Register a pseudo-test that copies
    # the DLLs next to the executables and make every gtest-based test wait for it
    # first (via ctest fixtures), so the DLLs are always findable at process startup.
    # Multi-config generators (VS) append a per-config subdirectory to both the
    # source (bin/<Config>) and the destination (MDBX_OUTPUT_DIR/<Config>); the
    # old code copied at build time per ut_* target instead, and msbuild /m raced
    # those copies onto the same file ("Permission denied", and on early runs the
    # copy could fire before gtest DLLs were linked, leaving tests unable to load
    # them). The copy now happens exactly once at ctest time, after the full build,
    # through this config-aware fixture.
    set(UT_DLL_COPY_TEST "")
    if(UT_NEED_DLLCRUTCH AND NOT CMAKE_CROSSCOMPILING AND DEFINED MDBX_OUTPUT_DIR)
      get_target_property(gtest_rt_dir gtest RUNTIME_OUTPUT_DIRECTORY)
      if(NOT gtest_rt_dir)
        set(gtest_rt_dir "${CMAKE_BINARY_DIR}/bin")
      endif()
      set(ut_dll_src "${gtest_rt_dir}")
      set(ut_dll_dst "${MDBX_OUTPUT_DIR}")
      if(CMAKE_CONFIGURATION_TYPES)
        set(ut_dll_src "${gtest_rt_dir}/$<CONFIG>")
        set(ut_dll_dst "${MDBX_OUTPUT_DIR}/$<CONFIG>")
      endif()
      add_test(
        NAME ut_copy_dlls
        COMMAND ${CMAKE_COMMAND} -Ddest_dir=${ut_dll_dst} -Dsrc_dirs=${ut_dll_src}
                -P "${CMAKE_CURRENT_LIST_DIR}/copy-test-dlls.cmake")
      set_tests_properties(ut_copy_dlls PROPERTIES LABELS "ut;ut.api" TIMEOUT 60 FIXTURES_SETUP ut-dlls)
      set(UT_DLL_COPY_TEST ut_copy_dlls)
    endif()

    # Shared test-environment TU (tests/ut/test-env.c++): the single
    # ci_runtime_limiter instance (GTEST_RUNTIME_LIMIT / GTEST_RUNTIME_DEADLINE
    # guard) and the single CI-only CRT popup suppressor. It is compiled ONCE
    # into an OBJECT library and that object is linked into every gtest target
    # via $<TARGET_OBJECTS:...>. An object library (not a static one) is used
    # deliberately: its object goes straight onto the link line of every
    # consumer, so the linker cannot drop it from binaries that never reference
    # the limiter — which would otherwise silently disable the popup
    # suppression. Compiling it once instead of per target avoids rebuilding the
    # same translation unit for each of the ~120 gtest binaries.
    if(CMAKE_CXX_COMPILER_LOADED AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../tests/ut/test-env.c++")
      set(UT_TEST_ENV_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../tests/ut/test-env.c++")
      set(UT_TEST_ENV_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../tests/ut")
      add_library(mdbx_test_env OBJECT "${UT_TEST_ENV_SOURCE}")
      target_include_directories(mdbx_test_env PUBLIC "${UT_TEST_ENV_INCLUDE_DIR}")
      target_link_libraries(mdbx_test_env PUBLIC ${UT_LIBRARIES})
      if(MDBX_BUILD_CXX AND MDBX_CXX_STANDARD)
        set_target_properties(mdbx_test_env PROPERTIES CXX_STANDARD ${MDBX_CXX_STANDARD}
                                                       CXX_STANDARD_REQUIRED ON)
      endif()
      set(UT_TEST_ENV_OBJECT "$<TARGET_OBJECTS:mdbx_test_env>")
    else()
      set(UT_TEST_ENV_SOURCE "")
      set(UT_TEST_ENV_INCLUDE_DIR "")
      set(UT_TEST_ENV_OBJECT "")
    endif()

    # ------------------------------------------------------------------------
    # Shared precompiled header for the gtest binaries.
    #
    # Parsing GoogleTest + the libmdbx APIs + the common standard headers costs
    # ~2 s per translation unit, and each gtest binary is a single TU, so a
    # clean build re-parses them ~120 times. Precompiling them once into an
    # anchor library and letting every test reuse that PCH via REUSE_FROM turns
    # those minutes into a single parse.
    #
    # The anchor mirrors the gtest targets' compile flags (standard, include
    # dirs, PIC/PIE); the mechanics live in cmake/pch.cmake and are shared with
    # the examples-PCH (see examples/CMakeLists.txt). REUSE_FROM only reuses a
    # PCH when the flags match; a target whose flags differ (e.g. the
    # MDBX_PROBES ones) simply does not reuse the PCH — GCC then falls back to
    # a normal compile without an error, so this is safe.
    if(UT_TEST_ENV_SOURCE)
      set(UT_PCH_ANCHOR_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../tests/ut/pch_anchor.cxx")
      set(UT_PCH_HEADER "${CMAKE_CURRENT_LIST_DIR}/../tests/ut/pch.h++")
      mdbx_add_pch_anchor(mdbx_test_pch "${UT_PCH_HEADER}" "${UT_PCH_ANCHOR_SOURCE}"
                          CXX_STANDARD ${MDBX_CXX_STANDARD}
                          INCLUDE ${UT_TEST_ENV_INCLUDE_DIR}
                          LINK ${UT_LIBRARIES}
                          LINK_PRIVATE ${TOOL_MDBX_LIB})
      # Local alias for the call sites in add_gtest()/add_issue_test().
      if(NOT COMMAND ut_apply_pch)
        macro(ut_apply_pch target)
          mdbx_test_pch_apply(${target})
        endmacro()
      endif()
    endif()
  else()
    set(UT_INCLUDE_DIRECTORIES "")
    set(UT_LIBRARIES "")
    message(STATUS "GoogleTest NOT available, so testing NOT be ENABLED")
  endif()

  # No-op fallback when the real helper was not defined above (MDBX_ENABLE_PCH=OFF,
  # CMake < 3.16, no GoogleTest, or no test-env TU): the gate is expressed purely
  # by whether mdbx_test_pch_apply is a no-op or not, so call sites never branch.
  if(NOT COMMAND ut_apply_pch)
    macro(ut_apply_pch target)
    endmacro()
  endif()

  function(add_gtest name)
    set(options DISABLED NO_PCH)
    set(oneValueArgs TIMEOUT PREFIX)
    set(multiValueArgs SOURCE LIBRARY INCLUDE_DIRECTORY DEPEND DLLPATH LABELS)
    cmake_parse_arguments(params "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(params_UNPARSED_ARGUMENTS)
      message(FATAL_ERROR "Unknown keywords given to add_gtest(): \"${params_UNPARSED_ARGUMENTS}\".")
    endif()

    if(GTEST_FOUND)
      macro(oops)
        message(FATAL_ERROR "add_gtest(): Opps, " ${ARGV})
      endmacro()

      if(NOT params_SOURCE)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}.c++")
          set(params_SOURCE ${name}.c++)
        else()
          set(params_SOURCE ${name}.cpp)
        endif()
      endif()

      set(target "${params_PREFIX}${name}")
      if(UT_TEST_ENV_OBJECT)
        add_executable(${target} ${params_SOURCE} ${UT_TEST_ENV_OBJECT})
        target_include_directories(${target} PRIVATE ${UT_TEST_ENV_INCLUDE_DIR})
      else()
        add_executable(${target} ${params_SOURCE})
      endif()
      set_target_properties(${target} PROPERTIES SKIP_BUILD_RPATH FALSE BUILD_WITH_INSTALL_RPATH FALSE)

      if(MSVC AND MSVC_C11_MAD_ATOMICS)
        # Test targets that #include library sources directly (e.g. details_rkl.c
        # which compiles src/rkl.c and src/txl.c) need C11 atomics on MSVC for
        # non-x86 targets. Apply the plain flag (not via a COMPILE_LANGUAGE
        # generator expression: that form was observed to silently no-op for
        # subdirectory targets under the Visual Studio generator), exactly as the
        # library does in target_setup_options() of the root CMakeLists.txt.
        # C_STANDARD is required too: without /std:c11+ cl.exe keeps
        # __STDC_VERSION__ at 199901L, so atomics-types.h does not define
        # MDBX_HAVE_C11ATOMICS and atomics-ops.h:46 aborts with C1189 on
        # non-x86 targets even when /experimental:c11atomics is passed (TASK-17).
        set_target_properties(${target} PROPERTIES C_STANDARD ${MDBX_C_STANDARD} C_STANDARD_REQUIRED ON)
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
      endif()

      if(params_DEPEND)
        add_dependencies(${target} ${params_DEPEND})
      endif()

      target_link_libraries(${target} ${UT_LIBRARIES})

      if(NOT params_LIBRARY AND DEFINED TOOL_MDBX_LIB)
        set(params_LIBRARY ${TOOL_MDBX_LIB})
      endif()

      if(params_LIBRARY)
        target_link_libraries(${target} ${params_LIBRARY})
      endif()

      if(MDBX_BUILD_CXX AND MDBX_CXX_STANDARD)
        set_target_properties(${target} PROPERTIES CXX_STANDARD ${MDBX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)
      endif()

      if(params_INCLUDE_DIRECTORY)
        set_target_properties(${target} PROPERTIES INCLUDE_DIRECTORIES ${params_INCLUDE_DIRECTORY})
      endif()

      # Reuse the shared precompiled header. Called after the standard and
      # include dirs are set so the flags match the anchor and the PCH is reused.
      # The ut_apply_pch macro is a no-op unless CMake >= 3.16 built the anchor.
      # NO_PCH opts a target out: the white-box tests include src/internals.h,
      # whose osal.h redefines snprintf (the ntdll fallback in WITHOUT_MSVC_CRT
      # builds) and other macros that clash with the already-parsed std headers
      # baked into the PCH.
      if(NOT params_NO_PCH)
        ut_apply_pch(${target})
      endif()

      if(UT_NEED_DLLCRUTCH)
        if(CMAKE_BUILD_TYPE)
          string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPERCASE)
        else()
          set(CMAKE_BUILD_TYPE_UPPERCASE "")
        endif()
        foreach(dep IN LISTS params_LIBRARY GTEST_BOTH_LIBRARIES)
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
                # For a shared library on Windows the DLL itself is placed into
                # RUNTIME_OUTPUT_DIRECTORY, while LIBRARY_OUTPUT_DIRECTORY holds
                # only the import library (lib/), so query the runtime dir first.
                get_target_property(dir ${dep} RUNTIME_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE_UPPERCASE})
                if(NOT dir)
                  get_target_property(dir ${dep} RUNTIME_OUTPUT_DIRECTORY)
                endif()
                if(NOT dir)
                  get_target_property(dir ${dep} LIBRARY_OUTPUT_DIRECTORY_${CMAKE_BUILD_TYPE_UPPERCASE})
                endif()
                if(NOT dir)
                  get_target_property(dir ${dep} LIBRARY_OUTPUT_DIRECTORY)
                endif()
              endif(filename)
            endif(CMAKE_CONFIGURATION_TYPES)
            if(dir)
              list(APPEND params_DLLPATH ${dir})
            else()
              # Path is configuration-dependent or not available: no build-time
              # copy is performed here. The ut_copy_dlls ctest fixture (above)
              # copies the shared libraries next to the executables after the
              # full build, which also removes the old per-target POST_BUILD
              # race under msbuild /m ("Permission denied").
            endif(dir)
          endif()
        endforeach(dep)
      endif(UT_NEED_DLLCRUTCH)

      if(NOT params_DISABLED AND NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR))
        add_test(NAME ${name} COMMAND $<TARGET_FILE:${target}>)
        if(params_LABELS)
          set_tests_properties(${name} PROPERTIES LABELS "${params_LABELS}")
        endif()
        if(UT_DLL_COPY_TEST)
          # The `ut_copy_dlls` pseudo-test copies the shared libraries required at
          # runtime (e.g. googletest DLLs) next to the executables; run it first
          # via a ctest fixture so it is honored even with -R/-L test selection.
          set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED ut-dlls)
        endif()
        if(params_TIMEOUT)
          if(MEMORYCHECK_COMMAND OR CMAKE_MEMORYCHECK_COMMAND)
            # FIXME: unless there are any other ideas how to fix the timeouts problem when testing under Valgrind.
            math(EXPR params_TIMEOUT "${params_TIMEOUT} * 42")
          endif()
          set_tests_properties(${name} PROPERTIES TIMEOUT ${params_TIMEOUT})
        endif()
        if(params_DLLPATH)
          # Compose DLL's path in the ENVIRONMENT property
          if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(params_DLLPATH_ENV "${params_DLLPATH};$ENV{PATH}")
          else()
            set(params_DLLPATH_ENV "${params_DLLPATH}:$ENV{PATH}")
            string(REPLACE ":" ";" params_DLLPATH_ENV "${params_DLLPATH_ENV}")
          endif()
          list(REMOVE_DUPLICATES params_DLLPATH_ENV)
          if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            string(REPLACE ";" "\\;" params_DLLPATH_ENV "${params_DLLPATH_ENV}")
          else()
            string(REPLACE ";" ":" params_DLLPATH_ENV "${params_DLLPATH_ENV}")
          endif()
          set_tests_properties(${name} PROPERTIES ENVIRONMENT "PATH=${params_DLLPATH_ENV}")
        endif()
      endif()
    else()
      # GoogleTest is unavailable, or requires a newer C++ standard than the one
      # requested: register a stub test that reports why it is skipped, so that
      # neither the build nor the surrounding set_tests_properties() calls of the
      # caller break. The library, the C++ API, the examples and the non-gtest
      # `mdbx_test` framework are still built and tested at the requested standard.
      if(GTEST_MIN_CXX_STANDARD)
        set(gtest_skip_reason
            "GoogleTest requires C++${GTEST_MIN_CXX_STANDARD}, but C++${MDBX_CXX_STANDARD} was requested")
      else()
        set(gtest_skip_reason "GoogleTest is not available")
      endif()
      add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -E echo "SKIPPED: ${name}: ${gtest_skip_reason}")
      set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 0)
      if(params_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${params_LABELS}")
      endif()
      unset(gtest_skip_reason)
    endif()
  endfunction(add_gtest)

  function(add_ut name)
    add_gtest(${name} PREFIX "ut_" LABELS "ut" ${ARGN})
  endfunction(add_ut)

  function(add_long_test name)
    add_gtest(${name} PREFIX "lt_" DISABLED ${ARGN})
  endfunction(add_long_test)

  function(add_perf_test name)
    add_gtest(${name} PREFIX "pt_" DISABLED ${ARGN})
  endfunction(add_perf_test)

endif(BUILD_TESTING)

cmake_policy(POP)
