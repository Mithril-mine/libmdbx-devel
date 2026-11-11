macro(add_issue_test)
  add_gtest(${ARGV} PREFIX "issue_" LABELS "ut;ut.issues")
endmacro(add_issue_test)

# The primary corpus: unconditional tests in the strict issue-number order.
add_issue_test(gh0010 SOURCE "issue_gh0010.c++")
add_issue_test(gh0011 SOURCE "issue_gh0011.c++")
add_issue_test(gh0017 SOURCE "issue_gh0017.c++" NO_PCH)
add_issue_test(gh0023 SOURCE "issue_gh0023.c++")
add_issue_test(gh0024 SOURCE "issue_gh0024.c++")
add_issue_test(gh0025 SOURCE "issue_gh0025.c++")
add_issue_test(gh0026 SOURCE "issue_gh0026.c++")
add_issue_test(gh0028 SOURCE "issue_gh0028.c++")
add_issue_test(gh0030 SOURCE "issue_gh0030.c++")
add_issue_test(gh0033 SOURCE "issue_gh0033.c++")
add_issue_test(gh0056 SOURCE "issue_gh0056.c++")
add_issue_test(gh0057 SOURCE "issue_gh0057.c++")
add_issue_test(gh0060 SOURCE "issue_gh0060.c++")
add_issue_test(gh0063 SOURCE "issue_gh0063.c++")
add_issue_test(gh0064 SOURCE "issue_gh0064.c++")
add_issue_test(gh0065 SOURCE "issue_gh0065.c++" NO_PCH)
add_issue_test(gh0066 SOURCE "issue_gh0066.c++")
add_issue_test(gh0068 SOURCE "issue_gh0068.c++")
add_issue_test(gh0069 SOURCE "issue_gh0069.c++")
add_issue_test(gh0073 SOURCE "issue_gh0073.c++")
add_issue_test(gh0074 SOURCE "issue_gh0074.c++")
add_issue_test(gh0075 SOURCE "issue_gh0075.c++")
add_issue_test(gh0077 SOURCE "issue_gh0077.c++")
add_issue_test(gh0078 SOURCE "issue_gh0078.c++")
add_issue_test(gh0079 SOURCE "issue_gh0079.c++")
add_issue_test(gh0082 SOURCE "issue_gh0082.c++")
add_issue_test(gh0083 SOURCE "issue_gh0083.c++")
add_issue_test(gh0084 SOURCE "issue_gh0084.c++")
add_issue_test(gh0085 SOURCE "issue_gh0085.c++")
add_issue_test(gh0086 SOURCE "issue_gh0086.c++")
add_issue_test(gh0087 SOURCE "issue_gh0087.c++")
add_issue_test(gh0088 SOURCE "issue_gh0088.c++")
add_issue_test(gh0089 SOURCE "issue_gh0089.c++")
add_issue_test(gh0090 SOURCE "issue_gh0090.c++")
add_issue_test(gh0092 SOURCE "issue_gh0092.c++")
add_issue_test(gh0093 SOURCE "issue_gh0093.c++")
add_issue_test(gh0094 SOURCE "issue_gh0094.c++")
add_issue_test(gh0096 SOURCE "issue_gh0096.c++")
add_issue_test(gh0097 SOURCE "issue_gh0097.c++")
add_issue_test(gh0098 SOURCE "issue_gh0098.c++")
add_issue_test(gh0099 SOURCE "issue_gh0099.c++")
add_issue_test(gh0100 SOURCE "issue_gh0100.c++")
add_issue_test(gh0101 SOURCE "issue_gh0101.c++")
add_issue_test(gh0102 SOURCE "issue_gh0102.c++")
add_issue_test(gh0103 SOURCE "issue_gh0103.c++")
add_issue_test(gh0104 SOURCE "issue_gh0104.c++")
add_issue_test(gh0104_clone2 SOURCE "issue_gh0104_clone2.c++")
add_issue_test(gh0106 SOURCE "issue_gh0106.c++")
add_issue_test(gh0108 SOURCE "issue_gh0108.c++")
add_issue_test(gh0112 SOURCE "issue_gh0112.c++")
add_issue_test(gh0114 SOURCE "issue_gh0114.c++")
add_issue_test(gh0116 SOURCE "issue_gh0116.c++")
add_issue_test(gh0117 SOURCE "issue_gh0117.c++")
add_issue_test(gh0154 SOURCE "issue_gh0154.c++")
add_issue_test(gh0170 SOURCE "issue_gh0170.c++")
add_issue_test(gh0173 SOURCE "issue_gh0173.c++")
add_issue_test(gh0176 SOURCE "issue_gh0176.c++")
add_issue_test(gh0179 SOURCE "issue_gh0179.c++" NO_PCH)
add_issue_test(gh0182 SOURCE "issue_gh0182.c++" NO_PCH)
add_issue_test(gh0301 SOURCE "issue_gh0301.c++")
add_issue_test(gh0304 SOURCE "issue_gh0304.c++")
add_issue_test(gh0305 SOURCE "issue_gh0305.c++")
add_issue_test(gh0306 SOURCE "issue_gh0306.c++")
add_issue_test(gh0307 SOURCE "issue_gh0307.c++")
add_issue_test(gh0311 SOURCE "issue_gh0311.c++")
add_issue_test(gh0314 SOURCE "issue_gh0314.c++")
add_issue_test(gh0315 SOURCE "issue_gh0315.c++" NO_PCH)
if(TARGET issue_gh0315)
  # the test re-runs its own executable as a child process; no target without GoogleTest
  set_property(TEST gh0315 APPEND PROPERTY ENVIRONMENT "MDBX_TEST_GH315_SELF=$<TARGET_FILE:issue_gh0315>")
