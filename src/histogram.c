/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static bool histogram_check(const struct MDBX_chk_histogram *p, size_t adj) {
  size_t quantiry = p->le1_count;
  for (size_t i = 0; i < ARRAY_LENGTH(p->ranges); ++i)
    quantiry += p->ranges[i].count;

  return quantiry == p->count - adj;
}

static void histogram_reduce_move(struct MDBX_chk_histogram *p, size_t point) {
  assert(histogram_check(p, 1));
  // объединяем
  p->ranges[point].end = p->ranges[point + 1].end;
  p->ranges[point].amount += p->ranges[point + 1].amount;
  p->ranges[point].count += p->ranges[point + 1].count;

  // перемещаем хвост
  while (++point < ARRAY_LENGTH(p->ranges) - 1)
    p->ranges[point] = p->ranges[point + 1];

  // обнуляем последний элемент и продолжаем
  p->ranges[ARRAY_LENGTH(p->ranges) - 1].count = 0;
  assert(histogram_check(p, 1));
}

static __hot void histogram_put(const size_t v, struct MDBX_chk_histogram *p,
                                intptr_t (*finder)(struct MDBX_chk_histogram *p, size_t v)) {
  STATIC_ASSERT(ARRAY_LENGTH(p->ranges) > 2);
  p->amount += v;
  p->count += 1;
  if (likely(v < 2)) {
    p->le1_amount += v;
    p->le1_count += 1;
    return;
  }

  const size_t size = ARRAY_LENGTH(p->ranges), last = size - 1;
  for (;;) {
    size_t i = 0;
    while (i < size && p->ranges[i].count && v >= p->ranges[i].begin) {
      if (v < p->ranges[i].end) {
        // значение попадает в существующий интервал
        p->ranges[i].amount += v;
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
      p->ranges[i].begin = v;
      p->ranges[i].end = v + 1;
      p->ranges[i].amount = v;
      p->ranges[i].count = 1;
      assert(histogram_check(p, 0));
      return;
    }

    const intptr_t point = finder(p, v);
    if (point >= 0)
      // ошибка меньше если слить соседние слоты и добавить новый
      histogram_reduce_move(p, point);
    else {
      // ошибка меньше если расширить слот и добавить к нему
      i = -point - 1;
      p->ranges[i].begin = (v < p->ranges[i].begin) ? v : p->ranges[i].begin;
      p->ranges[i].end = (v < p->ranges[i].end) ? p->ranges[i].end : v + 1;
      p->ranges[i].amount += v;
      p->ranges[i].count += 1;
      return;
    }
  }
}

//------------------------------------------------------------------------------

__hot static intptr_t histogram_minimize_error(struct MDBX_chk_histogram *p, size_t v) {
#if MDBX_HISTOGRAM_USING_128BIT
  bin128_t best_reduce = {.l = UINT64_MAX, .h = UINT64_MAX}, best_enhance = best_reduce;
#else
  uint64_t best_reduce = UINT64_MAX, best_enhance = best_reduce;
#endif /* MDBX_HISTOGRAM_USING_128BIT */

  // ищем пару для слияния с минимальной ошибкой
  intptr_t reduce = 0;
  for (size_t i = 0; i < ARRAY_LENGTH(p->ranges) - 1; ++i) {
    const size_t b1 = p->ranges[i].begin, e1 = p->ranges[i].end;
    const size_t b2 = p->ranges[i + 1].begin, e2 = p->ranges[i + 1].end;
    const uint64_t n1 = p->ranges[i].count, n2 = p->ranges[i + 1].count;
    assert(n1 > 0 && b1 > 0 && b1 < e1);
    assert(n2 > 0 && b2 > 0 && b2 < e2);
    assert(e1 <= b2);
    // за ошибку принимаем площадь изменений на гистограмме при слиянии слотов
    // s1 = (l1 = e1 - b1) * n1; s2 = (l2 = e2 - b2) * n2
    // sx = (lx = e2 - b1) * (nx = n1 + n2) == e2*n1 + e2*n2 - b1*n1 - b1*n2
    // err = s1 + s2 - sx == e1*n1 - b1*n1 + e2*n2 - b2*n2 - e2*n1 - e2*n2 + b1*n1 + b1*n
    // err = n1 * (e2 - e1) + n2 * (b2 - b1)
#if MDBX_HISTOGRAM_USING_128BIT
    const bin128_t err = u128_add(mul64x64_128(e2 - e1, n1), mul64x64_128(b2 - b1, n2));
    if (u128_gt(err, best_reduce))
      continue;
#else
    const uint64_t err = (e2 - e1) * n1 + (b2 - b1) * n2;
    if (err > best_reduce)
      continue;
#endif /* MDBX_HISTOGRAM_USING_128BIT */

    reduce = i;
    best_reduce = err;
  }

  // ищем слот для расширения с минимальной ошибкой
  intptr_t enhance = 0;
  for (size_t i = 0; i < ARRAY_LENGTH(p->ranges); ++i) {
    const size_t b = p->ranges[i].begin, e = p->ranges[i].end;
    const uint64_t n = p->ranges[i].count;
    const size_t ve = (v < e) ? e : v + 1;
    assert(n > 0 && b > 0 && b < e);
    if (i > 0 && v < p->ranges[i - 1].end)
      // пропускаем если расширение этого слота приведёт к пересечению с предыдущим
      continue;
    if (i < ARRAY_LENGTH(p->ranges) - 1 && v >= p->ranges[i + 1].begin)
      // пропускаем если расширение этого слота приведёт к пересечению со следующим
      continue;

    // за ошибку принимаем площадь изменений на гистограмме при расширении слота
#if MDBX_HISTOGRAM_USING_128BIT
    const bin128_t err =
        u128_add((v < b) ? mul64x64_128(b - v, n) : u128(v - b), (ve < e) ? u128(e - ve) : mul64x64_128(ve - e, n));
    if (u128_gt(err, best_enhance))
      continue;
#else
    const uint64_t err = ((v < b) ? (b - v) * n : v - b) + ((ve < e) ? e - ve : (ve - e) * n);
    if (err > best_enhance)
      continue;
#endif /* MDBX_HISTOGRAM_USING_128BIT */

    enhance = i;
    best_enhance = err;
  }

#if MDBX_HISTOGRAM_USING_128BIT
  if (u128_gt(best_enhance, best_reduce))
    return reduce;
#else
  if (best_enhance > best_reduce)
    return reduce;
#endif

  return -(enhance + 1);
}

__hot void histogram_acc(const size_t v, struct MDBX_chk_histogram *p) {
  histogram_put(v, p, histogram_minimize_error);
}

//------------------------------------------------------------------------------

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
