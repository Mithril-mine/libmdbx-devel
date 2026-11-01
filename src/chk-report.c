/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file chk-report.c — the printing/output machinery of mdbx_env_chk().

#include "internals.h"
__cold void chk_line_end(MDBX_chk_line_t *line) {
  if (likely(line)) {
    MDBX_chk_internal_t *chk = line->ctx->internal;
    ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    if (likely(chk->cb->print_done))
      chk->cb->print_done(line);
  }
}

__cold MDBX_chk_line_t *chk_line_begin(MDBX_chk_scope_t *const scope, enum MDBX_chk_severity severity) {
  MDBX_chk_internal_t *const chk = scope->internal;
  if (severity < MDBX_chk_warning)
    mdbx_env_chk_encount_problem(chk->usr);
  MDBX_chk_line_t *line = nullptr;
  if (likely(chk->cb->print_begin)) {
    line = chk->cb->print_begin(chk->usr, severity);
    if (likely(line)) {
      ASSERT(line->ctx == nullptr || (line->ctx == chk->usr && line->empty));
      ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
      line->ctx = chk->usr;
    }
  }
  return line;
}

__cold MDBX_chk_line_t *chk_line_feed(MDBX_chk_line_t *line) {
  if (likely(line)) {
    MDBX_chk_internal_t *chk = line->ctx->internal;
    enum MDBX_chk_severity severity = line->severity;
    chk_line_end(line);
    line = chk_line_begin(chk->usr->scope, severity);
  }
  return line;
}

__cold MDBX_chk_line_t *chk_flush(MDBX_chk_line_t *line) {
  if (likely(line)) {
    MDBX_chk_internal_t *chk = line->ctx->internal;
    ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    if (likely(chk->cb->print_flush)) {
      chk->cb->print_flush(line);
      ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
      line->out = line->begin;
    }
  }
  return line;
}

__cold size_t chk_print_wanna(MDBX_chk_line_t *line, size_t need) {
  if (likely(line && need)) {
    size_t have = line->end - line->out;
    ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    if (need > have) {
      chk_flush(line);
      have = line->end - line->out;
    }
    return (need < have) ? need : have;
  }
  return 0;
}

__cold MDBX_chk_line_t *chk_puts(MDBX_chk_line_t *line, const char *str) {
  if (likely(line && str && *str)) {
    MDBX_chk_internal_t *chk = line->ctx->internal;
    size_t left = strlen(str);
    ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    if (chk->cb->print_chars) {
      chk->cb->print_chars(line, str, left);
      ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    } else
      do {
        size_t chunk = chk_print_wanna(line, left);
        ASSERT(chunk <= left);
        if (unlikely(!chunk))
          break;
        memcpy(line->out, str, chunk);
        line->out += chunk;
        ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
        str += chunk;
        left -= chunk;
      } while (left);
    line->empty = false;
  }
  return line;
}

__cold MDBX_chk_line_t *chk_print_va(MDBX_chk_line_t *line, const char *fmt, va_list args) {
  if (likely(line)) {
    MDBX_chk_internal_t *chk = line->ctx->internal;
    ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    if (chk->cb->print_format) {
      chk->cb->print_format(line, fmt, args);
      ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
    } else {
      va_list ones;
      va_copy(ones, args);
      const int needed = vsnprintf(nullptr, 0, fmt, ones);
      va_end(ones);
      if (likely(needed > 0)) {
        const size_t have = chk_print_wanna(line, needed);
        if (likely(have > 0)) {
          int written = vsnprintf(line->out, have, fmt, args);
          if (likely(written > 0))
            line->out += written;
          ASSERT(line->begin <= line->end && line->begin <= line->out && line->out <= line->end);
        }
      }
    }
    line->empty = false;
  }
  return line;
}

__cold MDBX_chk_line_t *MDBX_PRINTF_ARGS(2, 3) chk_print(MDBX_chk_line_t *line, const char *fmt, ...) {
  if (likely(line)) {
    // MDBX_chk_internal_t *chk = line->ctx->internal;
    va_list args;
    va_start(args, fmt);
    line = chk_print_va(line, fmt, args);
    va_end(args);
    line->empty = false;
  }
  return line;
}

