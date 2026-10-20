#define debug_log debug_log_sub

#include "../../../src/pnl.c"

MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  (void)level;
  (void)function;
  (void)line;
  (void)fmt;
}

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok;

static bool check_bool(bool v, bool expect, const char *fn, unsigned line) {
  if (unlikely(v != expect)) {
    ++tst_failed;
    fflush(nullptr);
    fprintf(stderr, "got %s, expected %s, at %s:%u\n", v ? "true" : "false", expect ? "true" : "false", fn, line);
    fflush(nullptr);
    return false;
  }
  ++tst_ok;
  return true;
}

#define CHECK_TRUE(v) check_bool(v, true, __func__, __LINE__)
#define CHECK_FALSE(v) check_bool(v, false, __func__, __LINE__)
#define CHECK_EQ(v, exp)                                                                                             \
  do {                                                                                                               \
    const size_t got = (size_t)(v), want = (size_t)(exp);                                                            \
    if (unlikely(got != want)) {                                                                                     \
      ++tst_failed;                                                                                                  \
      fprintf(stderr, "got %zi, expected %zi, at %s:%u\n", got, want, __func__, __LINE__);                           \
    } else                                                                                                           \
      ++tst_ok;                                                                                                      \
  } while (0)

/* Build a bare page-number list (not an allocated pnl: we only need the data
 * part, size word included). Page numbers are stored in descending order, as
 * unconditionally used by the library since MDBX_PNL_ASCENDING = 0; the
 * caller supplies the numbers already in that order. */
static pgno_t make_pnl(pgno_t *arr, const pgno_t *src, size_t n) {
  arr[0] = (pgno_t)n;
  for (size_t i = 1; i <= n; ++i)
    arr[i] = src[i - 1];
  return (pgno_t)n;
}

static void test_intersect(void) {
  pgno_t a[16], b[16];

  /* disjoint ranges */
  make_pnl(a, (pgno_t[]){4, 3}, 2);
  make_pnl(b, (pgno_t[]){12, 11, 6}, 3);
  CHECK_FALSE(pnl_intersect(a, b));
  CHECK_FALSE(pnl_intersect(b, a));

  /* one shared page (the issue #83 case: two records both listing page 3) */
  make_pnl(a, (pgno_t[]){4, 3}, 2);
  make_pnl(b, (pgno_t[]){12, 11, 3}, 3);
  CHECK_TRUE(pnl_intersect(a, b));
  CHECK_TRUE(pnl_intersect(b, a));

  /* exact duplicate lists */
  make_pnl(a, (pgno_t[]){4, 3}, 2);
  make_pnl(b, (pgno_t[]){4, 3}, 2);
  CHECK_TRUE(pnl_intersect(a, b));

  /* empty lists never intersect */
  make_pnl(a, (pgno_t[]){}, 0);
  make_pnl(b, (pgno_t[]){4, 3}, 2);
  CHECK_FALSE(pnl_intersect(a, b));
  CHECK_FALSE(pnl_intersect(b, a));
}

static void test_check_full(void) {
  pgno_t a[16];

  /* strictly descending (expected internal order) passes */
  make_pnl(a, (pgno_t[]){12, 11, 6}, 3);
  CHECK_TRUE(pnl_check_full(a, 1 << 20));

  /* duplicated page inside one record fails the full check */
  make_pnl(a, (pgno_t[]){12, 11, 6, 6}, 4);
  CHECK_FALSE(pnl_check_full(a, 1 << 20));

  /* out-of-order record fails the full check */
  make_pnl(a, (pgno_t[]){11, 12, 6}, 3);
  CHECK_FALSE(pnl_check_full(a, 1 << 20));

  /* the same malformed list passes the lightweight check */
  CHECK_TRUE(pnl_check(a, 1 << 20));
}

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;

  test_intersect();
  test_check_full();

  if (tst_failed == 0) {
    fprintf(stdout, "Ok: %zu checks passed\n", tst_ok);
    return EXIT_SUCCESS;
  }
  fprintf(stderr, "Failed: %zu checks failed, %zu passed\n", tst_failed, tst_ok);
  return EXIT_FAILURE;
}
