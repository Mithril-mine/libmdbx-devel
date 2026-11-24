macro(add_durability_test)
  add_gtest(${ARGV} PREFIX "durability_" LABELS "ut;ut.durability")
endmacro(add_durability_test)

# The crash-storm (the durability test program, issue #564): the multi-
# writer forked victims and the kill-rule; the fork-based victim requires
# fork(), so the corpus is POSIX-only, the same gating as the crash-window
# matrix and the dead-writer recovery matrix registered in tests/CMakeLists.txt.
if(NOT WIN32)
  # NO_PCH: the white-box essentials.h include (the resolved MDBX_LOCKING
  # for the strand-modes skip, the same as dead-owner.c++ does) clashes with
  # the shared precompiled header. That include pulls config-cmake.h, which
  # defines MDBX_PROBES in the probes build - so the command-line definition
  # must NOT be added here (it is a redefinition under -Werror), only the
  # runtime MDBX_PROBES=1 activation for the forked children stays below.
  add_durability_test(crash-storm SOURCE "crash-storm.c++" NO_PCH)
  if(MDBX_PROBES)
    # APPEND: the exported MDBX_TEST_TIMEOUT_SCALE (and the PATH on Windows
    # setups) must survive, the same as the other (re)defining callers.
    set_property(TEST crash-storm APPEND PROPERTY ENVIRONMENT "MDBX_PROBES=1")
  endif()
endif()

# Scale the registered tests' TIMEOUTs by the platform/configuration factor
# (see cmake/test-timeout-scale.cmake).
mdbx_scale_dir_test_timeouts()
