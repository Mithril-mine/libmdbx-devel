# This makefile is for GNU Make 3.81 or above, and nowadays provided
# just for compatibility and preservation of traditions.
#
# Please use CMake in case of any difficulties or
# problems with this old-school's magic.
#
################################################################################
#
# Basic internal definitions. For a customizable variables and options see below.
#
$(info // The GNU Make $(MAKE_VERSION))
SHELL         := $(shell env bash -c 'echo $$BASH')
MAKE_VERx3    := $(shell printf "%3s%3s%3s" $(subst ., ,$(MAKE_VERSION)))
BASH_VERx2    := $(shell printf "%3s%3s" $$(echo $${BASH_VERSION} | cut -d . -f 1,2 | tr . ' '))
make_lt_3_81  := $(shell expr "$(MAKE_VERx3)" "<" "  3 81")
ifneq ($(make_lt_3_81),0)
$(error Please use GNU Make 3.81 or above)
endif
make_ge_4_1   := $(shell expr "$(MAKE_VERx3)" ">=" "  4  1")
make_ge_4_4   := $(shell expr "$(MAKE_VERx3)" ">=" "  4  4")
bash_ge_4_3   := $(shell expr "$(BASH_VERx2)" ">=" "  4  3")
SRC_PROBE_C   := $(shell [ -f mdbx.c ] && echo mdbx.c || echo src/osal.c)
SRC_PROBE_CXX := $(shell [ -f mdbx.c++ ] && echo mdbx.c++ || echo src/mdbx.c++)
UNAME         := $(shell uname -s 2>/dev/null || echo Unknown)

define cxx_filesystem_probe
  int main(int argc, const char*argv[]) {
    mdbx::filesystem::path probe(argv[0]);
    if (argc != 1) throw mdbx::filesystem::filesystem_error(std::string("fake"), std::error_code());
    return mdbx::filesystem::is_directory(probe.relative_path());
  }
endef
#
################################################################################
#
# Use `make options` to list the available libmdbx build options.
#
# Note that the defaults should already be correct for most platforms;
# you should not need to change any of these. Read their descriptions
# in the README and source code (see src/options.h) if you do.
#

# install sandbox
DESTDIR ?=
INSTALL ?= install
# install prefixes (inside sandbox)
prefix  ?= /usr/local
mandir  ?= $(prefix)/man
# lib/bin suffix for multiarch/biarch, e.g. '.x86_64'
suffix  ?=

# toolchain
CC      ?= gcc
CXX     ?= g++
CFLAGS_EXTRA ?=
LD      ?= ld
CMAKE	?= "$(shell which cmake 2>&-)"
CMAKE_OPT ?= $(MDBX_BUILD_OPTIONS)
CTEST	?= ctest
CTEST_OPT ?=
# target directory for `make dist`
DIST_DIR ?= dist
# sanitizers
ASAN_OPTIONS	?=log_path=asan.log:poison_history_size=42
UBSAN_OPTIONS	?=log_path=ubsan.log:print_stacktrace=1

# build options
MDBX_DEBUG           ?=
MDBX_CHECKING        ?=
MDBX_BUILD_OPTIONS   ?=$(if $(MDBX_DEBUG),-DMDBX_DEBUG=$(MDBX_DEBUG) ,)$(if $(MDBX_CHECKING),-DMDBX_CHECKING=$(MDBX_CHECKING) ,)
MDBX_BUILD_TIMESTAMP ?=$(if $(SOURCE_DATE_EPOCH),$(SOURCE_DATE_EPOCH),$(shell date +%Y-%m-%dT%H:%M:%S%z))
MDBX_BUILD_CXX       ?=YES
MDBX_BUILD_METADATA  ?=

# probe and compose common compiler flags with variable expansion trick (seems this work two times per session for GNU Make 3.81)
CFLAGS       ?= $(strip $(eval CFLAGS := -std=gnu11 -O2 -g -Wall -Werror -Wextra -Wpedantic -ffunction-sections -fPIC -fvisibility=hidden -pthread -Wno-error=attributes $$(shell for opt in -fno-semantic-interposition -Wno-unused-command-line-argument -Wno-tautological-compare; do [ -z "$$$$($(CC) '-DMDBX_BUILD_FLAGS="probe"' $$$${opt} -c $(SRC_PROBE_C) -o /dev/null >/dev/null 2>&1 || echo failed)" ] && echo "$$$${opt} "; done)$(CFLAGS_EXTRA))$(CFLAGS))

# choosing C++ standard with variable expansion trick (seems this work two times per session for GNU Make 3.81)
CXXSTD       ?= $(eval CXXSTD := $$(shell for std in gnu++23 c++23 gnu++2b c++2b gnu++20 c++20 gnu++2a c++2a gnu++17 c++17 gnu++1z c++1z gnu++14 c++14 gnu++1y c++1y gnu+11 c++11 gnu++0x c++0x; do $(CXX) -std=$$$${std} -DMDBX_BUILD_CXX=1 -c $(SRC_PROBE_CXX) -o /dev/null 2>probe4std-$$$${std}.err >/dev/null && echo "-std=$$$${std}" && exit; done))$(CXXSTD)
CXXFLAGS     ?= $(strip $(CXXSTD) $(filter-out -std=gnu11,$(CFLAGS)))

# libraries and options for linking
EXE_LDFLAGS  ?= -pthread
ifneq ($(make_ge_4_1),1)
# don't use variable expansion trick as workaround for bugs of GNU Make before 4.1
LIBS         ?= $(shell $(uname2libs))
LDFLAGS      ?= $(shell $(uname2ldflags))
LIB_STDCXXFS ?= $(shell echo '$(cxx_filesystem_probe)' | cat mdbx.h++ - | sed $$'1s/\xef\xbb\xbf//' | grep -v 'pragma once' | $(CXX) -x c++ $(CXXFLAGS) -Wno-error - -Wl,--allow-multiple-definition -lstdc++fs $(LIBS) $(LDFLAGS) $(EXE_LDFLAGS) -o /dev/null 2>probe4lstdfs.err >/dev/null && echo '-Wl,--allow-multiple-definition -lstdc++fs')
else
# using variable expansion trick to avoid repeaded probes
LIBS         ?= $(eval LIBS := $$(shell $$(uname2libs)))$(LIBS)
LDFLAGS      ?= $(eval LDFLAGS := $$(shell $$(uname2ldflags)))$(LDFLAGS)
LIB_STDCXXFS ?= $(eval LIB_STDCXXFS := $$(shell echo '$$(cxx_filesystem_probe)' | cat mdbx.h++ - | sed $$$$'1s/\xef\xbb\xbf//' | grep -v '#pragma once' | $(CXX) -x c++ $(CXXFLAGS) -Wno-error - -Wl,--allow-multiple-definition -lstdc++fs $(LIBS) $(LDFLAGS) $(EXE_LDFLAGS) -o /dev/null 2>probe4lstdfs.err >/dev/null && echo '-Wl,--allow-multiple-definition -lstdc++fs'))$(LIB_STDCXXFS)
endif

ifneq ($(make_ge_4_4),1)
.NOTPARALLEL:
WAIT         =
else
WAIT         = .WAIT
endif

################################################################################

define uname2sosuffix
  case "$(UNAME)" in
    Darwin*|Mach*) echo dylib;;
    CYGWIN*|MINGW*|MSYS*|Windows*) echo dll;;
    *) echo so;;
  esac
endef

define uname2ldflags
  case "$(UNAME)" in
    CYGWIN*|MINGW*|MSYS*|Windows*)
      echo '-Wl,--gc-sections,-O1,--as-needed';
      ;;
    *)
      $(LD) --help 2>/dev/null | grep -q -- --gc-sections && echo '-Wl,--gc-sections,-z,relro,-O1';
      $(LD) --help 2>/dev/null | grep -q -- -dead_strip && echo '-Wl,-dead_strip';
      $(LD) --help 2>/dev/null | grep -q -- --as-needed && echo '-Wl,--as-needed';
      ;;
  esac
endef

# TIP: try adding '-Wl,--no-as-needed,-lrt' for the ability to build with modern glibc, and then use with the old.
define uname2libs
  case "$(UNAME)" in
    CYGWIN*|MINGW*|MSYS*|Windows*)
      echo '-lntdll -lwinmm';
      ;;
    *SunOS*|*Solaris*)
      echo '-lkstat -lrt';
      ;;
    *Darwin*|OpenBSD*)
      echo '';
      ;;
    *)
      echo '-lrt -latomic';
      ;;
  esac
endef

