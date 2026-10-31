/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
///
/// https://en.wikipedia.org/wiki/Operating_system_abstraction_layer
/// \file osal.c — umbrella OSAL module, includes all osal-*.c parts.

#include "osal-misc.c"
#include "osal-thread.c"
#include "osal-ioring.c"
#include "osal-file.c"
#include "osal-mmap.c"
