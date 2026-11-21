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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "mdbx.h++"
#include <array>
#include <gtest/gtest.h>

#include "../test-env.h++"

#include <iostream>

#include <functional>
#include <random>

using buffer = mdbx::default_buffer;

std::default_random_engine prng(42);

static buffer random(size_t length) {
  /* the buffer(capacity) constructor creates an empty buffer with the spare
   * room, so the content must be filled and committed explicitly */
  buffer result(length);
  auto ptr = result.end_char_ptr();
  for (size_t i = 0; i < length; ++i)
    *ptr++ = char(prng());
  result.set_end(ptr);
  return result;
}

static bool basic() {
  bool ok = true;
  const char *const hex_dump = "1D58fa\n2e46E3\nBd9c7A\nC0bF";
  const uint8_t native[] = {0x1D, 0x58, 0xfa, 0x2e, 0x46, 0xE3, 0xBd, 0x9c, 0x7A, 0xC0, 0xbF};

  if (mdbx::slice(hex_dump).hex_decode(true) != mdbx::slice::wrap(native))
    std::cerr << "hex_decode() failed\n";
  else if (mdbx::slice::wrap(native).encode_hex(true, 4).hex_decode(true) != mdbx::slice::wrap(native))
    std::cerr << "hex_encode(UPPERCASE) failed\n";
  else if (mdbx::slice::wrap(native).encode_hex(false).hex_decode(true) != mdbx::slice::wrap(native))
    std::cerr << "hex_encode(lowercase) failed\n";

  if (mdbx::slice("").as_base64_string() != "" || mdbx::slice(" ").encode_base64().as_string() != "IA==" ||
      mdbx::slice("~0").encode_base64().as_string() != "fjA=" ||
      mdbx::slice("A_z").encode_base64().as_string() != "QV96" ||
      mdbx::slice("Ka9q").encode_base64().as_string() != "S2E5cQ==" ||
      mdbx::slice("123456789").encode_base64().as_string() != "MTIzNDU2Nzg5") {
    std::cerr << "encode_base64() failed\n";
    ok = false;
  }

  const uint8_t base58_rfc[] = {0x00, 0x00, 0x28, 0x7f, 0xb4, 0xcd};
  if (mdbx::slice("").as_base58_string() != "" || mdbx::slice(" ").encode_base58().as_string() != "Z" ||
      mdbx::slice("Hello World!").as_base58_string() != "2NEpo7TZRRrLZSi2U" ||
      mdbx::slice("The quick brown fox jumps over the lazy dog.").encode_base58().as_string() !=
          "USm3fpXnKG5EUBx2ndxBDMPVciP5hGey2Jh4NDv6gmeo1LkMeiKrLJUUBk6Z" ||
      mdbx::slice::wrap(base58_rfc).as_base58_string() != "11233QC4" ||
      mdbx::slice("~0").encode_base58().as_string() != "Aby" ||
      mdbx::slice("A_z").encode_base58().as_string() != "NxZw" ||
      mdbx::slice("Ka9q").encode_base58().as_string() != "2vkjDi" ||
      mdbx::slice("123456789").encode_base58().as_string() != "dKYWwnRHc7Ck") {
    std::cerr << "encode_base58() failed\n";
    ok = false;
  }

  if (mdbx::slice("Z").base58_decode() != mdbx::slice(" ") ||
      mdbx::slice("2NEpo7TZRRrLZSi2U").base58_decode() != "Hello World!" ||
      mdbx::slice("USm3fpXnKG5EUBx2ndxBDMPVciP5hGey2Jh4NDv6gmeo1LkMeiKrLJUUBk6Z").base58_decode() !=
          mdbx::slice("The quick brown fox jumps over the lazy dog.") ||
      mdbx::slice("11233QC4").base58_decode() != mdbx::slice::wrap(base58_rfc) ||
      mdbx::slice("Aby").base58_decode() != mdbx::slice("~0") ||
      mdbx::slice("NxZw").base58_decode() != mdbx::slice("A_z") ||
      mdbx::slice("2vkjDi").base58_decode() != mdbx::slice("Ka9q") ||
      mdbx::slice("dKYWwnRHc7Ck").base58_decode() != mdbx::slice("123456789")) {
    std::cerr << "decode_base58() failed\n";
    ok = false;
  }

  return ok;
}

template <typename CALLABLE> static bool throws_domain_error(CALLABLE &&call) {
  try {
    call();
  } catch (const std::domain_error &) {
    return true;
  }
  std::cerr << "the expected std::domain_error has not been thrown\n";
  return false;
}