SO_SUFFIX  := $(shell $(uname2sosuffix))
HEADERS    := mdbx.h mdbx.h++
#> dist-cutoff-begin
HEADERS	   += $(wildcard src/*.h) $(wildcard mdbx++/*.h++)
#< dist-cutoff-end
LIBRARIES  := libmdbx.a libmdbx.$(SO_SUFFIX)
TOOLS      := chk copy defrag drop dump load stat
MDBX_TOOLS := $(addprefix mdbx_,$(TOOLS))
MANPAGES   := mdbx_stat.1 mdbx_copy.1 mdbx_dump.1 mdbx_load.1 mdbx_chk.1 mdbx_drop.1
TIP        := // TIP:

.PHONY: all help options lib libs tools clean install uninstall check_buildflags_tag tools-static run-ut
.PHONY: install-strip install-no-strip strip libmdbx mdbx show-options lib-static lib-shared cmake-build ninja

boolean = $(if $(findstring $(strip $($1)),YES Yes yes y ON On on 1 true True TRUE),1,$(if $(findstring $(strip $($1)),NO No no n OFF Off off 0 false False FALSE),,$(error Wrong value `$($1)` of $1 for YES/NO option)))
select_by = $(if $(call boolean,$(1)),$(2),$(3))

ifeq ("$(origin V)", "command line")
  MDBX_BUILD_VERBOSE := $(V)
endif
ifndef MDBX_BUILD_VERBOSE
  MDBX_BUILD_VERBOSE := 0
endif

ifeq ($(call boolean,MDBX_BUILD_VERBOSE),1)
  QUIET :=
  HUSH :=
  $(info $(TIP) Use `make V=0` for quiet.)
else
  QUIET := @
  HUSH := >/dev/null
  $(info $(TIP) Use `make V=1` for verbose.)
endif

ifeq ($(UNAME),Darwin)
  $(info $(TIP) Use `brew install gnu-sed gnu-tar` and add ones to the beginning of the PATH.)
endif

all: show-options $(LIBRARIES) $(MDBX_TOOLS)

help:
	@echo ""
	@echo "  libmdbx build system - available targets"
	@echo ""
	@echo "  Build:"
	@echo "    make all                   - build the libraries and the tools"
	@echo "    make help                  - print this help"
	@echo "    make options               - list the build options"
	@echo "    make show-options          - show the effective build options and toolchain"
	@echo "    make lib / libs / libmdbx / mdbx - build the libraries"
	@echo "    make lib-static            - build the static library"
	@echo "    make lib-shared            - build the shared library"
	@echo "    make tools                 - build the tools"
	@echo "    make tools-static          - build the tools, statically linked against the system libraries"
	@echo "    make clean                 - remove all built artifacts"
	@echo "    make strip                 - strip debug symbols from the built binaries"
	@echo ""
	@echo "  CMake & Ninja:"
	@echo "    make cmake-build           - configure and build with CMake"
	@echo "    make ninja                 - configure and build with CMake and the Ninja generator"
	@echo "    make ninja-debug           - same as ninja, but with the Debug build type"
	@echo "    make ninja-assertions      - same as ninja, but with assertions enabled"
	@echo "    make ctest                 - build with CMake and run all tests via ctest"
	@echo "    make run-ut                - build and run the example executables"
	@echo "    make build-test            - build the test executables"
	@echo "    make cmake-assertions-build / cmake-asan-build / cmake-ubsan-build / cmake-memcheck-build / cmake-leak-build / cmake-stochastic-build"
	@echo "                              - configure and build with CMake for the corresponding mode"
	@echo ""
	@echo "  Install:"
	@echo "    make install               - install into the prefix (strip by default)"
	@echo "    make install-strip         - install and strip the binaries"
	@echo "    make install-no-strip      - install without stripping"
	@echo "    make uninstall             - remove the installed files"
	@echo ""
	@echo "  Testing:"
	@echo "    make test                  - run the basic tests"
	@echo "    make test-assertion        - build with assertions and run the basic test"
	@echo "    make test-asan             - build with AddressSanitizer and run the basic test"
	@echo "    make test-ubsan            - build with UndefinedBehaviourSanitizer and run the basic test"
	@echo "    make test-memcheck         - run the basic test under Valgrind/memcheck"
	@echo "    make test-valgrind         - alias for test-memcheck"
	@echo "    make test-leak             - build with LeakSanitizer and run the basic test"
	@echo "    make memcheck              - alias for smoke-memcheck"
	@echo "    make mdbx_legacy_example   - build the legacy C API example"
	@echo "    make mdbx_modern_example   - build the modern C++ API example"
	@echo ""
#> dist-cutoff-begin
	@echo ""
	@echo "  Long & scenario tests:"
	@echo "    make test-long             - run the long test (several weeks or until interrupted)"
	@echo "    make long-test             - alias for test-long"
	@echo "    make long-test-assertion   - run the long test with assertions"
	@echo "    make test-stochastic       - run the stochastic test"
	@echo "    make test-singleprocess    - run the single-process basic test (also used by cross-qemu)"
	@echo "    make build-stochastic      - build the framework for the stochastic test"
	@echo "    make check                 - smoke test with amalgamation and installation checks"
	@echo "    make test-ci               - run the tests used by the CI"
	@echo "    make test-ci-extra         - run test-ci plus cross-gcc and cross-qemu"
	@echo "    make check-posix-locking / check-posix-locking-sysv / check-posix-locking-1988 / check-posix-locking-2001 / check-posix-locking-2008"
	@echo "                              - check the POSIX locking modes"
	@echo ""
	@echo "  Smoke tests:"
	@echo "    make smoke                 - run the fast smoke test"
	@echo "    make smoke-assertion       - run the smoke test with assertions"
	@echo "    make smoke-asan            - run the smoke test under AddressSanitizer"
	@echo "    make smoke-ubsan           - run the smoke test under UndefinedBehaviourSanitizer"
	@echo "    make smoke-memcheck        - run the smoke test under Valgrind/memcheck"
	@echo "    make smoke-valgrind        - alias for smoke-memcheck"
	@echo "    make smoke-fault           - run the transaction-owner-failure smoke testcase"
	@echo "    make smoke-singleprocess   - run the single-process smoke test"
	@echo "    make smoke-t1              - T1 fast deterministic smoke (seconds, fixed seeds)"
	@echo "    make smoke-t2              - T2 medium smoke (quick-smoke family)"
	@echo "    make smoke-t3              - T3 long/full stochastic runs (milestone/nightly)"
	@echo "    make select-tests          - show CTest labels affected by changed paths (impact selection)"
	@echo ""
	@echo "  Benchmarking:"
	@echo "    make bench                 - run the ioarena benchmark"
	@echo "    make bench-couple          - run the benchmark for mdbx and lmdb"
	@echo "    make bench-triplet         - run the benchmark for mdbx, lmdb and sqlite3"
	@echo "    make bench-quartet         - run the benchmark for mdbx, lmdb, rocksdb and wiredtiger"
	@echo "    make bench-clean           - remove the temporary databases after benchmarking"
	@echo "    make re-bench              - clean and re-run the benchmark"
	@echo ""
	@echo "  Development:"
	@echo "    make cross-gcc             - check cross-compilation without running the tests"
	@echo "    make cross-qemu            - cross-compile and run the basic test under QEMU"
	@echo "    make gcc-analyzer          - run the gcc static analyzer"
	@echo "    make dist                  - build the amalgamated source code"
	@echo "    make doxygen               - build the HTML documentation"
	@echo "    make tags                  - build the tags database"
	@echo "    make release-assets        - build the release assets"
	@echo "    make reformat              - reformat the source code with clang-format"
#< dist-cutoff-end

show-options:
	@echo "  MDBX_BUILD_OPTIONS   = $(MDBX_BUILD_OPTIONS)"
	@echo "  MDBX_BUILD_CXX       = $(MDBX_BUILD_CXX)"
	@echo "  MDBX_BUILD_TIMESTAMP = $(MDBX_BUILD_TIMESTAMP)"
	@echo "  MDBX_BUILD_METADATA  = $(MDBX_BUILD_METADATA)"
	@echo '$(TIP) Use `make options` to listing available build options.'
	@echo $(call select_by,MDBX_BUILD_CXX,"  CXX      =`which $(CXX)` | `$(CXX) --version | head -1`","  CC       =`which $(CC)` | `$(CC) --version | head -1`")
	@echo $(call select_by,MDBX_BUILD_CXX,"  CXXFLAGS =$(CXXFLAGS)","  CFLAGS   =$(CFLAGS)")
	@echo $(call select_by,MDBX_BUILD_CXX,"  LDFLAGS  =$(LDFLAGS) $(LIB_STDCXXFS) $(LIBS) $(EXE_LDFLAGS)","  LDFLAGS  =$(LDFLAGS) $(LIBS) $(EXE_LDFLAGS)")
	@echo '$(TIP) Use `make help` to listing available targets.'

options:
	@echo "  INSTALL      =$(INSTALL)"
	@echo "  DESTDIR      =$(DESTDIR)"
	@echo "  prefix       =$(prefix)"
	@echo "  mandir       =$(mandir)"
	@echo "  suffix       =$(suffix)"
	@echo ""
	@echo "  CC           =$(CC)"
	@echo "  CFLAGS_EXTRA =$(CFLAGS_EXTRA)"
	@echo "  CFLAGS       =$(CFLAGS)"
	@echo "  CXX          =$(CXX)"
	@echo "  CXXSTD       =$(CXXSTD)"
	@echo "  CXXFLAGS     =$(CXXFLAGS)"
	@echo ""
	@echo "  LD           =$(LD)"
	@echo "  LDFLAGS      =$(LDFLAGS)"
	@echo "  EXE_LDFLAGS  =$(EXE_LDFLAGS)"
	@echo "  LIBS         =$(LIBS)"
	@echo ""
	@echo "  MDBX_BUILD_OPTIONS   = $(MDBX_BUILD_OPTIONS)"
	@echo "  MDBX_BUILD_TIMESTAMP = $(MDBX_BUILD_TIMESTAMP)"
	@echo "  MDBX_BUILD_METADATA  = $(MDBX_BUILD_METADATA)"
	@echo ""
	@echo "## Assortment items for MDBX_BUILD_OPTIONS:"
	@echo "##   Note that the defaults should already be correct for most platforms;"
	@echo "##   you should not need to change any of these. Read their descriptions"
#> dist-cutoff-begin
ifeq ($(wildcard mdbx.c),mdbx.c)
#< dist-cutoff-end
	@echo "##   in the README and source code (see mdbx.c) if you do."
	@grep -h '#ifndef MDBX_' mdbx.c | grep -v BUILD | sort -u | sed 's/#ifndef /  /'
#> dist-cutoff-begin
else
	@echo "##   in the README and source code (see src/options.h) if you do."
	@grep -h '#ifndef MDBX_' src/*.h | grep -v BUILD | sort -u | sed 's/#ifndef /  /'
endif
#< dist-cutoff-end

lib libs libmdbx mdbx: libmdbx.a libmdbx.$(SO_SUFFIX)

tools: $(MDBX_TOOLS)
tools-static: $(addsuffix .static,$(MDBX_TOOLS)) $(addsuffix .static-lto,$(MDBX_TOOLS))

strip: all
	@echo '  STRIP libmdbx.$(SO_SUFFIX) $(MDBX_TOOLS)'
	$(TRACE )strip libmdbx.$(SO_SUFFIX) $(MDBX_TOOLS)

clean:
	@echo '  CLEANING...'
	$(QUIET)rm -rf $(MDBX_TOOLS) mdbx_test @* *.[ao] *.[ls]o *.$(SO_SUFFIX) *.dSYM *~ tmp.db/* \
		*.gcov *.log *.err src/*.o tests/*.o tests/framework/*.o tests/ut/*.o tests/ut/*/*.o \
		mdbx_legacy_example mdbx_modern_example dist @dist-check \
		config-gnumake.h src/config-gnumake.h *.tar* @buildflags.tag @dist-checked.tag \
		mdbx_*.static mdbx_*.static-lto CMakeFiles

MDBX_BUILD_FLAGS =$(strip MDBX_BUILD_CXX=$(MDBX_BUILD_CXX) $(MDBX_BUILD_OPTIONS) $(call select_by,MDBX_BUILD_CXX,$(CXXFLAGS) $(LDFLAGS) $(LIB_STDCXXFS) $(LIBS),$(CFLAGS) $(LDFLAGS) $(LIBS)))
check_buildflags_tag:
	$(QUIET)if [ "$(MDBX_BUILD_FLAGS)" != "$$(cat @buildflags.tag 2>&1)" ]; then \
		echo "  TOUCH @buildflags.tag to force re-build with the (new) specified flags..." && \
		echo '$(MDBX_BUILD_FLAGS)' > @buildflags.tag; \
	fi

@buildflags.tag: check_buildflags_tag $(WAIT)

lib-static libmdbx.a: mdbx-static.o $(call select_by,MDBX_BUILD_CXX,mdbx++-static.o)
	@echo '  AR $@'
	$(QUIET)$(AR) rcs $@ $? $(HUSH)

lib-shared libmdbx.$(SO_SUFFIX): mdbx-dylib.o $(call select_by,MDBX_BUILD_CXX,mdbx++-dylib.o)
	@echo '  LD $@'
	$(QUIET)$(call select_by,MDBX_BUILD_CXX,$(CXX) $(CXXFLAGS),$(CC) $(CFLAGS)) $^ -pthread -shared $(LDFLAGS) $(call select_by,MDBX_BUILD_CXX,$(LIB_STDCXXFS)) $(LIBS) -o $@

# #####################################################################################################################
# CMake-based scenarios. Each scenario uses its own build directory to keep the configuration clean.
#
# $(1) = build directory, $(2) = extra CMake configure options, $(3) = extra CMake build arguments
define cmake-configure-build
	mkdir -p $(1) && ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) \
		$(CMAKE) $(CMAKE_OPT) $(2) $(if $(MDBX_SMOKE_EXTRA),-DMDBX_SMOKE_EXTRA="$(MDBX_SMOKE_EXTRA)",) -G Ninja -S . -B $(1) && \
		$(CMAKE) --build $(1) $(3)
