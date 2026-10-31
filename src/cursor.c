/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor.c — umbrella for the cursor module parts:
///   - cursor-setup.c   lifecycle: validation, tracking, touch/spill,
///                      shadowing, couple init, dupsort setup, cloning;
///   - cursor-nav.c     movement: siblings, first/last/next/prev wrappers,
///                      on-first/on-last predicates, long-range scrolling;
///   - cursor-mut.c     mutations: put/append/multiple and deletion;
///   - cursor-seek.c    positioning by key, operations dispatcher and
///                      estimate/distribution machinery;
///   - cursor-internal.h static __always_inline helper-templates shared
///                      by the parts above (sibling/cursor_bring/
///                      cursor_brim/cursor_step).
/// The cursor_check() state-gate lives in cursor-setup.c.

#include "internals.h"
#include "cursor-internal.h"

#include "cursor-setup.c"
#include "cursor-nav.c"
#include "cursor-mut.c"
#include "cursor-seek.c"
