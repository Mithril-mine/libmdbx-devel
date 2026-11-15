/* v13a-transcoder-fuzz.cpp — V13a stream B machine check (P2 arithmetic).
 *
 * Statement-level machine check of the pure functions of src/mdbx.c++:
 *   - to_hex/from_hex/to_base58/from_base58/to_base64/from_base64::write_bytes
 *     destination bounds: a canary after a buffer of EXACTLY
 *     envisage_result_length() bytes must never be touched, for every source
 *     length 0..~200, every wrap width {0,1,3,4,8,17,64} and random content
 *     (including all-zero buffers, which stress the leading-'1' prefix logic);
 *   - roundtrips: hex/base64 roundtrip identity (ignore_spaces on and off);
 *     base58: decode(encode(x)) == x (with the canonical leading-zero form);
 *   - is_erroneous() consistency with write_bytes(): a string deemed valid
 *     must decode without domain_error, an invalid one must throw;
 *   - slice::is_printable() (both modes) against an independent reference
 *     validator implementing the Unicode-6.0 table documented in the source;
 *   - buffer(0) / #137 area: default_buffer(0) and zero-length assign().
 *
 * Build (from the tree root):
 *   g++ -std=gnu++23 -g -O1 -fsanitize=address,leak \
 *     -DMDBX_BUILD_CXX=1 '-DMDBX_CONFIG_H="config-gnumake.h"' -I. -Isrc \
 *     docs/verification/repro/v13a-transcoder-fuzz.cpp \
 *     src/mdbx.c++ /path/to/libmdbx-core.o -o /tmp/v13a-fuzz -pthread -lrt
 */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "mdbx.h++"

static int failures = 0;
static long known_divergences_ascii = 0;
static long known_divergences_empty = 0;
#define CHECK(cond, msg)                                                                                               \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      printf("FAIL  %s\n", msg);                                                                                       \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)

static bool printable_reference(const mdbx::slice &s, bool disable_utf8) {
  /* Reference transcribed independently from the byte map in slice::is_printable().
   * X_ ("printable extended ASCII") bytes: 0x82..0x8C, 0x8E, 0x91..0x9C, 0x9E..0xBF,
   * 0xC0, 0xC1, 0xF5..0xFF.  P_: 0x20..0x7E.  Everything else (including control
   * characters and the UTF-8 lead bytes 0xC2..0xF4) is non-printable in ASCII mode. */
  static const unsigned char x_extended[32] = {/* bytes 0x80..0x9F that the map marks X_ */
                                               0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, /* 80..8F */
                                               0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1  /* 90..9F */};
  const auto len = s.length();
  if (len < 1)
    return false; /* the guard `length() < 1 -> false` in the code */
  const auto *p = s.byte_ptr();
  if (disable_utf8) {
    for (size_t i = 0; i < len; ++i) {
      const unsigned char c = p[i];
      const bool ok = (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xBF) || c == 0xC0 || c == 0xC1 ||
                      (c >= 0xF5) || (c >= 0x80 && c <= 0x9F && x_extended[c - 0x80]);
      if (!ok) {
        if (c >= 0xE0 && c <= 0xEF)
          /* KNOWN DIVERGENCE F-V13A-3 (counted, not a failure): the code accepts
           * 0xE0..0xEF in ASCII mode — the 3-byte-lead class 3<<LS aliases the P_
           * bit in `(P_|X_) & map[b]` */
          ++known_divergences_ascii;
        return false;
      }
    }
    return true;
  }
  /* strict UTF-8 per the Unicode 6.0 table quoted in slice::is_printable */
  size_t i = 0;
  while (i < len) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7F) /* no control character exceptions in the map */
        return false;
      i += 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
      if (i + 1 >= len || p[i + 1] < 0x80 || p[i + 1] > 0xBF)
        return false;
      i += 2;
    } else if (c == 0xE0) {
      if (i + 2 >= len || p[i + 1] < 0xA0 || p[i + 1] > 0xBF || p[i + 2] < 0x80 || p[i + 2] > 0xBF)
        return false;
      i += 3;
    } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
      if (i + 2 >= len || p[i + 1] < 0x80 || p[i + 1] > 0xBF || p[i + 2] < 0x80 || p[i + 2] > 0xBF)
        return false;
      i += 3;
    } else if (c == 0xED) {
      if (i + 2 >= len || p[i + 1] < 0x80 || p[i + 1] > 0x9F || p[i + 2] < 0x80 || p[i + 2] > 0xBF)
        return false;
      i += 3;
    } else if (c == 0xF0) {
      if (i + 3 >= len || p[i + 1] < 0x90 || p[i + 1] > 0xBF || p[i + 2] < 0x80 || p[i + 2] > 0xBF || p[i + 3] < 0x80 ||
          p[i + 3] > 0xBF)
        return false;
      i += 4;
    } else if (c >= 0xF1 && c <= 0xF3) {
      if (i + 3 >= len || p[i + 1] < 0x80 || p[i + 1] > 0xBF || p[i + 2] < 0x80 || p[i + 2] > 0xBF || p[i + 3] < 0x80 ||
          p[i + 3] > 0xBF)
        return false;
      i += 4;
    } else if (c == 0xF4) {
      if (i + 3 >= len || p[i + 1] < 0x80 || p[i + 1] > 0x8F || p[i + 2] < 0x80 || p[i + 2] > 0xBF || p[i + 3] < 0x80 ||
          p[i + 3] > 0xBF)
        return false;
      i += 4;
    } else
      return false;
  }
  return true;
}