endif()
add_issue_test(gh0318 SOURCE "issue_gh0318.c++")
add_issue_test(gh0319 SOURCE "issue_gh0319.c++")
if(NOT WIN32 AND NOT APPLE)
  # the pwrite-family wrapping requires the GNU ld --wrap feature
  add_issue_test(gh0321 SOURCE "issue_gh0321.c++" LIBRARY mdbx-static)
  target_compile_definitions(issue_gh0321 PRIVATE MDBX_TEST_PWRITE_WRAP=1)
  target_link_options(issue_gh0321 PRIVATE "-Wl,--wrap=pwrite" "-Wl,--wrap=pwrite64" "-Wl,--wrap=pwritev"
                      "-Wl,--wrap=pwritev64")
endif()
add_issue_test(gh0338 SOURCE "issue_gh0338.c++")
add_issue_test(gh0341 SOURCE "issue_gh0341.c++")
add_issue_test(gh0342 SOURCE "issue_gh0342.c++")
add_issue_test(gh0345 SOURCE "issue_gh0345.c++")
add_issue_test(gh0388 SOURCE "issue_gh0388.c++")

# Platform-gated tests: Windows-only cases in the if-branch,
# POSIX-only ones in the else-branch (numeric order within each).
if(WIN32)
  if(MDBX_PROBES)
    # issue #170: Windows-only throttling of the datafile-shrink attempts,
    # observed through the probe-bus (dxb_shrink_throttled / dxb_shrink_attempt).
    # The test includes only the public mdbx.h++ (the probe-bus is reached via
    # the exported mdbx_probe_ctl()), so it reuses the shared test PCH; the
    # MDBX_PROBES=1 environment enables the probe-bus at runtime, not via a
    # per-target compile definition, so the PCH flags still match.
    add_issue_test(gh0170_throttle SOURCE "issue_gh0170_throttle.c++")
    set_tests_properties(gh0170_throttle PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  endif()
  # issue #217: the shrink throttle must not rate-limit non-doomed shrinks
  # (single-process / MDBX_EXCLUSIVE), and a shrink must be deferred while a
  # FOREIGN process (spawned child) holds a read transaction. Plain Windows
  # test: no probe-bus dependency, so the regular Windows CI cells run it.
  add_issue_test(gh0217 SOURCE "issue_gh0217.c++")
  add_issue_test(gh0336 SOURCE "issue_gh0336.c++")
else()
  add_issue_test(gh0016 SOURCE "issue_gh0016.c++")
  add_issue_test(gh0058 SOURCE "issue_gh0058.c++")
  add_issue_test(gh0059 SOURCE "issue_gh0059.c++")
  # On Windows the environment keeps the lower region of the lock file (up to rdt_length) byte-range locked while open,
  # so no second handle in the process can write there and cached_oldest_txnid is not externally tamper-able.
  add_issue_test(gh0081 SOURCE "issue_gh0081.c++")
  add_issue_test(gh0107 SOURCE "issue_gh0107.c++")
  add_issue_test(gh0171 SOURCE "issue_gh0171.c++")
  add_issue_test(gh0201 SOURCE "issue_gh0201_locking2001.c++" NO_PCH)
  add_issue_test(gh0309 SOURCE "issue_gh0309.c++")
  add_issue_test(gh0310 SOURCE "issue_gh0310.c++")
  add_issue_test(gh0316 SOURCE "issue_gh0316.c++")
  add_issue_test(gh0324 SOURCE "issue_gh0324.c++")
  add_issue_test(gh0328 SOURCE "issue_gh0328.c++")
  add_issue_test(gh0329 SOURCE "issue_gh0329.c++")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT MDBX_DISABLE_GNU_SOURCE)
    # mounts a ramfs in an unprivileged user namespace; without _GNU_SOURCE glibc
    # hides FALLOC_FL_KEEP_SIZE, so libmdbx keeps the posix_fallocate() path there
    add_issue_test(gh0330 SOURCE "issue_gh0330.c++")
  endif()
  add_issue_test(gh0331 SOURCE "issue_gh0331.c++")
  add_issue_test(gh0339 SOURCE "issue_gh0339.c++")
