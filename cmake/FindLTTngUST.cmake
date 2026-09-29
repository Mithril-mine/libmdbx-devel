# FindLTTngUST.cmake — поиск LTTng-UST (Linux).
# Ищет lttng/tracepoint.h (включая multiarch), liblttng-ust, liblttng-ust-ctl.
# Экспортирует: LTTNG_UST_FOUND, LTTNG_UST_INCLUDE_DIR, LTTNG_UST_LIBRARIES,
# целевой LTTng::UST.

find_path(LTTNG_UST_INCLUDE_DIR NAMES lttng/tracepoint.h)

# multiarch fallback (Ubuntu/Debian: /usr/include/<triplet>/lttng/)
if(NOT LTTNG_UST_INCLUDE_DIR)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-multiarch
    OUTPUT_VARIABLE _lttng_triplet
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_lttng_triplet)
    find_path(LTTNG_UST_INCLUDE_DIR NAMES lttng/tracepoint.h
              PATHS "/usr/include/${_lttng_triplet}"
              NO_DEFAULT_PATH)
  endif()
endif()

find_library(LTTNG_UST_LIBRARY NAMES lttng-ust)
find_library(LTTNG_UST_CTL_LIBRARY NAMES lttng-ust-ctl)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LTTngUST DEFAULT_MSG
  LTTNG_UST_LIBRARY LTTNG_UST_INCLUDE_DIR)

# Каноничное имя FOUND-переменной совпадает с именем пакета; для удобства
# потребителей дублируем в LTTNG_UST_FOUND.
set(LTTNG_UST_FOUND ${LTTngUST_FOUND})

if(LTTNG_UST_FOUND AND NOT TARGET LTTng::UST)
  set(LTTNG_UST_LIBRARIES ${LTTNG_UST_LIBRARY})
  if(LTTNG_UST_CTL_LIBRARY)
    list(APPEND LTTNG_UST_LIBRARIES ${LTTNG_UST_CTL_LIBRARY})
  endif()
  add_library(LTTng::UST UNKNOWN IMPORTED)
  set_target_properties(LTTng::UST PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LTTNG_UST_INCLUDE_DIR}"
    IMPORTED_LOCATION "${LTTNG_UST_LIBRARY}")
endif()

mark_as_advanced(LTTNG_UST_INCLUDE_DIR LTTNG_UST_LIBRARY LTTNG_UST_CTL_LIBRARY)