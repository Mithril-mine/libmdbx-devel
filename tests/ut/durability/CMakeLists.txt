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
endif()

# Scale the registered tests' TIMEOUTs by the platform/configuration factor
# (see cmake/test-timeout-scale.cmake).
mdbx_scale_dir_test_timeouts()
