/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/// \file details.h — internal declarations of the platform-neutral OSAL
/// functions implemented in details.c and shared by the posix/ and windows/
/// implementations. This header is intended for use inside the OSAL sources
/// only (after "internals.h"); the rest of the project should not include it.
///
/// Every function declared here is defined exactly once in details.c and has
/// no platform-specific branches, so both osal/posix/*.c and osal/windows/*.c
/// rely on it instead of duplicating the code.

#pragma once

#include "../internals.h"

/*----------------------------------------------------------------------------*/
/* file */

/// Advances the file position outside of the data to avoid corruption due to
/// erroneous use of file descriptors in the application code. The exact
/// parking offset is negotiated by binary search over the file size.
MDBX_INTERNAL int osal_fseek_shut(mdbx_filehandle_t fd, uint64_t *safe_parking_lot_offset);

/*----------------------------------------------------------------------------*/
/* mmap */

/// Rejects the requested mapping size when it exceeds the limit for
/// sanitizers/valgrind (with logging); returns MDBX_SUCCESS or MDBX_TOO_LARGE.
MDBX_INTERNAL int check_mmap_limit(const size_t limit);

/*----------------------------------------------------------------------------*/
/* misc: memory, strings, uuid/bootid-hashing helpers */

/// Checks a well-formedness of an UUID-like bin128 value: not all-zero, not
/// all-ones, etc. Used by the bootid logic.
MDBX_INTERNAL bool check_uuid(bin128_t uuid);
/// Aggregates a piece of data into the PRNG state while collecting the boot id.
MDBX_INTERNAL void bootid_collect(bin128_t *p, const void *s, size_t n);
