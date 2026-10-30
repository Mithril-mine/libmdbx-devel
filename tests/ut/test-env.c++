/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// Единственный общий экземпляр runtime_limiter для всех gtest-таргетов
/// (этот файл компилируется в каждый gtest-таргет, см. add_gtest() в
/// cmake/testing.cmake). Здесь же определяется единственный подавитель
/// CRT-попапов, чтобы он выполнялся ровно один раз на тестовый бинарь
/// (а не по одному на TU, если бы объект был объявлен в заголовке).

#include "test-env.h"

runtime_limiter ci_runtime_limiter;

namespace detail {
struct ci_crt_popup_suppressor {
  ci_crt_popup_suppressor() { mdtest_ci_crt_suppress_popup(); }
};
} /* namespace detail */

static const ::detail::ci_crt_popup_suppressor ci_crt_popup_suppressor_instance;

/* Собственный main для всех gtest-таргетов: единственный экземпляр общего
 * кода (test-env.c++ компилируется в каждый gtest-таргет через
 * mdbx_test_env OBJECT). Заменяет gtest_main; в coverage-сборках mingw
 * дополнительно сбрасывает gcov-счётчики явно (см. MDBX_GCOV_FLUSH).
 * extern "C" объявление — на namespace scope: внутри функции недопустимо
 * (mingw gcc: "expected unqualified-id before string constant"). */
#if defined(MDBX_GCOV_FLUSH)
extern "C" void __gcov_dump(void);
#endif
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const int rc = RUN_ALL_TESTS();
#if defined(MDBX_GCOV_FLUSH)
  __gcov_dump();
#endif
  return rc;
}
