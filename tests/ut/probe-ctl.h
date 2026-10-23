/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
///
/// Minimal probe-bus control accessor for C++ unit-tests (coverage-pilot,
/// B68-P3). Declares only the exported mdbx_probe_ctl() entry point; the full
/// control protocol (arm/disarm/fault/alloc-fault/query) is textual.
/// Guarded by MDBX_PROBES so non-probe builds stay untouched.

#pragma once

#include <cstddef>

#if defined(MDBX_PROBES)
extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);
#endif /* MDBX_PROBES */