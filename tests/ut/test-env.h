/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// Common test-environment header for the gtest-based unit tests.
///
/// Provides:
///  - class runtime_limiter: a CI execution-time guard (pattern adopted from
///    libfpta's GTEST_IS_EXECUTION_TIMEOUT). The limit in seconds is read from
///    the GTEST_RUNTIME_LIMIT environment variable at construction; when the
///    deadline is exceeded the test reports "[  SKIPPED ] RUNTIME_LIMIT was
///    reached" once and the caller is expected to GTEST_SKIP() itself. It is
///    intended to be used together with GTEST_SHUFFLE=1 so that a part of the
///    tests runs in random order until the limit is hit, instead of the whole
///    run being killed by the CI timeout.
///
///    ADOPTION RULES:
///      - place the MDBX_GTEST_IS_EXECUTION_TIMEOUT() check at the very top of
///        EVERY TEST() body that should be skippable; do NOT put it into a
///        fixture's SetUp()/TearDown() or a helper called once per binary,
///        otherwise the deadline is evaluated once instead of per test;
///      - GTEST_SHUFFLE=1 is set by the CI scripts for nightly runs only
///        (CI_NIGHTLY=1), so routine cells execute in the deterministic gtest
///        order and the limiter skips the same tail of the suite every time.
///
///  - mdtest_ci_crt_suppress_popup(): on Windows and ONLY on CI (env CI or
///    MDBX_TEST_NO_CRT_POPUP is set) redirects the CRT debug/assert popups to
///    stderr so a debug assertion does not hang the headless runner waiting
///    for a dialog; the OS error dialogs are suppressed for every _WIN32
///    toolchain, while the CRT debug-report calls are MSVC-specific
///    (MinGW defines _WIN32 but its <crtdbg.h> pairing is unreliable).
///    Locally the popups are preserved for interactive debugging.
///
/// Both the single `ci_runtime_limiter` instance and the single popup
/// suppressor object are defined in test-env.c++, which cmake compiles into
/// EVERY gtest target (see add_gtest() in cmake/testing.cmake) — exactly one
/// copy per test binary, constructed before main(), regardless of whether
/// that binary references the limiter. Including this header alone adds no
/// objects and has no side effects.

#pragma once

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
/* The CRT debug-report API is MSVC-specific: MinGW defines _WIN32 but its
 * <crtdbg.h>/msvcrt pairing does not reliably provide _CrtSetReportMode,
 * so the calls below are compiled for MSVC only. */
#ifdef _MSC_VER
#include <crtdbg.h>
#endif /* _MSC_VER */
#endif /* _WIN32 */

/* MSVC (/W4 /WX) and clang-cl (-Werror) promote the CRT C4996 'getenv is
 * unsafe' deprecation to a hard error; wrap the lookup exactly like
 * get_cached.c++/crash_safety.c++ do (see f383ac96). */
static inline const char *mdtest_getenv(const char *name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  const char *value = ::getenv(name);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return value;
}

/* Ограничитель по времени выполнения (образец: libfpta fpta_test.h).
 * Нужен для предотвращения таймаута тестов в CI. Предполагается, что он
 * используется вместе с установкой GTEST_SHUFFLE=1, что в сумме дает
 * выполнение части тестов в случайном порядке, пока не будет превышен лимит,
 * заданный через переменную среды окружения GTEST_RUNTIME_LIMIT. */
class runtime_limiter {
  const time_t edge;

  static time_t fetch() {
    const char *const GTEST_RUNTIME_LIMIT = mdtest_getenv("GTEST_RUNTIME_LIMIT");
    if (GTEST_RUNTIME_LIMIT) {
      const long limit = atol(GTEST_RUNTIME_LIMIT);
      if (limit > 0)
        return time(nullptr) + static_cast<time_t>(limit);
    }
    return 0;
  }

public:
  runtime_limiter() : edge(fetch()) {}

  /// Tag for the testing constructor below (disambiguates it from the
  /// environment-driven one).
  struct for_testing {};

  /// Testing constructor: an explicit absolute deadline (0 disables the
  /// limiter); an edge in the past makes is_timeout() true immediately, with
  /// no sleeping and no environment involved (see ut/api/test-env-selftest.c++).
  runtime_limiter(for_testing, time_t absolute_edge) : edge(absolute_edge) {}

  bool is_timeout() {
    if (edge && time(nullptr) > edge) {
      const auto current = ::testing::UnitTest::GetInstance();
      static const void *last_reported;
      if (last_reported != current) {
        last_reported = current;
        std::cout << "[  SKIPPED ] RUNTIME_LIMIT was reached" << std::endl;
      }
      return true;
    }
    return false;
  }
};

extern runtime_limiter ci_runtime_limiter;

#define MDBX_GTEST_IS_EXECUTION_TIMEOUT() ci_runtime_limiter.is_timeout()

static inline void mdtest_ci_crt_suppress_popup() {
#ifdef _WIN32
  /* Keep the interactive CRT popups for local debugging: suppress them only
   * on CI (env CI is set by the runners) or when explicitly requested via
   * MDBX_TEST_NO_CRT_POPUP. */
  if (!mdtest_getenv("CI") && !mdtest_getenv("MDBX_TEST_NO_CRT_POPUP"))
    return;
#ifdef _MSC_VER
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif /* _MSC_VER */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif /* _WIN32 */
}