/* canary-guarded scratch */
struct scratch {
  std::vector<char> buf;
  size_t size;
  explicit scratch(size_t n) : buf(n + 64), size(n) {
    memset(buf.data() + n, 0xA5, 64); /* canary */
  }
  char *data() { return buf.data(); }
  size_t dest_size() const { return size; }
  bool canary_intact() const {
    for (size_t i = 0; i < 64; ++i)
      if ((unsigned char)buf.data()[size + i] != 0xA5)
        return false;
    return true;
  }
};

int main() {
  std::mt19937_64 rng(0xC0FFEEuLL /* fixed seed, reproducible */);
  (void)rng;

  const unsigned wraps[] = {0, 1, 3, 4, 8, 17, 64};
  std::vector<std::vector<unsigned char>> corpus;
  for (size_t len = 0; len <= 200; ++len) {
    std::vector<unsigned char> v(len, 0); /* all-zero first */
    corpus.push_back(v);
    v.assign(len, 0xFF);
    corpus.push_back(v);
    for (int k = 0; k < 3; ++k) { /* a few random per length */
      std::vector<unsigned char> r(len);
      for (size_t i = 0; i < len; ++i)
        r[i] = (unsigned char)(rng() & 0xFF);
      corpus.push_back(r);
    }
  }

  size_t runs = 0;
  for (const auto &src : corpus) {
    const mdbx::slice s(src.data(), src.size());
    for (unsigned wrap : wraps) {
      /* --- to_hex --- */
      {
        mdbx::to_hex tx(s, ((&src)[0].size() & 1) != 0, wrap);
        scratch sc(tx.envisage_result_length());
        char *const end = tx.write_bytes(sc.data(), sc.dest_size());
        CHECK(sc.canary_intact(), "to_hex canary");
        CHECK(size_t(end - sc.data()) <= sc.dest_size(), "to_hex bounds");
        /* roundtrip: hex decode of the encoding */
        if (!wrap && s.length()) {
          mdbx::from_hex fh(mdbx::slice(sc.data(), size_t(end - sc.data())));
          scratch out(fh.envisage_result_length());
          char *const oend = fh.write_bytes(out.data(), out.dest_size());
          CHECK(sc.canary_intact() && size_t(oend - out.data()) <= out.dest_size(), "from_hex bounds");
          CHECK(mdbx::slice(out.data(), size_t(oend - out.data())) == s, "hex roundtrip");
        }
        ++runs;
      }
      /* --- to_base58 / from_base58 --- */
      {
        mdbx::to_base58 tb(s, wrap);
        scratch sc(tb.envisage_result_length());
        char *const end = tb.write_bytes(sc.data(), sc.dest_size());
        CHECK(sc.canary_intact(), "to_base58 canary");
        CHECK(size_t(end - sc.data()) <= tb.envisage_result_length(), "to_base58 bounds");
        if (!wrap) {
          mdbx::from_base58 fb(mdbx::slice(sc.data(), size_t(end - sc.data())));
          CHECK(!fb.is_erroneous(), "from_base58 of own encoding is valid");
          scratch out(fb.envisage_result_length());
          char *const oend = fb.write_bytes(out.data(), out.dest_size());
          CHECK(sc.canary_intact() && size_t(oend - out.data()) <= fb.envisage_result_length(), "from_base58 bounds");
          CHECK(mdbx::slice(out.data(), size_t(oend - out.data())) == s, "base58 roundtrip");
        }
        ++runs;
      }
      /* --- to_base64 / from_base64 --- */
      {
        mdbx::to_base64 tb(s, wrap);
        scratch sc(tb.envisage_result_length());
        char *const end = tb.write_bytes(sc.data(), sc.dest_size());
        CHECK(sc.canary_intact(), "to_base64 canary");
        CHECK(size_t(end - sc.data()) <= tb.envisage_result_length(), "to_base64 bounds");
        if (!wrap && s.length()) {
          mdbx::from_base64 fb(mdbx::slice(sc.data(), size_t(end - sc.data())), true);
          CHECK(!fb.is_erroneous(), "from_base64 of own encoding is valid");
          scratch out(fb.envisage_result_length());
          char *const oend = fb.write_bytes(out.data(), out.dest_size());
          CHECK(sc.canary_intact() && size_t(oend - out.data()) <= fb.envisage_result_length(), "from_base64 bounds");
          CHECK(mdbx::slice(out.data(), size_t(oend - out.data())) == s, "base64 roundtrip");
        }
        ++runs;
      }
    }
  }
  printf("transcoder runs: %zu\n", runs);

  /* from_hex/from_base58/from_base64 is_erroneous vs write_bytes consistency
   * on adversarial inputs (mixed spaces, padding, invalid chars) */
  {
    std::uniform_int_distribution<int> byte(0, 255);
    for (int k = 0; k < 20000; ++k) {
      size_t len = size_t(rng() % 24);
      std::string str(len, ' ');
      for (auto &c : str)
        c = char(byte(rng));
      mdbx::slice s(str.data(), str.size());
      /* base58 */
      {
        mdbx::from_base58 fb(s, true);
        scratch sc(fb.envisage_result_length());
        try {
          fb.write_bytes(sc.data(), sc.dest_size());
          CHECK(!fb.is_erroneous(), "base58 valid-agreement");
        } catch (const std::domain_error &) {
          CHECK(fb.is_erroneous(), "base58 invalid-agreement");
        }
        CHECK(sc.canary_intact(), "from_base58 adversarial canary");
      }
      /* base64 */
      {
        mdbx::from_base64 fb(s, true);
        scratch sc(fb.envisage_result_length());
        bool no_digits = true;
        for (char c : str)
          if (c > ' ')
            no_digits = false;
        try {
          fb.write_bytes(sc.data(), sc.dest_size());
          if (no_digits) /* same F-V13A-2 divergence as from_hex */
            ++known_divergences_empty;
          else
            CHECK(!fb.is_erroneous(), "base64 valid-agreement");
        } catch (const std::domain_error &) {
          CHECK(fb.is_erroneous(), "base64 invalid-agreement");
        }
        CHECK(sc.canary_intact(), "from_base64 adversarial canary");
      }
      /* hex */
      {
        mdbx::from_hex fh(s, true);
        scratch sc(fh.envisage_result_length());
        bool no_digits = true;
        for (char c : str)
          if (isxdigit((unsigned char)c))
            no_digits = false;
        try {
          fh.write_bytes(sc.data(), sc.dest_size());
          /* KNOWN DIVERGENCE F-V13A-2 (counted, not a failure): a source without
           * a single decodable character decodes successfully (to empty) while
           * is_erroneous() reports true (the `got` flag) */
          if (no_digits)
            ++known_divergences_empty;
          else
            CHECK(!fh.is_erroneous(), "hex valid-agreement");
        } catch (const std::domain_error &) {
          CHECK(fh.is_erroneous(), "hex invalid-agreement");
        }
        CHECK(sc.canary_intact(), "from_hex adversarial canary");
      }
    }
  }

  /* is_printable vs reference */
  {
    std::vector<std::vector<unsigned char>> cases;
    for (int c = 0; c < 256; ++c)
      cases.push_back({(unsigned char)c});
    const unsigned char *pairs2[] = {(const unsigned char *)"\xC2\x80", (const unsigned char *)"\xC2\xBF",
                                     (const unsigned char *)"\xDF\x80", (const unsigned char *)"\xC0\x80",
                                     (const unsigned char *)"\xC1\xBF", (const unsigned char *)"\xC2\x7F"};
    for (auto *p : pairs2)
      cases.push_back(std::vector<unsigned char>(p, p + 2));
    const unsigned char e0a0[] = "\xE0\xA0\x80", e09f[] = "\xE0\x9F\xBF", ed9f[] = "\xED\x9F\xBF",
                        eda0[] = "\xED\xA0\x80", f48f[] = "\xF4\x8F\xBF\xBF", f490[] = "\xF4\x90\x80\x80",
                        f090[] = "\xF0\x90\x80\x80", f08f[] = "\xF0\x8F\xBF\xBF";
    for (auto *p : {e0a0, e09f, ed9f, eda0, f48f, f490, f090, f08f})
      cases.push_back(std::vector<unsigned char>(p, p + strlen((const char *)p)));
    std::uniform_int_distribution<int> byte(0, 255);
    for (int k = 0; k < 40000; ++k) {
      size_t len = size_t(rng() % 10);
      std::vector<unsigned char> v(len);
      for (auto &c : v)
        c = (unsigned char)byte(rng);
      cases.push_back(v);
    }
    for (const auto &v : cases) {
      mdbx::slice s(v.data(), v.size());
      CHECK(s.is_printable(false) == printable_reference(s, false), "is_printable utf8 agreement");
      const long before = known_divergences_ascii;
      const bool code_ascii = s.is_printable(true);
      const bool ref_ascii = printable_reference(s, true);
      if (known_divergences_ascii != before)
        /* divergence inside the skipped 0xE0..0xEF bytes (F-V13A-3); all other
         * bytes of the string must still agree */
        CHECK(code_ascii || !ref_ascii, "is_printable ascii agreement (mod F-V13A-3)");
      else
        CHECK(code_ascii == ref_ascii, "is_printable ascii agreement");
    }
    printf("known divergences (documented findings, not failures): "
           "%ld single-byte E0-EF accepted in ascii mode (F-V13A-3), "
           "%ld no-digit sources decode-to-empty yet report erroneous (F-V13A-2)\n",
           known_divergences_ascii, known_divergences_empty);
    CHECK(mdbx::slice().is_printable() == false, "empty slice is not printable");
  }

  /* buffer(0) / zero-length assign (#137 area) */
  {
    mdbx::default_buffer b0(size_t(0));
    CHECK(b0.empty() && b0.is_freestanding(), "buffer(0) is empty & freestanding");
    mdbx::default_buffer b1;
    b1.assign(nullptr, 0);
    CHECK(b1.empty(), "assign(nullptr, 0) ok");
    const char *p = "abc";
    b1.assign(p, 3);
    CHECK(b1 == mdbx::slice("abc"), "assign copies");
    /* insulate of a reference buffer */
    mdbx::default_buffer ref(mdbx::slice(p, 3), true);
    CHECK(!ref.is_freestanding(), "reference-mode buffer");
    mdbx::default_buffer own(mdbx::slice(p, 3), false);
    CHECK(own.is_freestanding() && own == mdbx::slice("abc"), "owning-ctor insulates (copies)");
    /* self-move */
    mdbx::default_buffer sm(mdbx::slice("xy"), false);
    sm = std::move(sm);
    CHECK(sm == mdbx::slice("xy"), "buffer self-move is a no-op");
  }

  printf(failures ? "FUZZ: %d FAILURES\n" : "FUZZ: all checks passed\n", failures);
  return failures ? 1 : 0;
}
