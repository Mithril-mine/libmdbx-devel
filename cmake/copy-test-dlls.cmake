# Helper for the `ut_copy_dlls` pseudo-test: copies the shared libraries (DLLs)
# required by the tests at runtime into the directory where the test executables
# are placed.
#
# Usage:
#   cmake -Ddest_dir=<absolute-dir> -Dsrc_dirs="dir1;dir2" -P copy-test-dlls.cmake

if(NOT DEFINED dest_dir OR NOT DEFINED src_dirs)
  message(FATAL_ERROR "Usage: cmake -Ddest_dir=<dir> -Dsrc_dirs=\"d1;d2\" -P copy-test-dlls.cmake")
endif()

foreach(src IN LISTS src_dirs)
  if(NOT IS_DIRECTORY "${src}")
    continue()
  endif()
  file(GLOB dlls "${src}/*.dll")
  foreach(dll IN LISTS dlls)
    get_filename_component(name "${dll}" NAME)
    file(COPY "${dll}" DESTINATION "${dest_dir}")
    message(STATUS "Copied ${name} -> ${dest_dir}")
  endforeach()
endforeach()