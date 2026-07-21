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

set(LIBMDBX_TEST_SOURCES
    base.h++
    cases.c++
    chrono.c++
    chrono.h++
    config.c++
    config.h++
    copy.c++
    dead.c++
    hill.c++
    jitter.c++
    keygen.c++
    keygen.h++
    log.c++
    log.h++
    main.c++
    osal.h++
    osal-unix.c++
    osal-windows.c++
    test.c++
    test.h++
    try.c++
    utils.c++
    utils.h++
    append.c++
    ttl.c++
    nested.c++
    fork.c++
    ../../src/essentials.h
    ../../src/osal.h
    ../../src/options.h
    ../../mdbx.h++)

if(NOT MDBX_BUILD_CXX)
  probe_libcxx_filesystem()
  list(APPEND LIBMDBX_TEST_SOURCES "${MDBX_SOURCE_DIR}/mdbx.c++")
endif()

add_executable(mdbx_test ${LIBMDBX_TEST_SOURCES})
target_compile_definitions(mdbx_test PRIVATE MDBX_BUILD_TEST=1 MDBX_BUILD_CXX=1)

if(MDBX_CXX_STANDARD)
  set_target_properties(mdbx_test PROPERTIES CXX_STANDARD ${MDBX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)
endif()

set_target_properties(mdbx_test PROPERTIES INTERPROCEDURAL_OPTIMIZATION $<BOOL:${INTERPROCEDURAL_OPTIMIZATION}>)
target_setup_options(mdbx_test)

if(NOT MDBX_BUILD_CXX)
  target_compile_definitions(mdbx_test PRIVATE MDBX_BUILD_CXX=1)
  if(WIN32)
    target_compile_definitions(mdbx_test PRIVATE MDBX_WITHOUT_MSVC_CRT=0)
  endif()
endif()

if(NOT MDBX_BUILD_CXX AND LIBCXX_FILESYSTEM)
  if(CMAKE_COMPILER_IS_ELBRUSCXX
     AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 1.25.23
     AND NOT CMAKE_VERSION VERSION_LESS 3.13)
    target_link_options(mdbx_test PRIVATE "-Wl,--allow-multiple-definition")
  endif()
  target_link_libraries(mdbx_test ${LIBCXX_FILESYSTEM})
endif()

if(CMAKE_VERSION VERSION_LESS 3.1)
  target_link_libraries(mdbx_test ${TOOL_MDBX_LIB} ${LIB_MATH} ${CMAKE_THREAD_LIBS_INIT})
else()
  target_link_libraries(mdbx_test ${TOOL_MDBX_LIB} ${LIB_MATH} Threads::Threads)
endif()
if(WIN32)
  target_link_libraries(mdbx_test winmm.lib)
endif()
