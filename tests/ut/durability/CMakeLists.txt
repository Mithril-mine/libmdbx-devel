macro(add_durability_test)
  add_gtest(${ARGV} PREFIX "durability_" LABELS "ut;ut.durability")
endmacro(add_durability_test)

# The crash-window matrix (the durability test program, issue #564): the
# fork-based victim requires fork(), so the whole corpus is POSIX-only; the
# multi-actor Windows coverage stays with the mdbx_test framework's
# --inject-writefault smoke scenarios.
if(NOT WIN32)
  add_durability_test(crash-windows SOURCE "crash-windows.c++")
  if(MDBX_PROBES)
    target_compile_definitions(durability_crash-windows PRIVATE MDBX_PROBES)
    set_tests_properties(crash-windows PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  endif()
  # The white-box include of src/essentials.h (to learn the resolved
  # MDBX_LOCKING) requires the NO_PCH setup.
  add_durability_test(dead-owner SOURCE "dead-owner.c++" NO_PCH)
  # The lock-events smoke (the #562 follow-up): the call-time ordering
  # capture of the write-lock lifecycle via the ipclock_* probe sites, with
  # the same fork-based multi-process choreography (hence NO_PCH too). The
  # MDBX_PROBES macro comes from config-cmake.h (pulled by essentials.h), so
  # no compile definition is added here; the ENVIRONMENT is APPENDed per the
  # add_gtest() comment about the ASAN options.
  add_durability_test(lock-events SOURCE "lock-events.c++" NO_PCH)
  if(MDBX_PROBES)
    set_property(TEST lock-events APPEND PROPERTY ENVIRONMENT "MDBX_PROBES=1")
  endif()
endif()

# Scale the registered tests' TIMEOUTs by the platform/configuration factor
# (see cmake/test-timeout-scale.cmake).
mdbx_scale_dir_test_timeouts()
