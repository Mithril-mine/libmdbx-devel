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
