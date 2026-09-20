# This is thunk-Makefile for calling GNU Make 3.81 or above.
#
# It forwards every target to the GNUmakefile, which requires GNU Make.
# The explicit list below sets the default goal (`all`), while the
# .DEFAULT rule forwards any other target, so this Makefile is a
# transparent proxy even for makes that do not know the GNUmakefile targets.
#
# Note: the flags are not passed explicitly, since the sub-make inherits them
# via the MAKEFLAGS environment variable (passing $(MAKEFLAGS) verbatim would
# make GNU Make treat the single-letter flags, e.g. `-n`, as goals).

all help options show-options check_buildflags_tag clean strip \
lib libs lib-static lib-shared libmdbx mdbx \
tools tools-static mdbx_chk mdbx_copy mdbx_defrag mdbx_drop mdbx_dump mdbx_load mdbx_stat \
cmake-build cmake-assertions-build cmake-asan-build cmake-ubsan-build cmake-memcheck-build cmake-leak-build cmake-stochastic-build \
ninja ninja-debug ninja-assertions ctest run-ut build-test build-stochastic \
install install-strip install-no-strip uninstall \
test test-assertion test-asan test-ubsan test-memcheck test-leak test-valgrind \
test-long test-long-assertion test-stochastic test-singleprocess test-ci test-ci-extra \
smoke smoke-assertion smoke-asan smoke-ubsan smoke-memcheck smoke-valgrind smoke-fault smoke-singleprocess \
memcheck long-test check check-posix-locking check-posix-locking-run \
check-posix-locking-sysv check-posix-locking-1988 check-posix-locking-2001 check-posix-locking-2008 \
mdbx_legacy_example mdbx_modern_example \
bench bench-couple bench-triplet bench-quartet bench-clean re-bench \
dist doxygen tags release-assets reformat gcc-analyzer cross-gcc cross-qemu:
	@CC=$(CC) \
	CXX=`if test -n "$(CXX)" && which "$(CXX)" > /dev/null; then echo "$(CXX)"; elif test -n "$(CCC)" && which "$(CCC)" > /dev/null; then echo "$(CCC)"; else echo "c++"; fi` \
	`which gmake || which gnumake || echo 'echo "GNU Make 3.81 or above is required"; exit 2;'` \
		-f GNUmakefile $@

.DEFAULT:
	@CC=$(CC) \
	CXX=`if test -n "$(CXX)" && which "$(CXX)" > /dev/null; then echo "$(CXX)"; elif test -n "$(CCC)" && which "$(CCC)" > /dev/null; then echo "$(CCC)"; else echo "c++"; fi` \
	`which gmake || which gnumake || echo 'echo "GNU Make 3.81 or above is required"; exit 2;'` \
		-f GNUmakefile $@