endef

# $(1) = build directory, $(2) = ctest label regular expression, $(3) = extra ctest options
define ctest-scenario-run
	@echo '  RUN: ctest --label-regex $(2) in $(1)'
	$(QUIET)ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) $(CTEST) --test-dir $(1) \
		--label-regex '$(2)' --output-on-failure --parallel `(nproc | sysctl -n hw.ncpu | echo 2) 2>/dev/null` $(3) $(CTEST_OPT)
endef

cmake-build:
	@echo '  RUN: cmake -G Ninja && cmake --build @cmake-build'
	$(QUIET)$(call cmake-configure-build,@cmake-build,,)

cmake-assertions-build:
	@echo '  RUN: cmake -G Ninja -DMDBX_CHECKING=2 && cmake --build @cmake-assertions-build'
	$(QUIET)$(call cmake-configure-build,@cmake-assertions-build,-DMDBX_CHECKING=2,)

cmake-asan-build:
	@echo '  RUN: cmake -G Ninja -DENABLE_ASAN=ON && cmake --build @cmake-asan-build'
	$(QUIET)$(call cmake-configure-build,@cmake-asan-build,-DENABLE_ASAN:BOOL=ON -DENABLE_UBSAN:BOOL=OFF -DENABLE_MEMCHECK:BOOL=OFF -DMDBX_CHECKING=2,)

cmake-ubsan-build:
	@echo '  RUN: cmake -G Ninja -DENABLE_UBSAN=ON && cmake --build @cmake-ubsan-build'
	$(QUIET)$(call cmake-configure-build,@cmake-ubsan-build,-DENABLE_UBSAN:BOOL=ON -DENABLE_ASAN:BOOL=OFF -DENABLE_MEMCHECK:BOOL=OFF -DMDBX_CHECKING=2,)

cmake-memcheck-build:
	@echo '  RUN: cmake -G Ninja -DENABLE_MEMCHECK=ON && cmake --build @cmake-memcheck-build'
	$(QUIET)$(call cmake-configure-build,@cmake-memcheck-build,-DENABLE_MEMCHECK:BOOL=ON -DENABLE_ASAN:BOOL=OFF -DENABLE_UBSAN:BOOL=OFF -DMDBX_CHECKING=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo,)

cmake-leak-build:
	@echo '  RUN: cmake -G Ninja -fsanitize=leak && cmake --build @cmake-leak-build'
	$(QUIET)$(call cmake-configure-build,@cmake-leak-build,-DCMAKE_C_FLAGS=-fsanitize=leak -DCMAKE_CXX_FLAGS=-fsanitize=leak -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=leak -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=leak -DMDBX_CHECKING=2 -DMDBX_ENABLE_LONG_TESTS=ON,)

cmake-stochastic-build:
	@echo '  RUN: cmake -G Ninja -DMDBX_ENABLE_LONG_TESTS=ON && cmake --build @cmake-stochastic-build'
	$(QUIET)$(call cmake-configure-build,@cmake-stochastic-build,-DMDBX_ENABLE_LONG_TESTS=ON,)

cmake-probes-build:
	@echo '  RUN: cmake -G Ninja -DENABLE_SYSTEMTAP=ON && cmake --build @cmake-probes-build'
	$(QUIET)$(call cmake-configure-build,@cmake-probes-build,-DENABLE_SYSTEMTAP:BOOL=ON,) && \
		readelf -n @cmake-probes-build/libmdbx.so | grep -c 'Provider: mdbx' | \
		sed 's/^/  USDT probes in libmdbx.so: /'

ninja-assertions: cmake-assertions-build
ninja-debug: CMAKE_OPT += -DCMAKE_BUILD_TYPE=Debug
ninja-debug: cmake-build
ninja: cmake-build

ctest: cmake-build
	@echo '  RUN: ctest ..'
	$(QUIET)ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) $(CTEST) --test-dir @cmake-build --parallel `(nproc | sysctl -n hw.ncpu | echo 2) 2>/dev/null` --schedule-random $(CTEST_OPT)

run-ut: mdbx_legacy_example $(call select_by,MDBX_BUILD_CXX,mdbx_modern_example,)
	$(QUIET)for UT in $^; do echo "  Running $$UT" && ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) ./$${UT} || exit -1; done

TEST_TARGETS := mdbx_legacy_example $(call select_by,MDBX_BUILD_CXX,mdbx_modern_example,)
TEST_BUILD_TARGETS :=
ifneq ($(CMAKE),"")
TEST_TARGETS += ctest
TEST_BUILD_TARGETS += cmake-build
endif

.PHONY: cmake-build cmake-assertions-build cmake-asan-build cmake-ubsan-build cmake-memcheck-build cmake-leak-build cmake-stochastic-build
.PHONY: ninja-assertions ninja-debug ninja $(TEST_TARGETS) $(TEST_BUILD_TARGETS) test-ubsan test-asan test-memcheck test-leak test-assertion test build-test smoke check
test: $(TEST_TARGETS)
build-test: $(TEST_BUILD_TARGETS)

test-valgrind: test-memcheck
smoke-valgrind: smoke-memcheck
test-assertion: cmake-assertions-build
	$(call ctest-scenario-run,@cmake-assertions-build,.*,)
smoke-assertion: cmake-assertions-build
	$(call ctest-scenario-run,@cmake-assertions-build,^smoke$$,)
test-ubsan: cmake-ubsan-build
	$(call ctest-scenario-run,@cmake-ubsan-build,.*,)
smoke-ubsan: cmake-ubsan-build
	$(call ctest-scenario-run,@cmake-ubsan-build,^smoke$$,)
test-asan: cmake-asan-build
	$(call ctest-scenario-run,@cmake-asan-build,.*,)
smoke-asan: cmake-asan-build
	$(call ctest-scenario-run,@cmake-asan-build,^smoke$$,)
test-memcheck: cmake-memcheck-build
	@echo '  RUN: ctest -T memcheck @cmake-memcheck-build'
	$(QUIET)ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) $(CTEST) --test-dir @cmake-memcheck-build -T memcheck $(CTEST_OPT)
smoke-memcheck: cmake-memcheck-build
	@echo '  RUN: ctest -T memcheck (smoke) @cmake-memcheck-build'
	$(QUIET)ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) $(CTEST) --test-dir @cmake-memcheck-build --label-regex '^smoke$$' -T memcheck $(CTEST_OPT)
memcheck: smoke-memcheck
test-leak: cmake-leak-build
	$(call ctest-scenario-run,@cmake-leak-build,^stochastic$$,)

mdbx_legacy_example: mdbx.h examples/example-mdbx.c libmdbx.$(SO_SUFFIX)
	@echo '  CC+LD $@'
	$(QUIET)$(CC) $(CFLAGS) -I. examples/example-mdbx.c ./libmdbx.$(SO_SUFFIX) -o $@

mdbx_modern_example: mdbx.h examples/example-mdbx.c++ libmdbx.$(SO_SUFFIX)
	@echo '  CC+LD $@'
	$(QUIET)$(CXX) $(CXXFLAGS) -I. examples/example-mdbx.c++ ./libmdbx.$(SO_SUFFIX) -o $@

