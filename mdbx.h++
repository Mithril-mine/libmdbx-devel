/// \file mdbx.h++
/// \brief The libmdbx C++ API header file.
///
/// \details _libmdbx_ (aka MDBX) is an extremely fast, compact, powerful, embeddable,
/// transactional [key-value store](https://en.wikipedia.org/wiki/Key-value_database),
/// with [Apache 2.0 license](./LICENSE). _MDBX_ has a specific set of properties and capabilities,
/// focused on creating unique lightweight solutions with extraordinary performance.
///
/// Please visit https://libmdbx.dqdkfa.ru for more information, documentation,
/// C++ API description and links to the origin git repo with the source code.
/// Questions, feedback and suggestions are welcome to the Telegram' group
/// https://t.me/libmdbx.
///
/// Donations are welcome to ETH `0xD104d8f8B2dC312aaD74899F83EBf3EEBDC1EA3A`,
/// BTC `bc1qzvl9uegf2ea6cwlytnanrscyv8snwsvrc0xfsu`, SOL `FTCTgbHajoLVZGr8aEFWMzx3NDMyS5wXJgfeMTmJznRi`.
/// Всё будет хорошо!
///
/// The libmdbx project has been completely relocated to the jurisdiction of the Russian Federation.
/// \note _libmdbx_ is still open and provided with first-class free support.
///
/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2020-2026
///

#pragma once

//
// Tested with, since 2026:
//  - Elbrus LCC >= 1.28 (http://www.mcst.ru/lcc);
//  - GNU C++ >= 11.3;
//  - CLANG >= 14.0;
//  - MSVC >= 19.44 (Visual Studio 2022 toolchain v143),
// before 2026:
//  - Elbrus LCC >= 1.23 (http://www.mcst.ru/lcc);
//  - GNU C++ >= 4.8;
//  - CLANG >= 3.9;
//  - MSVC >= 14.0 (Visual Studio 2015),
//    but 19.2x could hang due optimizer bug;
//  - AppleClang, but without C++20 concepts.
//

#include "mdbx++/begin.h++"

#include "mdbx++/decl_exceptions.h++"

//------------------------------------------------------------------------------

/// \defgroup cxx_data slices and buffers
/// @{

#include "mdbx++/decl_slice.h++"

#include "mdbx++/decl_transcoders.h++"

#include "mdbx++/decl_buffer.h++"

/// end of cxx_data @}

#include "mdbx++/decl_core.h++"

#include "mdbx++/decl_env.h++"

#include "mdbx++/decl_txn.h++"

#include "mdbx++/decl_cursor.h++"

//==============================================================================
//
// Inline body of the libmdbx C++ API
//

#include "mdbx++/impl_core.h++"

#include "mdbx++/impl_exceptions.h++"

#include "mdbx++/impl_slice.h++"

#include "mdbx++/impl_buffer.h++"

//------------------------------------------------------------------------------

#include "mdbx++/impl_env.h++"

#include "mdbx++/impl_txn.h++"

#include "mdbx++/impl_cursor.h++"

//------------------------------------------------------------------------------

#include "mdbx++/end.h++"
