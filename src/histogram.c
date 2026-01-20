/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static bool histogram_check(const struct MDBX_chk_histogram *p, size_t adj) {
  size_t quantiry = p->le1_count;
  for (size_t i = 0; i < ARRAY_LENGTH(p->ranges); ++i)
    quantiry += p->ranges[i].count;

  return quantiry == p->count - adj;
}

static size_t div_8s(size_t numerator, size_t divider) {
  assert(numerator <= (SIZE_MAX >> 8));
  return (numerator << 8) / divider;
}

static size_t mul_8s(size_t quotient, size_t multiplier) {
  size_t hi = multiplier * (quotient >> 8);
  size_t lo = multiplier * (quotient & 255) + 128;
  return hi + (lo >> 8);
}

static void histogram_reduce(struct MDBX_chk_histogram *p) {
  assert(histogram_check(p, 1));
  const size_t size = ARRAY_LENGTH(p->ranges), last = size - 1;
  // ищем пару для слияния с минимальной ошибкой
  size_t min_err = SIZE_MAX, min_i = last - 1;
  for (size_t i = 0; i < last; ++i) {
    const size_t b1 = p->ranges[i].begin, e1 = p->ranges[i].end, s1 = p->ranges[i].amount;
    const size_t b2 = p->ranges[i + 1].begin, e2 = p->ranges[i + 1].end, s2 = p->ranges[i + 1].amount;
    const size_t l1 = e1 - b1, l2 = e2 - b2, lx = e2 - b1, sx = s1 + s2;
    assert(s1 > 0 && b1 > 0 && b1 < e1);
    assert(s2 > 0 && b2 > 0 && b2 < e2);
    assert(e1 <= b2);
    // за ошибку принимаем площадь изменений на гистограмме при слиянии
    const size_t h1 = div_8s(s1, l1), h2 = div_8s(s2, l2), hx = div_8s(sx, lx);
    const size_t d1 = mul_8s((h1 > hx) ? h1 - hx : hx - h1, l1);
    const size_t d2 = mul_8s((h2 > hx) ? h2 - hx : hx - h2, l2);
    const size_t dx = mul_8s(hx, b2 - e1);
    const size_t err = d1 + d2 + dx;
    if (min_err >= err) {
      min_i = i;
      min_err = err;
    }
  }
  // объединяем
  p->ranges[min_i].end = p->ranges[min_i + 1].end;
  p->ranges[min_i].amount += p->ranges[min_i + 1].amount;
  p->ranges[min_i].count += p->ranges[min_i + 1].count;
  if (min_i < last)
    // перемещаем хвост
    memmove(p->ranges + min_i + 1, p->ranges + min_i + 2, (last - min_i) * sizeof(p->ranges[0]));
  // обнуляем последний элемент и продолжаем
  p->ranges[last].count = 0;
  assert(histogram_check(p, 1));
}

void histogram_acc(const size_t n, struct MDBX_chk_histogram *p) {
  STATIC_ASSERT(ARRAY_LENGTH(p->ranges) > 2);
  p->amount += n;
  p->count += 1;
  if (likely(n < 2)) {
    p->le1_amount += n;
    p->le1_count += 1;
    return;
  }

  const size_t size = ARRAY_LENGTH(p->ranges), last = size - 1;
  for (;;) {
    size_t i = 0;
    while (i < size && p->ranges[i].count && n >= p->ranges[i].begin) {
      if (n < p->ranges[i].end) {
        // значение попадает в существующий интервал
        p->ranges[i].amount += n;
        p->ranges[i].count += 1;
        return;
      }
      ++i;
    }
    if (p->ranges[last].count == 0) {
      // использованы еще не все слоты, добавляем интервал
      assert(i < size && histogram_check(p, 1));
      if (p->ranges[i].count && i < last) {
        // раздвигаем
        memmove(p->ranges + i + 1, p->ranges + i, (last - i) * sizeof(p->ranges[0]));
      }
      p->ranges[i].begin = n;
      p->ranges[i].end = n + 1;
      p->ranges[i].amount = n;
      p->ranges[i].count = 1;
      assert(histogram_check(p, 0));
      return;
    }
    histogram_reduce(p);
  }
}

__cold MDBX_chk_line_t *histogram_dist(MDBX_chk_line_t *line, const struct MDBX_chk_histogram *histogram,
                                       const char *prefix, const char *first, bool amount) {
  /* https://en.wikipedia.org/wiki/Multiplication_sign */
#if defined(unix) || defined(linux) || defined(__unix__) || defined(__unix) || defined(__linux__) ||                   \
    defined(__APPLE__) || defined(__MACH__) || defined(_DARWIN_C_SOURCE)
#define UNICODE_MULSIGN_STR "×"
#define UNICODE_MULSIGN_FMT "s"
#elif defined(_WIN32) || defined(_WIN64)
#define UNICODE_MULSIGN_STR L"\u00d7"
#define UNICODE_MULSIGN_FMT "ls"
#else
#define UNICODE_MULSIGN_STR "*"
#define UNICODE_MULSIGN_FMT "s"
#endif
  line = chk_print(line, "%s:", prefix);
  const char *comma = "";
  const size_t first_val = amount ? histogram->le1_amount : histogram->le1_count;
  if (first_val) {
    chk_print(line, " %s%" UNICODE_MULSIGN_FMT "%" PRIuSIZE, first, UNICODE_MULSIGN_STR, first_val);
    comma = ",";
  }
  for (size_t n = 0; n < ARRAY_LENGTH(histogram->ranges); ++n)
    if (histogram->ranges[n].count) {
      chk_print(line, "%s %" PRIuSIZE, comma, histogram->ranges[n].begin);
      if (histogram->ranges[n].begin != histogram->ranges[n].end - 1)
        chk_print(line, "-%" PRIuSIZE, histogram->ranges[n].end - 1);
      line = chk_print(line, "%" UNICODE_MULSIGN_FMT "%" PRIuSIZE, UNICODE_MULSIGN_STR,
                       amount ? histogram->ranges[n].amount : histogram->ranges[n].count);
      comma = ",";
    }

  ENSURE_MSG(nullptr, histogram_check(histogram, 0), "Historgam related bug, please report this");
  return line;
}

__cold MDBX_chk_line_t *histogram_print(MDBX_chk_scope_t *scope, MDBX_chk_line_t *line,
                                        const struct MDBX_chk_histogram *histogram, const char *prefix,
                                        const char *first, bool amount) {
  if (histogram->count) {
    line = chk_print(line, "%s %" PRIuSIZE, prefix, amount ? histogram->amount : histogram->count);
    if (scope->verbosity > MDBX_chk_info)
      line = chk_puts(histogram_dist(line, histogram, " (distribution", first, amount), ")");
  }
  return line;
}
