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
/// \file Anchor translation unit that owns the shared precompiled header
/// (tests/ut/pch.h++). Every gtest target reuses this PCH via
/// target_precompile_headers(... REUSE_FROM mdbx_test_pch), so the heavy
/// GoogleTest + libmdbx + standard-library headers are parsed once instead of
/// once per test binary. The TU itself is intentionally empty; it exists only
/// to give the OBJECT/STATIC anchor library something to compile the PCH for.

#include "pch.h++"
