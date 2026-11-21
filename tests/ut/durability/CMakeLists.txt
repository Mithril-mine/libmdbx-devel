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
endif()

# The Windows RDONLY mapping-extent reproducer (issue #549): the reader's
# section extent is pinned to the open-time file size (NtCreateSection
# MaximumSize), so a fresh snapshot beyond it fails with MDBX_EPERM. The body
# gates itself with #ifdef _WIN32 and skips elsewhere, so the suite stays
# green on the non-Windows cells.
add_durability_test(windows-mmap-extent SOURCE "windows-mmap-extent.c++")

# The best-effort stress of the windows-lck-restored-copy-opener-race finding
# (#549): Windows-only at runtime (the spawn/handshake machinery is Win32),
# registered unconditionally so the non-Windows cells carry the skip evidence.
add_durability_test(windows-lck-race SOURCE "windows-lck-race.c++")

# Scale the registered tests' TIMEOUTs by the platform/configuration factor
# (see cmake/test-timeout-scale.cmake).
mdbx_scale_dir_test_timeouts()