static bool predicates() {
  bool ok = true;

  // An empty (or spaces-only, when spaces are ignored) source is NOT a valid
  // dump: a dump of zero bytes is not a dump.
  if (mdbx::slice("").is_hex() || mdbx::slice("").is_hex(true) || mdbx::slice(" ").is_hex(true) ||
      mdbx::slice("  \t\n").is_hex(true) || mdbx::slice("").is_base64() || mdbx::slice("").is_base64(true) ||
      mdbx::slice(" ").is_base64(true) || mdbx::slice("    ").is_base64(true) || mdbx::slice("").is_base58() ||
      mdbx::slice("").is_base58(true) || mdbx::slice(" ").is_base58(true) || mdbx::slice("   ").is_base58(true)) {
    std::cerr << "the dump predicates must reject an empty or spaces-only source\n";
    ok = false;
  }

  // A valid non-empty dump is still recognized; spaces outside the groups are fine.
  if (!mdbx::slice("4142").is_hex() || !mdbx::slice("41 42").is_hex(true) || !mdbx::slice("IA==").is_base64() ||
      !mdbx::slice("QUJD REVG").is_base64(true) || !mdbx::slice("1").is_base58() ||
      !mdbx::slice("2NEpo7TZRRrLZSi2U").is_base58() || !mdbx::slice(" 1 ").is_base58(true)) {
    std::cerr << "the dump predicates must accept a valid non-empty dump\n";
    ok = false;
  }

  // Decoding an empty (or spaces-only) dump is an error for every entry point.
  ok = throws_domain_error([] { (void)mdbx::slice("").hex_decode(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::slice("  ").hex_decode(true); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::from_hex(mdbx::slice("")).as_buffer(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::from_hex(mdbx::slice(" "), true).as_string(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::slice("").base64_decode(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::slice("    ").base64_decode(true); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::from_base64(mdbx::slice("")).as_string(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::from_base64(mdbx::slice("   "), true).as_string(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::slice("").base58_decode(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::slice("   ").base58_decode(true); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::from_base58(mdbx::slice("")).as_string(); }) && ok;
  ok = throws_domain_error([] { (void)mdbx::from_base58(mdbx::slice("  "), true).as_string(); }) && ok;
  ok = throws_domain_error([&] {
         buffer sink;
         (void)sink.append_decoded_hex(mdbx::slice(""));
       }) &&
       ok;
  ok = throws_domain_error([&] {
         buffer sink;
         (void)sink.append_decoded_base58(mdbx::slice(""));
       }) &&
       ok;
  ok = throws_domain_error([&] {
         buffer sink;
         (void)sink.append_decoded_base64(mdbx::slice(""));
       }) &&
       ok;

  // Writing to a caller-provided buffer must reject the empty source as well.
  char chunk[8];
  ok = throws_domain_error([&] { (void)mdbx::from_hex(mdbx::slice("")).write_bytes(chunk, sizeof(chunk)); }) && ok;
  ok = throws_domain_error([&] { (void)mdbx::from_base58(mdbx::slice("")).write_bytes(chunk, sizeof(chunk)); }) && ok;
  ok = throws_domain_error([&] { (void)mdbx::from_base64(mdbx::slice("")).write_bytes(chunk, sizeof(chunk)); }) && ok;

  // A non-empty dump is still decoded; the encoders still produce an empty
  // output for an empty source, which is just not a decodable dump.
  if (mdbx::slice("4142").hex_decode() != mdbx::slice("AB") ||
      mdbx::slice("IA==").base64_decode() != mdbx::slice(" ") || mdbx::slice("Z").base58_decode() != mdbx::slice(" ") ||
      mdbx::slice("").encode_hex().as_string() != "" || mdbx::slice("").encode_base58().as_string() != "" ||
      mdbx::slice("").encode_base64().as_string() != "") {
    std::cerr << "the transcoders of non-empty sources are broken\n";
    ok = false;
  }

  return ok;
}

/// The exact ASCII-mode (disable_utf8 == true) acceptance, pinned here by the
/// exhaustive 0..255 sweep: printable ASCII-7, the Latin-1 extended printable
/// codes (A0..BF), the CP1252-styled printable positions inside the C1 controls,
/// plus C0/C1 and F5..FF which are never UTF8 lead bytes. The UTF8 lead bytes
/// C2..DF, E0..EF and F0..F4, as well as the other controls, must be rejected.
static bool ascii_printable_expected(unsigned c) {
  if (c >= 0x20 && c <= 0x7E)
    return true; // printable ASCII-7
  if (c >= 0xA0 && c <= 0xBF)
    return true; // Latin-1 extended printable
  if (c >= 0xF5)
    return true; // above the last UTF8 lead byte, printable by design
  if (c == 0xC0 || c == 0xC1)
    return true; // Latin-1 letters, never valid UTF8 lead bytes
  return (c >= 0x82 && c <= 0x8C) || c == 0x8E || (c >= 0x91 && c <= 0x9C) || c == 0x9E || c == 0x9F;
}

static bool printable() {
  bool ok = true;

  // the exhaustive 0..255 sweep of the single bytes in the extended-ASCII mode
  for (unsigned c = 0; c < 256; ++c) {
    const char single[1] = {static_cast<char>(c)};
    const mdbx::slice slice(single, 1);
    if (slice.is_printable(true) != ascii_printable_expected(c)) {
      std::cerr << "is_printable(true) mismatch for the byte 0x" << std::hex << c << std::dec << "\n";
      ok = false;
    }
  }

  // multi-byte sequences in the extended-ASCII mode: any UTF8 lead byte rejects
  if (!mdbx::slice("Hello, World!").is_printable(true) || !mdbx::slice("\xA0\xC0\xC1\xF5\xFF").is_printable(true) ||
      mdbx::slice("\xC2\x80").is_printable(true) || mdbx::slice("\xDF\xBF").is_printable(true) ||
      mdbx::slice("\xE0\xA0\x80").is_printable(true) || mdbx::slice("\xE2\x82\xAC").is_printable(true) ||
      mdbx::slice("\xED\x9F\xBF").is_printable(true) || mdbx::slice("\xF0\x9F\x98\x80").is_printable(true) ||
      mdbx::slice("\xF4\x8F\xBF\xBF").is_printable(true) || mdbx::slice("\x80").is_printable(true) ||
      mdbx::slice("\x01").is_printable(true) || mdbx::slice("\x7F").is_printable(true)) {
    std::cerr << "is_printable(true) must reject the UTF8 lead bytes and controls\n";
    ok = false;
  }

  // the UTF8 mode: an empty content and the broken sequences are rejected
  if (mdbx::slice("").is_printable() || !mdbx::slice("Hello, World!").is_printable() ||
      !mdbx::slice("\xD0\x9F").is_printable() || !mdbx::slice("\xE0\xA0\x80").is_printable() ||
      !mdbx::slice("\xE2\x82\xAC").is_printable() || !mdbx::slice("\xED\x9F\xBF").is_printable() ||
      !mdbx::slice("\xF0\x9F\x98\x80").is_printable() || !mdbx::slice("\xF4\x8F\xBF\xBF").is_printable() ||
      mdbx::slice("\xC0\x80").is_printable() || mdbx::slice("\xE0\x80").is_printable() ||
      mdbx::slice("\xED\xA0\x80").is_printable() || mdbx::slice("\xF4\x90\x80\x80").is_printable() ||
      mdbx::slice("\xF5\x80\x80\x80").is_printable() || mdbx::slice("\xA0").is_printable() ||
      mdbx::slice("\x80").is_printable() || mdbx::slice("\xC2").is_printable()) {
    std::cerr << "is_printable() failed\n";
    ok = false;
  }

  return ok;
}

int doit() {
  auto ok = basic() && predicates() && printable();
  for (size_t n = 0; n < 1000; ++n) {
    /* the length 0 is not roundtrippable here: an empty dump is not decodable */
    for (size_t length = 1; ok && length < 111; ++length) {
      const auto pattern = random(length);
      if (pattern !=
          pattern.encode_hex(bool(prng() & 1), prng() % 111).hex_decode(true).encode_hex().hex_decode(false)) {
        std::cerr << "hex encode/decode failed: n " << n << ", length " << length << std::endl;
        ok = false;
      }
      if (pattern !=
          pattern.encode_base64(unsigned(prng() % 111)).base64_decode(true).encode_base64().base64_decode(false)) {
        std::cerr << "base64 encode/decode failed: n " << n << ", length " << length << std::endl;
        ok = false;
      }
      if (pattern !=
          pattern.encode_base58(unsigned(prng() % 111)).base58_decode(true).encode_base58().base58_decode(false)) {
        std::cerr << "base58 encode/decode failed: n " << n << ", length " << length << std::endl;
        ok = false;
      }
    }
  }

  if (!ok) {
    std::cerr << "Fail\n";
    return EXIT_FAILURE;
  }
  std::cout << "OK\n";
  return EXIT_SUCCESS;
}

TEST(ut_hex_base64_base58, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  try {
    ASSERT_EQ(EXIT_SUCCESS, doit());
  } catch (const std::exception &ex) {
    std::cerr << "Exception: " << ex.what() << "\n";
    FAIL();
  }
}