endif()

# Tests requiring the probe-bus (built with MDBX_PROBES=ON).
if(MDBX_PROBES)
  add_issue_test(gh0067 SOURCE "issue_gh0067.c++" NO_PCH)
  set_tests_properties(gh0067 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0103_restart SOURCE "issue_gh0103_restart.c++")
  set_tests_properties(gh0103_restart PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0104_clone SOURCE "issue_gh0104_clone.c++")
  set_tests_properties(gh0104_clone PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0203 SOURCE "issue_gh0203.c++" NO_PCH)
  set_tests_properties(gh0203 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  # gh0182 also carries the probe-bus lckless tests of issue #215 (they use
  # mdbx_probe_ctl to force the lck-open failure): the C++ TU does not see the
  # internal config-cmake.h define by itself, so propagate MDBX_PROBES and
  # activate the registry at runtime.
  target_compile_definitions(issue_gh0182 PRIVATE MDBX_PROBES)
  set_tests_properties(gh0182 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0322 SOURCE "issue_gh0322.c++")
  target_compile_definitions(issue_gh0322 PRIVATE MDBX_PROBES)
  set_tests_properties(gh0322 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0326 SOURCE "issue_gh0326.c++")
  target_compile_definitions(issue_gh0326 PRIVATE MDBX_PROBES)
  set_tests_properties(gh0326 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0327 SOURCE "issue_gh0327.c++")
  set_tests_properties(gh0327 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0331_direct SOURCE "issue_gh0331_direct.c++")
  set_tests_properties(gh0331_direct PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0332 SOURCE "issue_gh0332.c++")
  set_tests_properties(gh0332 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0333 SOURCE "issue_gh0333.c++")
  set_tests_properties(gh0333 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
  add_issue_test(gh0337 SOURCE "issue_gh0337.c++")
  set_tests_properties(gh0337 PROPERTIES ENVIRONMENT "MDBX_PROBES=1")
endif()

# Tests requiring the tools (MDBX_BUILD_TOOLS, the mdbx_dump/load/stat binaries).
if(MDBX_BUILD_TOOLS)
  add_issue_test(gh0070 SOURCE "issue_gh0070.c++")
  set_tests_properties(
    gh0070
    PROPERTIES
      ENVIRONMENT
        "MDBX_TOOL_DUMP=$<TARGET_FILE:mdbx_dump>;MDBX_TOOL_LOAD=$<TARGET_FILE:mdbx_load>")
  add_issue_test(gh0071 SOURCE "issue_gh0071.c++")
  set_tests_properties(gh0071 PROPERTIES
                       ENVIRONMENT "MDBX_TOOL_DUMP=$<TARGET_FILE:mdbx_dump>")
  add_issue_test(gh0072 SOURCE "issue_gh0072.c++")
  set_tests_properties(
    gh0072
    PROPERTIES
      ENVIRONMENT
        "MDBX_TOOL_DUMP=$<TARGET_FILE:mdbx_dump>;MDBX_TOOL_LOAD=$<TARGET_FILE:mdbx_load>;MDBX_TOOL_STAT=$<TARGET_FILE:mdbx_stat>")
  add_issue_test(gh0076 SOURCE "issue_gh0076.c++")
  add_issue_test(gh0312 SOURCE "issue_gh0312.c++")
endif()