#> dist-cutoff-begin
ifeq ($(wildcard mdbx.c),mdbx.c)
#< dist-cutoff-end

################################################################################
# Amalgamated source code, i.e. distributed after `make dist`
MAN_SRCDIR := man1/

dist:
	@echo '  Starting 2026 libmdbx is distributed in an amalgamated source code form.'
	@echo '  So amalgamation is no longer required. Please update your build scripts.'

config-gnumake.h: @buildflags.tag mdbx.c $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  MAKE $@'
	$(QUIET)(echo '#define MDBX_BUILD_TIMESTAMP "$(MDBX_BUILD_TIMESTAMP)"' \
	&& echo "#define MDBX_BUILD_FLAGS \"$$(cat @buildflags.tag)\"" \
	&& echo '#define MDBX_BUILD_COMPILER "$(shell (LC_ALL=C $(CC) --version || echo 'Please use GCC or CLANG compatible compiler') | head -1)"' \
	&& echo '#define MDBX_BUILD_TARGET "$(shell set -o pipefail; (LC_ALL=C $(CC) -v 2>&1 | grep -i '^Target:' | cut -d ' ' -f 2- || (LC_ALL=C $(CC) --version | grep -qi e2k && echo E2K) || echo 'Please use GCC or CLANG compatible compiler') | head -1)"' \
	&& echo '#define MDBX_BUILD_CXX $(call select_by,MDBX_BUILD_CXX,1,0)' \
	&& echo '#define MDBX_BUILD_METADATA "$(MDBX_BUILD_METADATA)"' \
	) >$@

