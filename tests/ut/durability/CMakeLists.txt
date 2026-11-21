macro(add_durability_test)
  add_gtest(${ARGV} PREFIX "durability_" LABELS "ut;ut.durability")
endmacro(add_durability_test)

# The crash-storm (the durability test program, issue #564): the multi-
# writer forked victims and the kill-rule; the fork-based victim requires
# fork(), so the corpus is POSIX-only, the same gating as the crash-window
# matrix and the dead-writer recovery matrix registered in tests/CMakeLists.txt.
if(NOT WIN32)
  add_durability_test(crash-storm SOURCE "crash-storm.c++")
  if(MDBX_PROBES)
    target_compile_definitions(durability_crash-storm PRIVATE MDBX_PROBES)
    set_tests_properties(crash-storm PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  endif()
endif()

# Scale the registered tests' TIMEOUTs by the platform/configuration factor
# (see cmake/test-timeout-scale.cmake).
mdbx_scale_dir_test_timeouts()