MDBX_MAYBE_UNUSED __cold void chk_println_va(MDBX_chk_scope_t *const scope, enum MDBX_chk_severity severity,
                                             const char *fmt, va_list args) {
  chk_line_end(chk_print_va(chk_line_begin(scope, severity), fmt, args));
}

MDBX_MAYBE_UNUSED __cold void chk_println(MDBX_chk_scope_t *const scope, enum MDBX_chk_severity severity,
                                          const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  chk_println_va(scope, severity, fmt, args);
  va_end(args);
}

__cold MDBX_chk_line_t *chk_print_size(MDBX_chk_line_t *line, const char *prefix, const uint64_t value,
                                       const char *suffix) {
  static const char sf[] = "KMGTPEZY"; /* LY: Kilo, Mega, Giga, Tera, Peta, Exa, Zetta, Yotta! */
  if (likely(line)) {
    MDBX_chk_internal_t *chk = line->ctx->internal;
    prefix = prefix ? prefix : "";
    suffix = suffix ? suffix : "";
    if (chk->cb->print_size)
      chk->cb->print_size(line, prefix, value, suffix);
    else
      for (unsigned i = 0;; ++i) {
        const unsigned scale = 10 + i * 10;
        const uint64_t rounded = value + (UINT64_C(5) << (scale - 10));
        const uint64_t integer = rounded >> scale;
        const uint64_t fractional = (rounded - (integer << scale)) * 100u >> scale;
        if ((rounded >> scale) <= 1000)
          return chk_print(line, "%s%" PRIu64 " (%u.%02u %ciB)%s", prefix, value, (unsigned)integer,
                           (unsigned)fractional, sf[i], suffix);
      }
    line->empty = false;
  }
  return line;
}

__cold MDBX_chk_line_t *chk_print_ratio(MDBX_chk_line_t *line, size_t numerator, size_t denominator,
                                        unsigned precision) {
  if (line) {
    ratio2digits_buffer_t buffer;
    line = chk_puts(line, ratio2digits(numerator, denominator, &buffer, precision));
  }
  return line;
}

__cold MDBX_chk_line_t *chk_print_percent(MDBX_chk_line_t *line, const char *triplet, size_t value, size_t whole,
                                          const char *unit) {
  if (line) {
    const char *s1 = triplet;
    const char *s2 = s1 + strlen(s1) + 1;
    const char *s3 = s2 + strlen(s2) + 1;
    ratio2digits_buffer_t buffer;
    line = chk_print(line, "%s %" PRIuSIZE "%s%s (%s%%%s)%s", s1, value, unit, &"s"[*unit == 0 || value == 1],
                     ratio2percent(value, whole, &buffer), s2, s3);
  }
  return line;
}

__cold MDBX_chk_line_t *chk_print_pages_percent(MDBX_chk_line_t *line, const char *triplet, size_t pages,
                                                size_t whole) {
  return chk_print_percent(line, triplet, pages, whole, " page");
}

__cold MDBX_chk_line_t *chk_print_bytes_percent(MDBX_chk_line_t *line, const char *triplet, size_t pages,
                                                size_t whole) {
  return chk_print_percent(line, triplet, pages, whole, " byte");
}

__cold MDBX_chk_line_t *chk_print_pages_percent_bb(MDBX_chk_line_t *line, const char *prefix, size_t pages,
                                                   size_t backed, size_t boundary) {
  if (line) {
    ratio2digits_buffer_t buffer_backed, buffer_boundary;
    line =
        chk_print(line, "%s %" PRIuSIZE " page%s (%s%% of backed, %s%% of boundary)", prefix, pages, &"s"[pages == 1],
                  ratio2percent(pages, backed, &buffer_backed), ratio2percent(pages, boundary, &buffer_boundary));
  }
  return line;
}