mdbx-dylib.o: config-gnumake.h mdbx.c mdbx.h mdbx-internals.h $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  CC $@'
	$(QUIET)$(CC) $(CFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -DLIBMDBX_EXPORTS=1 -c mdbx.c -o $@

mdbx-static.o: config-gnumake.h mdbx.c mdbx.h mdbx-internals.h $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  CC $@'
	$(QUIET)$(CC) $(CFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -ULIBMDBX_EXPORTS -c mdbx.c -o $@

mdbx++-dylib.o: config-gnumake.h mdbx.c++ $(HEADERS) mdbx-internals.h $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  CC $@'
	$(QUIET)$(CXX) $(CXXFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -DLIBMDBX_EXPORTS=1 -c mdbx.c++ -o $@

mdbx++-static.o: config-gnumake.h mdbx.c++ $(HEADERS) mdbx-internals.h $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  CC $@'
	$(QUIET)$(CXX) $(CXXFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -ULIBMDBX_EXPORTS -c mdbx.c++ -o $@

mdbx_%:	mdbx_%.c mdbx-static.o mdbx-wingetopt.h
	@echo '  CC+LD $@'
	$(QUIET)$(CC) $(CFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' $< mdbx-static.o $(LDFLAGS) $(EXE_LDFLAGS) $(LIBS) -o $@

mdbx_%.static: mdbx_%.c mdbx-static.o
	@echo '  CC+LD $@'
	$(QUIET)$(CC) $(CFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' $^ $(LDFLAGS) $(EXE_LDFLAGS) -static -Wl,--strip-all -o $@

mdbx_%.static-lto: mdbx_%.c config-gnumake.h mdbx.c mdbx.h
	@echo '  CC+LD $@'
	$(QUIET)$(CC) $(CFLAGS) -Os -flto $(MDBX_BUILD_OPTIONS) '-DLIBMDBX_API=' '-DMDBX_CONFIG_H="config-gnumake.h"' \
		$< mdbx.c $(LDFLAGS) $(EXE_LDFLAGS) $(LIBS) -static -Wl,--strip-all -o $@

check smoke: test

#> dist-cutoff-begin
else
################################################################################
# Non-amalgamated sources with test framework

.PHONY: build-stochastic check cross-gcc cross-qemu dist doxygen gcc-analyzer long-test
.PHONY: reformat release-assets tags smoke smoke-fault
.PHONY: smoke-singleprocess test-singleprocess test-stochastic test-long test-valgrind test-memcheck memcheck smoke-memcheck
.PHONY: smoke-t1 smoke-t2 smoke-t3 select-tests
.PHONY: smoke-assertion long-test-assertion test-ci test-ci-extra check-posix-locking check-posix-locking-run

test-ci-extra: test-ci cross-gcc cross-qemu

test-ci:
	$(QUIET)for T in check \
		smoke-singleprocess smoke-fault smoke-memcheck \
		test-leak test-asan test-ubsan test-singleprocess test-memcheck; \
	do $(MAKE) $$T || break; done

DIST_EXTRA := LICENSE NOTICE COPYRIGHT README.md TODO.md CMakeLists.txt GNUmakefile Makefile ChangeLog.md VERSION.json config.h.in ntdll.def \
	$(addprefix man1/, $(MANPAGES)) cmake/compiler.cmake cmake/profile.cmake cmake/utils.cmake windows-safeseh-masm.asm windows-safeseh-yasm.asm \
	windows-safeseh.obj valgrind.supp conanfile.py \
	$(addprefix examples/, CMakeLists.txt example-mdbx.c++ example-mdbx.c pcrf/pcrf_simulator.c README.md)

DIST_SRC   := mdbx.h mdbx.h++ mdbx.c mdbx.c++ $(addsuffix .c, $(MDBX_TOOLS)) mdbx-internals.h mdbx-wingetopt.h

ifndef SED
SED        := $(shell which gnu-sed 2>&- || echo sed)
endif
TAR        ?= $(shell which gnu-tar 2>&- || echo tar)
ZIP        ?= $(shell which zip || echo "echo 'Please install zip'")
CLANG_FORMAT ?= $(shell (which clang-format-19 || which clang-format) 2>/dev/null)

reformat:
	@echo '  RUNNING clang-format...'
	$(QUIET)if [ -n "$(CLANG_FORMAT)" ]; then \
		git ls-files --deduplicate | grep -E '\.(c|c++|h|h++)(\.in)?$$' | xargs -r $(CLANG_FORMAT) -i --style=file; \
	else \
		echo "clang-format version 19 not found for 'reformat'"; \
	fi

MAN_SRCDIR := src/man1/
ALLOY_DEPS := $(shell git ls-files --deduplicate src/ | grep -e /tools -e /man -v)
MDBX_GIT_DIR := $(shell if [ -d .git ]; then echo .git; elif [ -s .git -a -f .git ]; then grep '^gitdir: ' .git | cut -d ':' -f 2; else echo git_directory_is_absent; fi)
MDBX_GIT_LASTVTAG := $(shell git describe --tags --dirty=-DIRTY --abbrev=0 '--match=v[0-9]*' 2>&- || echo 'Please fetch tags and/or install non-obsolete git version')
MDBX_GIT_3DOT := $(shell set -o pipefail; echo "$(MDBX_GIT_LASTVTAG)" | $(SED) -n 's|^v*\([0-9]\{1,\}\.[0-9]\{1,\}\.[0-9]\{1,\}\)\(.*\)|\1|p' || echo 'Please fetch tags and/or use non-obsolete git version')
MDBX_GIT_TWEAK := $(shell set -o pipefail; git rev-list $(shell git describe --tags --abbrev=0 '--match=v[0-9]*')..HEAD --count 2>&- || echo 'Please fetch tags and/or use non-obsolete git version')
MDBX_GIT_TIMESTAMP := $(shell git show --no-patch --format=%cI HEAD 2>&- || echo 'Please install latest git version')
MDBX_GIT_DESCRIBE := $(shell git describe --tags --long --dirty '--match=v[0-9]*' 2>&- || echo 'Please fetch tags and/or install non-obsolete git version')
MDBX_GIT_PRERELEASE := $(shell echo "$(MDBX_GIT_LASTVTAG)" | $(SED) -n 's|^v*\([0-9]\{1,\}\.[0-9]\{1,\}\.[0-9]\{1,\}\)\(.*\)-\([-.0-1a-zA-Z]\+\)|\3|p')
MDBX_VERSION_PURE = $(MDBX_GIT_3DOT)$(if $(filter-out 0,$(MDBX_GIT_TWEAK)),.$(MDBX_GIT_TWEAK),)$(if $(MDBX_GIT_PRERELEASE),-$(MDBX_GIT_PRERELEASE),)
MDBX_VERSION_IDENT = $(shell set -o pipefail; echo -n '$(MDBX_GIT_DESCRIBE)' | tr -c -s '[a-zA-Z0-9.]' _)
MDBX_VERSION_NODOT = $(subst .,_,$(MDBX_VERSION_IDENT))
MDBX_BUILD_SOURCERY = $(shell set -o pipefail; $(MAKE) IOARENA=false CXXSTD= -s src/version.c >/dev/null && (openssl dgst -r -sha256 src/version.c || sha256sum src/version.c || shasum -a 256 src/version.c) 2>/dev/null | cut -d ' ' -f 1 || (echo 'Please install openssl or sha256sum or shasum' >&2 && echo sha256sum_is_no_available))_$(MDBX_VERSION_NODOT)
MDBX_DIST_DIR = libmdbx-$(MDBX_VERSION_NODOT)

# Extra options mdbx_test utility
MDBX_SMOKE_EXTRA ?=

check: DESTDIR = $(shell pwd)/@check-install
check: CMAKE_OPT += -Werror=dev
check: clean | smoke-assertion ninja-assertions dist install test ctest
long-test-assertion: smoke-assertion

.PHONY: check-posix-locking-sysv check-posix-locking-1988 check-posix-locking-2001 check-posix-locking-2008
check-posix-locking-sysv: MDBX_LOCKING_OPT = -DMDBX_LOCKING=5
check-posix-locking-1988: MDBX_LOCKING_OPT = -DMDBX_LOCKING=1988
check-posix-locking-2001: MDBX_LOCKING_OPT = -DMDBX_LOCKING=2001
check-posix-locking-2008: MDBX_LOCKING_OPT = -DMDBX_LOCKING=2008
check-posix-locking-sysv: check-posix-locking-run
check-posix-locking-1988: check-posix-locking-run
check-posix-locking-2001: check-posix-locking-run
check-posix-locking-2008: check-posix-locking-run
check-posix-locking-run:
	$(QUIET)$(call cmake-configure-build,@cmake-locking-build,$(MDBX_LOCKING_OPT) -DMDBX_CHECKING=2,) && \
		ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) $(CTEST) --test-dir @cmake-locking-build --output-on-failure $(CTEST_OPT)
check-posix-locking:
	$(QUIET)for LCK in sysv 1988 2001 2008; do $(MAKE) check-posix-locking-$${LCK} || break; done;

smoke: cmake-build
	$(call ctest-scenario-run,@cmake-build,^smoke$$,)

smoke-singleprocess: cmake-build
	$(call ctest-scenario-run,@cmake-build,^smoke-singleprocess$$,)

smoke-fault: cmake-build
	$(call ctest-scenario-run,@cmake-build,^smoke-fault$$,)

smoke-t1: cmake-build
	$(call ctest-scenario-run,@cmake-build,^smoke-t1$$,)

smoke-t2: cmake-build
	$(call ctest-scenario-run,@cmake-build,^smoke-t2$$,)

smoke-t3: cmake-stochastic-build
	$(call ctest-scenario-run,@cmake-stochastic-build,^smoke-t3$$,)

select-tests:
	$(QUIET)tests/select-tests.sh $(SELECT_TESTS_ARGS)

test-stochastic: cmake-stochastic-build
	$(call ctest-scenario-run,@cmake-stochastic-build,^stochastic$$,)

long-test: test-long
test-long: cmake-stochastic-build
	$(call ctest-scenario-run,@cmake-stochastic-build,^stochastic-long$$,)

test-singleprocess: cmake-stochastic-build
	$(call ctest-scenario-run,@cmake-stochastic-build,^stochastic-single$$,)

gcc-analyzer:
	@echo '  RE-BUILD with `-fanalyzer` option...'
	@echo "NOTE: There a lot of false-positive warnings at 2020-05-01 by pre-release GCC-10 (20200328, Red Hat 10.0.1-0.11)"
	$(QUIET)$(call cmake-configure-build,@cmake-analyzer-build,'-DCMAKE_C_FLAGS=-Og -fanalyzer -Wno-error' '-DCMAKE_CXX_FLAGS=-Og -fanalyzer -Wno-error',)

build-stochastic: cmake-build

define tool-rule
mdbx_$(1):	src/tools/$(1).c libmdbx.a
	@echo '  CC+LD $$@'
	$(QUIET)$$(CC) $$(CFLAGS) $$(MDBX_BUILD_OPTIONS) -Isrc '-DMDBX_CONFIG_H="config-gnumake.h"' $$^ $$(LDFLAGS) $$(EXE_LDFLAGS) $$(LIBS) -o $$@

mdbx_$(1).static:	src/tools/$(1).c mdbx-static.o
	@echo '  CC+LD $$@'
	$(QUIET)$$(CC) $$(CFLAGS) $$(MDBX_BUILD_OPTIONS) -Isrc '-DMDBX_CONFIG_H="config-gnumake.h"' $$^ $$(LDFLAGS) $$(EXE_LDFLAGS) $$(LIBS) -static -Wl,--strip-all -o $$@

mdbx_$(1).static-lto: src/tools/$(1).c src/config-gnumake.h src/version.c src/alloy.c $(ALLOY_DEPS)
	@echo '  CC+LD $$@'
	$(QUIET)$$(CC) $$(CFLAGS) -Os -flto $$(MDBX_BUILD_OPTIONS) -Isrc '-DLIBMDBX_API=' '-DMDBX_CONFIG_H="config-gnumake.h"' \
		$$< src/alloy.c $$(LDFLAGS) $$(EXE_LDFLAGS) $$(LIBS) -static -Wl,--strip-all -o $$@

endef
$(foreach file,$(TOOLS),$(eval $(call tool-rule,$(file))))

$(MDBX_GIT_DIR)/HEAD $(MDBX_GIT_DIR)/index $(MDBX_GIT_DIR)/refs/tags:
	@echo '*** ' >&2
	@echo '*** Please don''t use tarballs nor zips which are automatically provided by GitHub !' >&2
	@echo '*** These archives do not contain version information and thus are unfit to build libmdbx.' >&2
	@echo '*** ' >&2
	@echo '*** Instead just follow the https://libmdbx.dqdkfa.ru/usage.html' >&2
	@echo '*** PLEASE, AVOID USING ANY OTHER TECHNIQUES.' >&2
	@echo '*** ' >&2
	@false

define version-rule
$(1): $(2) $(lastword $(MAKEFILE_LIST)) $(MDBX_GIT_DIR)/HEAD $(MDBX_GIT_DIR)/index $(MDBX_GIT_DIR)/refs/tags LICENSE NOTICE COPYRIGHT
	@echo '  MAKE $$@'
	$(QUIET)$$(SED) \
		-e "s|@MDBX_GIT_TIMESTAMP@|$$(MDBX_GIT_TIMESTAMP)|" \
		-e "s|@MDBX_GIT_TREE@|$$(shell git show --no-patch --format=%T HEAD || echo 'Please install latest git version')|" \
		-e "s|@MDBX_GIT_COMMIT@|$$(shell git show --no-patch --format=%H HEAD || echo 'Please install latest git version')|" \
		-e "s|@MDBX_GIT_DESCRIBE@|$$(MDBX_GIT_DESCRIBE)|" \
		-e "s|\$$$${MDBX_VERSION_MAJOR}|$$(shell echo '$$(MDBX_GIT_3DOT)' | cut -d . -f 1)|" \
		-e "s|\$$$${MDBX_VERSION_MINOR}|$$(shell echo '$$(MDBX_GIT_3DOT)' | cut -d . -f 2)|" \
		-e "s|\$$$${MDBX_VERSION_PATCH}|$$(shell echo '$$(MDBX_GIT_3DOT)' | cut -d . -f 3)|" \
		-e "s|\$$$${MDBX_VERSION_TWEAK}|$$(MDBX_GIT_TWEAK)|" \
		-e "s|@MDBX_VERSION_PRERELEASE@|$$(MDBX_GIT_PRERELEASE)|" \
		-e "s|@MDBX_VERSION_PURE@|$$(MDBX_VERSION_PURE)|" \
	$(2) >$$@

endef

$(eval $(call version-rule, src/version.c, src/version.c.in))

$(eval $(call version-rule, $(DIST_DIR)/@tmp-amalgam.inc, src/amalgam.in))

src/config-gnumake.h: @buildflags.tag src/version.c $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  MAKE $@'
	$(QUIET)(echo '#define MDBX_BUILD_TIMESTAMP "$(MDBX_BUILD_TIMESTAMP)"' \
	&& echo "#define MDBX_BUILD_FLAGS \"$$(cat @buildflags.tag)\"" \
	&& echo '#define MDBX_BUILD_COMPILER "$(shell (LC_ALL=C $(CC) --version || echo 'Please use GCC or CLANG compatible compiler') | head -1)"' \
	&& echo '#define MDBX_BUILD_TARGET "$(shell set -o pipefail; (LC_ALL=C $(CC) -v 2>&1 | grep -i '^Target:' | cut -d ' ' -f 2- || (LC_ALL=C $(CC) --version | grep -qi e2k && echo E2K) || echo 'Please use GCC or CLANG compatible compiler') | head -1)"' \
	&& echo '#define MDBX_BUILD_SOURCERY $(MDBX_BUILD_SOURCERY)' \
	&& echo '#define MDBX_BUILD_CXX $(call select_by,MDBX_BUILD_CXX,1,0)' \
	&& echo '#define MDBX_BUILD_METADATA "$(MDBX_BUILD_METADATA)"' \
	) >$@

mdbx-dylib.o: src/config-gnumake.h src/version.c src/alloy.c $(ALLOY_DEPS) $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  CC $@'
	$(QUIET)$(CC) $(CFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -DLIBMDBX_EXPORTS=1 -c src/alloy.c -o $@

mdbx-static.o: src/config-gnumake.h src/version.c src/alloy.c $(ALLOY_DEPS) $(lastword $(MAKEFILE_LIST)) LICENSE NOTICE COPYRIGHT
	@echo '  CC $@'
	$(QUIET)$(CC) $(CFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -ULIBMDBX_EXPORTS -c src/alloy.c -o $@

docs/Doxyfile: docs/Doxyfile.in src/version.c $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)$(SED) \
		-e "s|@MDBX_GIT_TIMESTAMP@|$(MDBX_GIT_TIMESTAMP)|" \
		-e "s|@MDBX_GIT_TREE@|$(shell git show --no-patch --format=%T HEAD || echo 'Please install latest git version')|" \
		-e "s|@MDBX_GIT_COMMIT@|$(shell git show --no-patch --format=%H HEAD || echo 'Please install latest git version')|" \
		-e "s|@MDBX_GIT_DESCRIBE@|$(MDBX_GIT_DESCRIBE)|" \
		-e "s|\$${MDBX_VERSION_MAJOR}|$(shell echo '$(MDBX_GIT_3DOT)' | cut -d . -f 1)|" \
		-e "s|\$${MDBX_VERSION_MINOR}|$(shell echo '$(MDBX_GIT_3DOT)' | cut -d . -f 2)|" \
		-e "s|\$${MDBX_VERSION_PATCH}|$(shell echo '$(MDBX_GIT_3DOT)' | cut -d . -f 3)|" \
		-e "s|\$${MDBX_VERSION_TWEAK}|$(MDBX_GIT_TWEAK)|" \
		-e "s|@MDBX_VERSION_PRERELEASE@|$(MDBX_GIT_PRERELEASE)|" \
		-e "s|@MDBX_VERSION_PURE@|$(MDBX_VERSION_PURE)|" \
	docs/Doxyfile.in >$@

define md-extract-section
docs/__$(1).md: $(2) $(lastword $(MAKEFILE_LIST))
	@echo '  EXTRACT $1'
	$(QUIET)$(SED) -n '/<!-- section-begin $(1) -->/,/<!-- section-end -->/p' $(2) >$$@ && test -s $$@

endef
$(foreach section,overview mithril characteristics improvements history usage performance bindings,$(eval $(call md-extract-section,$(section),$(DIST_DIR)/README.md)))

docs/contrib.fame: src/version.c $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)echo "" > $@ && git fame --show-email --format=md --silent-progress -w -M -C | grep '^|' >> $@

docs/overall.md: docs/__overview.md docs/_toc.md docs/__mithril.md docs/__history.md COPYRIGHT LICENSE NOTICE $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)echo -e "\\mainpage Overall\n\\section brief Brief" | cat - $(filter %.md, $^) >$@ && echo -e "\n\n\nLicense\n=======\n" | cat - LICENSE >>$@

docs/intro.md: docs/_preface.md docs/__characteristics.md docs/__improvements.md docs/_restrictions.md
	@echo '  MAKE $@'
	$(QUIET)cat $^ | $(SED) 's/^Performance comparison$$/Performance comparison {#performance}/;s/^Improvements beyond LMDB$$/Improvements beyond LMDB {#improvements}/' >$@

docs/usage.md: docs/__usage.md docs/_starting.md docs/__bindings.md
	@echo '  MAKE $@'
	$(QUIET)echo -e "\\page usage Usage\n\\section getting Building & Embedding" | cat - $^ | $(SED) 's/^Bindings$$/Bindings {#bindings}/' >$@

doxygen: docs/Doxyfile docs/overall.md docs/intro.md docs/usage.md $(DIST_DIR)/mdbx.h $(DIST_DIR)/mdbx.h++ src/options.h ChangeLog.md COPYRIGHT LICENSE NOTICE docs/ld+json $(lastword $(MAKEFILE_LIST))
	@echo '  RUNNING doxygen...'
	$(QUIET)rm -rf docs/html && \
	cat $(DIST_DIR)/mdbx.h | tr '\n' '\r' | $(SED) -e 's/LIBMDBX_INLINE_API\s*(\s*\([^,]\+\),\s*\([^,]\+\),\s*(\s*\([^)]\+\)\s*)\s*)\s*{/inline \1 \2(\3) {/g' | tr '\r' '\n' >docs/mdbx.h && \
	cp $(DIST_DIR)/mdbx.h++ src/options.h ChangeLog.md docs/ && (cd docs && doxygen Doxyfile $(HUSH)) && cp COPYRIGHT LICENSE NOTICE docs/html/ && \
	$(SED) -i docs/html/index.html -e '/\/MathJax.js"><\/script>/r docs/ld+json' -e 's/<title>libmdbx: Overall<\/title>//;T;r docs/title' && \
	$(SED) -i docs/html/sitemap.xml -e '/^\s*<\/urlset>/e cat docs/sitemap.add'

mdbx++-dylib.o: src/config-gnumake.h src/mdbx.c++ $(HEADERS) $(lastword $(MAKEFILE_LIST))
	@echo '  CC $@'
	$(QUIET)$(CXX) $(CXXFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -DLIBMDBX_EXPORTS=1 -c src/mdbx.c++ -o $@

mdbx++-static.o: src/config-gnumake.h src/mdbx.c++ $(HEADERS) $(lastword $(MAKEFILE_LIST))
	@echo '  CC $@'
	$(QUIET)$(CXX) $(CXXFLAGS) $(MDBX_BUILD_OPTIONS) '-DMDBX_CONFIG_H="config-gnumake.h"' -ULIBMDBX_EXPORTS -c src/mdbx.c++ -o $@

dist: tags $(WAIT) @dist-checked.tag libmdbx-sources-$(MDBX_VERSION_IDENT).tar.gz $(lastword $(MAKEFILE_LIST))
	@echo '  AMALGAMATION is done'

tags:
	@echo '  FETCH git tags...'
	$(QUIET)git fetch --tags --force

release-assets: libmdbx-amalgamated-$(MDBX_GIT_3DOT).zpaq \
	libmdbx-amalgamated-$(MDBX_GIT_3DOT).tar.xz \
	libmdbx-amalgamated-$(MDBX_GIT_3DOT).tar.bz2 \
	libmdbx-amalgamated-$(MDBX_GIT_3DOT).tar.gz \
	libmdbx-amalgamated-$(subst .,_,$(MDBX_GIT_3DOT)).zip
	$(QUIET)([ \
		"$$(set -o pipefail; git describe | $(SED) -n '/^v[0-9]\{1,\}\.[0-9]\{1,\}\.[0-9]\{1,\}$$/p' || echo fail-left)" \
	== \
		"$$(git describe --tags --dirty=-dirty || echo fail-right)" ] \
		|| (echo 'ERROR: Is not a valid release because not in the clean state with a suitable annotated tag!!!' >&2 && false)) \
	&& echo '  RELEASE ASSETS are done'

@dist-checked.tag: $(addprefix $(DIST_DIR)/, $(DIST_SRC) $(DIST_EXTRA))
	@echo -n '  VERIFY amalgamated sources...'
	$(QUIET)rm -rf $@ $(DIST_DIR)/@tmp-squashed.inc $(DIST_DIR)/@tmp-amalgam.inc \
	&& if grep -R "define xMDBX_ALLOY" dist | grep -q MDBX_BUILD_SOURCERY; then echo "sed output is WRONG!" >&2; exit 2; fi \
	&& rm -rf @dist-check && cp -r -p $(DIST_DIR) @dist-check && ($(MAKE) -j IOARENA=false CXXSTD=$(CXXSTD) -C @dist-check all check ninja-assertions >@dist-check.log 2>@dist-check.err || (cat @dist-check.err && exit 1)) \
	&& touch $@ || (echo " FAILED! See @dist-check.log and @dist-check.err" >&2; exit 2) && echo " Ok"

%.tar.gz: @dist-checked.tag
	@echo '  CREATE $@'
	$(QUIET)$(TAR) -c $(shell LC_ALL=C $(TAR) --help | grep -q -- '--owner' && echo '--owner=0 --group=0') -f - -C dist $(DIST_SRC) $(DIST_EXTRA) | gzip -c -9 >$@

%.tar.xz: @dist-checked.tag
	@echo '  CREATE $@'
	$(QUIET)$(TAR) -c $(shell LC_ALL=C $(TAR) --help | grep -q -- '--owner' && echo '--owner=0 --group=0') -f - -C dist $(DIST_SRC) $(DIST_EXTRA) | xz -9 -z >$@

%.tar.bz2: @dist-checked.tag
	@echo '  CREATE $@'
	$(QUIET)$(TAR) -c $(shell LC_ALL=C $(TAR) --help | grep -q -- '--owner' && echo '--owner=0 --group=0') -f - -C dist $(DIST_SRC) $(DIST_EXTRA) | bzip2 -9 -z >$@

%.zip: @dist-checked.tag
	@echo '  CREATE $@'
	$(QUIET)rm -rf $@ && (cd dist && $(ZIP) -9 ../$@ $(DIST_SRC) $(DIST_EXTRA)) &>@zip.log

%.zpaq: @dist-checked.tag
	@echo '  CREATE $@'
	$(QUIET)rm -rf $@ && (cd dist && zpaq a ../$@ $(DIST_SRC) $(DIST_EXTRA) -m59) &>@zpaq.log

$(DIST_DIR)/mdbx-internals.h: src/version.c $(DIST_DIR)/@tmp-amalgam.inc $(ALLOY_DEPS) $(lastword $(MAKEFILE_LIST))
	@echo '  ALLOYING...'
	$(QUIET)mkdir -p dist \
	&& (grep -v '#include ' src/alloy.c && echo '#define MDBX_BUILD_SOURCERY $(MDBX_BUILD_SOURCERY)' \
	&& $(SED) \
		-e '/#include "preface.h"/r src/preface.h' \
		-e '/#include "osal.h"/r src/osal.h' \
		-e '/#include "options.h"/r src/options.h' \
		-e '/#include "atomics-types.h"/r src/atomics-types.h' \
		-e '/#include "layout-dxb.h"/r src/layout-dxb.h' \
		-e '/#include "layout-lck.h"/r src/layout-lck.h' \
		-e '/#include "logging_and_debug.h"/r src/logging_and_debug.h' \
		-e '/#include "utils.h"/r src/utils.h' \
		-e '/#include "pnl.h"/r src/pnl.h' \
		src/essentials.h \
	| $(SED) \
		-e 's|#include "../mdbx.h"|@INCLUDE "mdbx.h"|' \
		-e '/#pragma once/d' \
		-e '/#include "/d' \
		-e 's|@INCLUDE|#include|' \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
	) \
	| grep -v '^///' | cat -s $(DIST_DIR)/@tmp-amalgam.inc - >$@

$(DIST_DIR)/@tmp-squashed.inc: $(DIST_DIR)/mdbx-internals.h $(DIST_DIR)/@tmp-amalgam.inc src/version.c $(ALLOY_DEPS) $(lastword $(MAKEFILE_LIST))
	$(QUIET)($(SED) \
		-e 's|#include "essentials.h"|@INCLUDE "mdbx-internals.h"|' \
		-e '/#include "atomics-ops.h"/r src/atomics-ops.h' \
		-e '/#include "proto.h"/r src/proto.h' \
		-e '/#include "rkl.h"/r src/rkl.h' \
		-e '/#include "txl.h"/r src/txl.h' \
		-e '/#include "unaligned.h"/r src/unaligned.h' \
		-e '/#include "comparators.h"/r src/comparators.h' \
		-e '/#include "cogs.h"/r src/cogs.h' \
		-e '/#include "cursor.h"/r src/cursor.h' \
		-e '/#include "dbi.h"/r src/dbi.h' \
		-e '/#include "dml.h"/r src/dml.h' \
		-e '/#include "dpl.h"/r src/dpl.h' \
		-e '/#include "gc.h"/r src/gc.h' \
		-e '/#include "lck.h"/r src/lck.h' \
		-e '/#include "meta.h"/r src/meta.h' \
		-e '/#include "node.h"/r src/node.h' \
		-e '/#include "page-iov.h"/r src/page-iov.h' \
		-e '/#include "page-ops.h"/r src/page-ops.h' \
		-e '/#include "spill.h"/r src/spill.h' \
		-e '/#include "sort.h"/r src/sort.h' \
		-e '/#include "rthc.h"/r src/rthc.h' \
		-e '/#include "walk.h"/r src/walk.h' \
		-e '/#include "windows-import.h"/r src/windows-import.h' \
		src/internals.h \
	| $(SED) \
		-e '/#pragma once/d' \
		-e '/#include "/d' \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
	| grep -v '^///') >$@

$(DIST_DIR)/mdbx.c: $(DIST_DIR)/@tmp-squashed.inc $(DIST_DIR)/@tmp-amalgam.inc $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)(cat $(DIST_DIR)/@tmp-squashed.inc $(shell git ls-files --deduplicate src/*.c | grep -v alloy) src/version.c | $(SED) \
		-e '/#include "debug_begin.h"/r src/debug_begin.h' \
		-e '/#include "debug_end.h"/r src/debug_end.h' \
	) | $(SED) \
		-e '/#include "/d;/#pragma once/d' \
		-e 's|@INCLUDE|#include|' \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
	| grep -v '^///' | cat -s $(DIST_DIR)/@tmp-amalgam.inc - >$@

$(DIST_DIR)/mdbx.h++: $(filter %h++, $(HEADERS)) $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)cat mdbx.h++ | $(SED) \
		-e '/#include "mdbx++\/begin.h++"/r mdbx++/begin.h++' \
		-e '/#include "mdbx++\/decl_exceptions.h++"/r mdbx++/decl_exceptions.h++' \
		-e '/#include "mdbx++\/decl_slice.h++"/r mdbx++/decl_slice.h++' \
		-e '/#include "mdbx++\/decl_transcoders.h++"/r mdbx++/decl_transcoders.h++' \
		-e '/#include "mdbx++\/decl_buffer.h++"/r mdbx++/decl_buffer.h++' \
		-e '/#include "mdbx++\/decl_core.h++"/r mdbx++/decl_core.h++' \
		-e '/#include "mdbx++\/decl_env.h++"/r mdbx++/decl_env.h++' \
		-e '/#include "mdbx++\/decl_txn.h++"/r mdbx++/decl_txn.h++' \
		-e '/#include "mdbx++\/decl_cursor.h++"/r mdbx++/decl_cursor.h++' \
		-e '/#include "mdbx++\/impl_slice.h++"/r mdbx++/impl_slice.h++' \
		-e '/#include "mdbx++\/impl_buffer.h++"/r mdbx++/impl_buffer.h++' \
		-e '/#include "mdbx++\/impl_core.h++"/r mdbx++/impl_core.h++' \
		-e '/#include "mdbx++\/impl_exceptions.h++"/r mdbx++/impl_exceptions.h++' \
		-e '/#include "mdbx++\/impl_env.h++"/r mdbx++/impl_env.h++' \
		-e '/#include "mdbx++\/impl_txn.h++"/r mdbx++/impl_txn.h++' \
		-e '/#include "mdbx++\/impl_cursor.h++"/r mdbx++/impl_cursor.h++' \
		-e '/#include "mdbx++\/end.h++"/r mdbx++/end.h++' \
		-e '/#include "xyz"/r xyz' \
	| $(SED) \
		-e '/dist-cutoff-begin/,/dist-cutoff-end/d' \
		-e 's|#include "../mdbx.h"|@INCLUDE "mdbx.h"|' \
	| $(SED) \
		-e "s|@MDBX_GIT_TIMESTAMP@|$(MDBX_GIT_TIMESTAMP)|" \
		-e "s|@MDBX_GIT_DESCRIBE@|$(MDBX_GIT_DESCRIBE)|" \
		-e '/#include "/d' \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
		-e 's|@INCLUDE|#include|' \
	| cat -s >$@

$(DIST_DIR)/mdbx.c++: $(DIST_DIR)/@tmp-squashed.inc $(DIST_DIR)/@tmp-amalgam.inc src/mdbx.c++ $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)$(SED) -e '/#ifndef __cplusplus/,/#endif \/\* !__cplusplus \*\//d' $(DIST_DIR)/@tmp-squashed.inc | \
	cat - src/mdbx.c++ | \
	$(SED) \
		-e 's|#include "../mdbx.h++"|@INCLUDE "mdbx.h++"|' \
		-e '/#include "/d' \
		-e 's|@INCLUDE|#include|' \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
	| grep -v '^///' | cat -s $(DIST_DIR)/@tmp-amalgam.inc - >$@

$(DIST_DIR)/mdbx-wingetopt.h: src/tools/wingetopt.h src/tools/wingetopt.c
	@echo '  MAKE $@'
	$(QUIET)mkdir -p dist && cat $^ | \
	$(SED) \
		-e '/#include "/d' \
		-e '/#pragma once/d' \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
	| grep -v '^///' | cat -s >$@

define dist-tool-rule
$(DIST_DIR)/mdbx_$(1).c: src/tools/$(1).c \
		$(DIST_DIR)/mdbx-internals.h $(lastword $(MAKEFILE_LIST)) $(DIST_DIR)/@tmp-amalgam.inc
	@echo '  MAKE $$@'
	$(QUIET)mkdir -p dist && \
	$(SED) \
		-e 's|#include "essentials.h"|@INCLUDE "mdbx-internals.h"|' \
		-e 's|#include "wingetopt.h"|@INCLUDE "mdbx-wingetopt.h"|' \
		-e '/#include "/d;/#pragma once/d;/#define xMDBX_ALLOY/d' \
		-e 's|@INCLUDE|#include|' \
		src/tools/$(1).c \
	| $(SED) \
		-e "s|@MDBX_GIT_TIMESTAMP@|$$(MDBX_GIT_TIMESTAMP)|" \
		-e "s|@MDBX_GIT_DESCRIBE@|$$(MDBX_GIT_DESCRIBE)|" \
		-e '/ clang-format o/d;/ \*INDENT-O/d' \
	| grep -v '^///' | cat -s $(DIST_DIR)/@tmp-amalgam.inc - >$$@

endef
$(foreach file,$(TOOLS),$(eval $(call dist-tool-rule,$(file))))

define dist-extra-rule
$(1): $(2) src/version.c $(lastword $(MAKEFILE_LIST))
	@echo '  REFINE $$@'
	$(QUIET)mkdir -p $$(dir $$@) && $(SED) \
		-e '/^\s*#> dist-cutoff-begin/,/^\s*#< dist-cutoff-end/d' \
		-e '/^\s*\/\/\s*> dist-cutoff-begin/,/^\s*\/\/\s*< dist-cutoff-end/d' \
		-e '/^\s*\/\*\s*> dist-cutoff-begin/,/^\s*\/\*\s*< dist-cutoff-end/d' \
		-e '/^\s*<!-- dist-cutoff-begin -->/,/^\s*<!-- dist-cutoff-end -->/d' \
		-e "s|@MDBX_GIT_TIMESTAMP@|$$(MDBX_GIT_TIMESTAMP)|" \
		-e "s|@MDBX_GIT_DESCRIBE@|$$(MDBX_GIT_DESCRIBE)|" \
	$$< | cat -s >$$@

endef
$(foreach file,mdbx.h $(filter-out man1/% VERSION.json .clang-format-ignore %.in ntdll.def windows-safeseh%, $(DIST_EXTRA)), $(eval $(call dist-extra-rule, $(DIST_DIR)/$(file), $(file))))

$(DIST_DIR)/VERSION.json: src/version.c
	@echo '  MAKE $@'
	$(QUIET)mkdir -p $(DIST_DIR)/ && echo "{ \"git_describe\": \"$(MDBX_GIT_DESCRIBE)\", \"git_timestamp\": \"$(MDBX_GIT_TIMESTAMP)\", \"git_tree\": \"$(shell git show --no-patch --format=%T HEAD 2>&1)\", \"git_commit\": \"$(shell git show --no-patch --format=%H HEAD 2>&1)\", \"semver\": \"$(MDBX_VERSION_PURE)\" }" >$@

$(DIST_DIR)/.clang-format-ignore: $(lastword $(MAKEFILE_LIST))
	@echo '  MAKE $@'
	$(QUIET)echo "$(filter-out %.h %h++,$(DIST_SRC))" | tr ' ' \\n > $@

define dist-copy-rule
$(DIST_DIR)/$(1): $(2) $(lastword $(MAKEFILE_LIST))
	@echo '  COPY $$@'
	$(QUIET)mkdir -p $(DIST_DIR)/ && cp $$< $$@

endef
$(foreach file,$(addprefix src/,ntdll.def windows-safeseh-masm.asm windows-safeseh-yasm.asm windows-safeseh.obj), $(eval $(call dist-copy-rule,$(notdir $(file)),$(file))))

$(eval $(call dist-extra-rule, $(DIST_DIR)/config.h.in, src/config.h.in))

$(eval $(call dist-extra-rule, $(DIST_DIR)/man1/mdbx_%.1, src/man1/mdbx_%.1))

endif

################################################################################
# Cross-compilation simple test

CROSS_LIST = \
	aarch64-linux-gnu-gcc \
	arm-linux-gnueabihf-gcc \
	hppa-linux-gnu-gcc \
	powerpc64-linux-gnu-gcc\
	riscv64-linux-gnu-gcc \
	s390x-linux-gnu-gcc \
	sh4-linux-gnu-gcc

## On Ubuntu Noble (24.04.2) with QEMU 8.2 (8.2.2+ds-0ubuntu1.7) & GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04)
# sparc64-linux-gnu-gcc       - fails mmap/BAD_ADDRESS (previously: qemu-coredump since mmap-troubles, qemu fails fcntl for F_SETLK/F_GETLK)
# alpha-linux-gnu-gcc         - qemu-coredump (qemu mmap-troubles)
# powerpc-linux-gnu-gcc       - fails mmap/BAD_ADDRESS (previously: qemu-coredump since mmap-troubles, qemu fails fcntl for F_SETLK/F_GETLK)
# mips*-linux-gnuabi*-gcc     - unavailable since 2026
CROSS_LIST_NOQEMU = sparc64-linux-gnu-gcc alpha-linux-gnu-gcc powerpc-linux-gnu-gcc

cross-gcc:
	@echo '  Re-building by cross-compiler for: $(CROSS_LIST_NOQEMU) $(CROSS_LIST)'
	@echo "CORRESPONDING CROSS-COMPILERs ARE REQUIRED."
	@echo "FOR INSTANCE: sudo apt install \$$(apt list 'g++-*' | grep 'g++-[a-z0-9]\+-linux-gnu/' | cut -f 1 -d / | sort -u)"
	$(QUIET)for CC in $(CROSS_LIST_NOQEMU) $(CROSS_LIST); do \
		echo "===================== $$CC"; \
		BD=@cmake-cross-$$(echo $$CC | tr -c 'a-zA-Z0-9' _); \
		rm -rf $$BD; \
		$(CMAKE) -S . -B $$BD -DCMAKE_C_COMPILER=$$CC -DCMAKE_CXX_COMPILER=$$(echo $$CC | $(SED) 's/-gcc/-g++/') \
			-DCMAKE_EXE_LINKER_FLAGS=-static -DMDBX_ENABLE_TESTS=OFF || exit $$?; \
		$(CMAKE) --build $$BD || exit $$?; \
	done

# Unfortunately qemu don't provide robust support for futexes.
# Therefore it is impossible to run full multi-process tests.
cross-qemu:
	@echo '  Re-building by cross-compiler and re-check by QEMU for: $(CROSS_LIST)'
	@echo "CORRESPONDING CROSS-COMPILERs AND QEMUs ARE REQUIRED."
	@echo "FOR INSTANCE: "
	@echo "	1) sudo apt install \$$(apt list 'g++-*' | grep 'g++-[a-z0-9]\+-linux-gnu/' | cut -f 1 -d / | sort -u)"
	@echo "	2) sudo apt install binfmt-support qemu-user-static qemu-user \$$(apt list 'qemu-system-*' | grep 'qemu-system-[a-z0-9]\+/' | cut -f 1 -d / | sort -u)"
	$(QUIET)for CC in $(CROSS_LIST); do \
		echo "===================== $$CC + qemu"; \
		BD=@cmake-cross-$$(echo $$CC | tr -c 'a-zA-Z0-9' _); \
		rm -rf $$BD; \
		$(CMAKE) -S . -B $$BD -DCMAKE_C_COMPILER=$$CC -DCMAKE_CXX_COMPILER=$$(echo $$CC | $(SED) 's/-gcc/-g++/') \
			-DCMAKE_EXE_LINKER_FLAGS=-static -DMDBX_LOCKING=5 \
			-DCMAKE_C_FLAGS=-DMDBX_SAFE4QEMU -DCMAKE_CXX_FLAGS=-DMDBX_SAFE4QEMU -DMDBX_ENABLE_TESTS=ON \
			-DMDBX_ENABLE_LONG_TESTS=ON || exit $$?; \
		$(CMAKE) --build $$BD || exit $$?; \
		ASAN_OPTIONS=$(ASAN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_OPTIONS) $(CTEST) --test-dir $$BD \
			--label-regex '^smoke-singleprocess$$|^stochastic-single$$' --output-on-failure || exit $$?; \
	done

#< dist-cutoff-end

install: $(LIBRARIES) $(MDBX_TOOLS) $(HEADERS)
	@echo '  INSTALLING...'
	$(QUIET)mkdir -p $(DESTDIR)$(prefix)/bin$(suffix) && \
		$(INSTALL) -p $(EXE_INSTALL_FLAGS) $(MDBX_TOOLS) $(DESTDIR)$(prefix)/bin$(suffix)/ && \
	mkdir -p $(DESTDIR)$(prefix)/lib$(suffix)/ && \
		$(INSTALL) -p $(EXE_INSTALL_FLAGS) $(filter-out libmdbx.a,$(LIBRARIES)) $(DESTDIR)$(prefix)/lib$(suffix)/ && \
	mkdir -p $(DESTDIR)$(prefix)/lib$(suffix)/ && \
		$(INSTALL) -p libmdbx.a $(DESTDIR)$(prefix)/lib$(suffix)/ && \
	mkdir -p $(DESTDIR)$(prefix)/include/ && \
		$(INSTALL) -p -m 444 $(HEADERS) $(DESTDIR)$(prefix)/include/ && \
	mkdir -p $(DESTDIR)$(mandir)/man1/ && \
		$(INSTALL) -p -m 444 $(addprefix $(MAN_SRCDIR), $(MANPAGES)) $(DESTDIR)$(mandir)/man1/

install-strip: EXE_INSTALL_FLAGS = -s
install-strip: install

install-no-strip: EXE_INSTALL_FLAGS =
install-no-strip: install

uninstall:
	@echo '  UNINSTALLING/REMOVE...'
	$(QUIET)rm -f $(addprefix $(DESTDIR)$(prefix)/bin$(suffix)/,$(MDBX_TOOLS)) \
		$(addprefix $(DESTDIR)$(prefix)/lib$(suffix)/,$(LIBRARIES)) \
		$(addprefix $(DESTDIR)$(prefix)/include/,$(HEADERS)) \
		$(addprefix $(DESTDIR)$(mandir)/man1/,$(MANPAGES))

################################################################################
# Benchmarking by ioarena

ifeq ($(origin IOARENA),undefined)
IOARENA := $(shell \
  (test -x ../ioarena/@BUILD/src/ioarena && echo ../ioarena/@BUILD/src/ioarena) || \
  (test -x ../../@BUILD/src/ioarena && echo ../../@BUILD/src/ioarena) || \
  (test -x ../../src/ioarena && echo ../../src/ioarena) || which ioarena 2>&- || \
  (echo false && echo '$(TIP) Clone and build the https://sourcecraft.dev/dqdkfa/ioarena within a neighbouring directory for availability of benchmarking.' >&2))
endif
NN	?= 25000000
BENCH_CRUD_MODE ?= nosync

bench-clean:
	@echo '  REMOVE bench-*.txt _ioarena/*'
	$(QUIET)rm -rf bench-*.txt _ioarena/*

re-bench: bench-clean bench

ifeq ($(or $(IOARENA),false),false)
bench bench-quartet bench-triplet bench-couple:
	$(QUIET)echo 'The `ioarena` benchmark is required.' >&2 && \
	echo 'Please clone and build the https://abf.io/erthink/ioarena.git within a neighbouring `ioarena` directory.' >&2 && \
	false

else

.PHONY: bench bench-clean bench-couple re-bench bench-quartet bench-triplet

define bench-rule
bench-$(1)_$(2).txt: $(3) $(IOARENA) $(lastword $(MAKEFILE_LIST))
	@echo '  RUNNING ioarena for $1/$2...'
	$(QUIET)(export LD_LIBRARY_PATH="./:$$$${LD_LIBRARY_PATH}"; \
		ldd $(IOARENA) | grep -i $(1) && \
		$(IOARENA) -D $(1) -B batch -m $(BENCH_CRUD_MODE) -n $(2) \
			| tee $$@ | grep throughput | $(SED) 's/throughput/batch×N/' && \
		$(IOARENA) -D $(1) -B crud -m $(BENCH_CRUD_MODE) -n $(2) \
			| tee -a $$@ | grep throughput | $(SED) 's/throughput/   crud/' && \
		$(IOARENA) -D $(1) -B iterate,get,iterate,get,iterate -m $(BENCH_CRUD_MODE) -r 4 -n $(2) \
			| tee -a $$@ | grep throughput | $(SED) '0,/throughput/{s/throughput/iterate/};s/throughput/    get/' && \
		$(IOARENA) -D $(1) -B delete -m $(BENCH_CRUD_MODE) -n $(2) \
			| tee -a $$@ | grep throughput | $(SED) 's/throughput/ delete/' && \
	true) || mv -f $$@ $$@.error

endef


$(eval $(call bench-rule,mdbx,$(NN),libmdbx.$(SO_SUFFIX)))

$(eval $(call bench-rule,sophia,$(NN)))
$(eval $(call bench-rule,leveldb,$(NN)))
$(eval $(call bench-rule,rocksdb,$(NN)))
$(eval $(call bench-rule,wiredtiger,$(NN)))
$(eval $(call bench-rule,forestdb,$(NN)))
$(eval $(call bench-rule,lmdb,$(NN)))
$(eval $(call bench-rule,nessdb,$(NN)))
$(eval $(call bench-rule,sqlite3,$(NN)))
$(eval $(call bench-rule,ejdb,$(NN)))
$(eval $(call bench-rule,vedisdb,$(NN)))
$(eval $(call bench-rule,dummy,$(NN)))
bench: bench-mdbx_$(NN).txt
bench-quartet: bench-mdbx_$(NN).txt bench-lmdb_$(NN).txt bench-rocksdb_$(NN).txt bench-wiredtiger_$(NN).txt
bench-triplet: bench-mdbx_$(NN).txt bench-lmdb_$(NN).txt bench-sqlite3_$(NN).txt
bench-couple: bench-mdbx_$(NN).txt bench-lmdb_$(NN).txt

# $(eval $(call bench-rule,debug,10))
# .PHONY: bench-debug
# bench-debug: bench-debug_10.txt

endif
