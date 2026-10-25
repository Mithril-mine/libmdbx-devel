/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// Common test-environment header for the gtest-based unit tests.
///
/// Provides:
///  - class runtime_limiter: a CI execution-time guard (pattern adopted from
///    libfpta's GTEST_IS_EXECUTION_TIMEOUT). The deadline is read at
///    construction from two environment variables: GTEST_RUNTIME_LIMIT (a
///    relative budget in seconds from the start) and GTEST_RUNTIME_DEADLINE
///    (an absolute deadline in UTC seconds since the epoch, so a whole
///    parallel run shares one moment in time); when both are set the earliest
///    wins. The clock is std::chrono::steady_clock, so changing the system
///    time does not affect the limiter. When the deadline is exceeded the
///    test reports "[  SKIPPED ] RUNTIME_LIMIT was reached" once and the
///    caller is expected to GTEST_SKIP() itself. It is intended to be used
///    together with GTEST_SHUFFLE=1 so that a part of the tests runs in
///    random order until the limit is hit, instead of the whole run being
///    killed by the CI timeout.
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

#include <chrono>
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
 * заданный через переменные среды окружения:
 *  - GTEST_RUNTIME_LIMIT — относительный лимит в секундах от момента старта;
 *  - GTEST_RUNTIME_DEADLINE — абсолютный дедлайн в UTC-секундах (epoch),
 *    позволяет ограничить суммарное время набора тестов, запускаемых
 *    параллельно и в случайном порядке, единым моментом времени.
 * Если заданы обе переменные, действует более ранний дедлайн. Отсчет ведется
 * от std::chrono::steady_clock, поэтому перевод системного времени не влияет. */
class runtime_limiter {
  const std::chrono::steady_clock::time_point edge;

  /* Deadline math shared by the default constructor and the self-test:
   * `limit_seconds` is a relative budget (<=0 means unset), `utc_deadline`
   * an absolute wall-clock deadline in UTC seconds (<=0 means unset).
   * The earliest of the two wins; both unset give a zero (disabled) edge. */
  static std::chrono::steady_clock::time_point compute(const long limit_seconds, const long utc_deadline) {
    const auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point result;
    if (limit_seconds > 0)
      result = now + std::chrono::seconds(limit_seconds);
    if (utc_deadline > 0) {
      const long delta = utc_deadline - static_cast<long>(std::time(nullptr));
      const auto absolute = delta > 0 ? now + std::chrono::seconds(delta) : now - std::chrono::seconds(1);
      if (result == std::chrono::steady_clock::time_point() || absolute < result)
        result = absolute;
    }
    return result;
  }

  static std::chrono::steady_clock::time_point fetch() {
    long limit_seconds = 0;
    const char *const limit = mdtest_getenv("GTEST_RUNTIME_LIMIT");
    if (limit)
      limit_seconds = std::atol(limit);
    long utc_deadline = 0;
    const char *const deadline = mdtest_getenv("GTEST_RUNTIME_DEADLINE");
    if (deadline)
      utc_deadline = std::atol(deadline);
    return compute(limit_seconds, utc_deadline);
  }

public:
  runtime_limiter() : edge(fetch()) {}

  /// Tag for the testing constructor below (disambiguates it from the
  /// environment-driven one).
  struct for_testing {};

  /// Testing constructor: an explicit steady-clock offset from now (zero
  /// disables the limiter, a negative one makes is_timeout() true
  /// immediately), with no sleeping and no environment involved (see
  /// ut/api/test-env-selftest.c++).
  runtime_limiter(for_testing, const std::chrono::seconds offset)
      : edge(offset.count() == 0 ? std::chrono::steady_clock::time_point()
                                 : std::chrono::steady_clock::now() + offset) {}

  /// The same deadline math the environment-driven constructor performs,
  /// exposed for the self-test without touching the environment.
  static std::chrono::steady_clock::time_point compute_edge(const long limit_seconds, const long utc_deadline) {
    return compute(limit_seconds, utc_deadline);
  }

  bool is_timeout() {
    if (edge != std::chrono::steady_clock::time_point() && std::chrono::steady_clock::now() > edge) {
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
