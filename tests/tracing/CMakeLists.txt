# tests/tracing — managed/controlled testing instrumentation (dev-only).
# См. docs/engineering/testing-methodology.md, гл. 9-10.
# Подключается из корневого CMakeLists.txt под MDBX_BUILD_TRACING (dist-cutoff).

option(ENABLE_LTTNG "Enable LTTng-UST tracepoints for MPROBE_COLLECT/WATCH (Linux)" OFF)

set(MPROBE_TRACING_SOURCES mprobe_providers.c mprobe_test.c)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  # LTTng-UST: канал метрик (COLLECT/WATCH)
  if(ENABLE_LTTNG)
    list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
    find_package(LTTngUST)
    if(NOT LTTNG_UST_FOUND)
      message(WARNING "mprobe: LTTng-UST not found; ENABLE_LTTNG ignored")
    else()
      list(APPEND MPROBE_TRACING_SOURCES mprobe_lttng_provider.c)
    endif()
  endif()
  # systemtap-sdt-dev (sys/sdt.h): USDT для MPROBE_FAULT
  if(ENABLE_SYSTEMTAP)
    find_path(SYSTEMTAP_SDT_INCLUDE_DIR NAMES sys/sdt.h)
    if(NOT SYSTEMTAP_SDT_INCLUDE_DIR)
      message(WARNING
        "mprobe: sys/sdt.h not found (install systemtap-sdt-dev); USDT disabled")
    endif()
  endif()
elseif(WIN32)
  # ETW в advapi32; Detours — FetchContent для тестовых целей
  include(FetchContent)
  FetchContent_Declare(
    detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        v4.0.1)
  FetchContent_GetProperties(detours)
  if(NOT detours_POPULATED)
    FetchContent_Populate(detours)
  endif()
  add_library(detours STATIC
    ${detours_SOURCE_DIR}/src/detours.cpp
    ${detours_SOURCE_DIR}/src/disasm.cpp
    ${detours_SOURCE_DIR}/src/modules.cpp
    ${detours_SOURCE_DIR}/src/creatwth.cpp)
  target_include_directories(detours PUBLIC ${detours_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(detours PRIVATE /W3 /wd4100 /wd4127)
  endif()
endif()

add_executable(mprobe_test ${MPROBE_TRACING_SOURCES})
target_include_directories(mprobe_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  if(ENABLE_LTTNG AND TARGET LTTng::UST)
    target_compile_definitions(mprobe_test PRIVATE ENABLE_LTTNG=1)
    target_link_libraries(mprobe_test PRIVATE LTTng::UST)
    target_include_directories(mprobe_test PRIVATE ${LTTNG_UST_INCLUDE_DIR})
  endif()
  if(ENABLE_SYSTEMTAP AND SYSTEMTAP_SDT_INCLUDE_DIR)
    target_compile_definitions(mprobe_test PRIVATE ENABLE_SYSTEMTAP=1)
    target_include_directories(mprobe_test PRIVATE ${SYSTEMTAP_SDT_INCLUDE_DIR})
  endif()
elseif(WIN32)
  target_compile_definitions(mprobe_test PRIVATE MPROBE_USE_DETOURS=1)
  target_link_libraries(mprobe_test PRIVATE detours advapi32)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mprobe_test PRIVATE -Wall -Wextra)
endif()

include(CTest)
add_test(NAME mprobe_tracing COMMAND mprobe_test)