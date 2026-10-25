/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// \file Precompiled header shared by every gtest-based test target.
///
/// It gathers the headers that virtually every test includes anyway (GoogleTest,
/// the shared test environment, the libmdbx C and C++ APIs and the common part
/// of the standard library). Parsing them costs ~2 seconds per translation unit,
/// so compiling them once into a precompiled header and reusing it across all
/// ~120 test binaries (see cmake/testing.cmake, target_precompile_headers with
/// REUSE_FROM) saves minutes on a clean build.
///
/// Keep this header free of any project-local define that is not set for every
/// consumer: REUSE_FROM requires the compile flags of all reusing targets to
/// match the anchor target exactly, so a per-target macro (e.g. MDBX_PROBES)
/// must NOT appear here. The libmdbx public headers (mdbx.h / mdbx.h++) are
/// deliberately NOT precompiled: a few white-box tests include the internal
/// src/essentials.h + internals.h instead, which redefine macros (e.g.
/// MDBX_DEPRECATED) that the public mdbx.h also defines, so baking mdbx.h into
/// the PCH would collide with them under -Werror. Each test includes the
/// libmdbx header it needs directly. Adding a header that is only needed by a
/// few tests is counterproductive — it would be parsed for all of them; include
/// such a header directly in the tests that need it.

#pragma once

/* GoogleTest and the shared test environment (runtime limiter, CI popup
 * suppression). test-env.h already includes <gtest/gtest.h>. */
#include "test-env.h"

/* The common part of the standard library used across the tests. */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <iosfwd>
#include <iostream>
#include <istream>
#include <iterator>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <ostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
