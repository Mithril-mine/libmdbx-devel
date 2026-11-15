/* Reproducer for F-V4-1 (bug:tree-cutoff:dump-position-overshoot) — LATENT.
 *
 * The defect lives in cursor_dump_position (src/tree-cutoff.c:8-39), compiled
 * only under `#ifndef NDEBUG`, so it cannot be linked from a release build.
 * The accumulation pattern `tail += snprintf(tail, end - tail, ...)` uses the
 * snprintf RETURN value (the untruncated length). REACHABILITY: every current
 * call site passes print_pageptr = false (the ".%u" shape fits easily), so
 * the overrun is NOT reachable today; it becomes live the moment any caller
 * passes print_pageptr = true (the parameter exists for it) on a deep stack:
 *   - top = CURSOR_STACK_SIZE-1 = 31 on 64-bit (internals.h:313);
 *   - pgno up to 10 digits; %p of a real 64-bit pointer = 16 chars;
 *   - per level ".%p#%u->%u" = 1+16+1+10+2+2 = 32 chars;
 *   - the subcursor second pass (goto at tree-cutoff.c:33-37) doubles output:
 *     2 x 32 x 32 + captions ~= 1090 > 1023. After the first truncated call
 *     `tail` exceeds `end`, `end - tail` becomes negative, the following
 *     size_t limit wraps to ~SIZE_MAX and every later snprintf is a
 *     bounds-violation (glibc _FORTIFY_SOURCE aborts).
 *
 * Build:  cc -O1 -o repro v4-dump-position-overshoot.c && ./repro
 *           => SIGABRT (buggy shape with pageptr=true), exit 134.
 * Fixed:  cc -O1 -DFIXED -o repro v4-dump-position-overshoot.c && ./repro
 *           => exit 0; the proposed fix clamps `tail` to `end` after EVERY
 *              snprintf (clamping only at pass end is too late: the wrapped
 *              size is consumed inside the per-level loop).
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char buf[1024];
static char guard[256]; /* sibling storage to observe a real clobber */

#ifdef FIXED
/* The fix: clamp the cursor back into the buffer after every snprintf. */
static void addf(char **tail, char *end, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(*tail, (size_t)(end - *tail), fmt, ap);
  va_end(ap);
  *tail += len;
  if (*tail > end)
    *tail = end; /* clamp: subsequent size arguments stay >= 0 */
}
#define SNPRINTF(tail, end, ...) addf(&(tail), (end), __VA_ARGS__)
#else
/* The buggy shape: raw `tail += snprintf(tail, end - tail, ...)` exactly as
 * in tree-cutoff.c:15-31. The size_t wrap after a truncated call is the
 * defect under study, so the three call sites below trip CodeQL's
 * overflowing-snprintf query by design: those alerts are dismissed as
 * "used in tests" (a verification-archive harness, not shipping code). */
#define SNPRINTF(tail, end, ...) ((tail) += snprintf((tail), (size_t)((end) - (tail)), __VA_ARGS__))
#endif

/* Faithful transcription of the cursor_dump_position loop shape
 * (caption + per-level ".%p#%u->%u" with print_pageptr, then the
 * subcursor second pass). */
static char *dump_position(int top, unsigned pgno, int print_pageptr, int print_subcursor) {
  const char *caption = "begin";
  const int passes = print_subcursor ? 2 : 1;
  char *tail = buf;
  for (int pass = 0; pass < passes; ++pass) {
    char *const end = buf + sizeof(buf) - 1;
    if (pass == 1)
      caption = "->subcur";
    SNPRINTF(tail, end, "%s[%i]", caption, top);
    for (intptr_t i = 0; i <= top; ++i)
      if (print_pageptr)
        SNPRINTF(tail, end, ".%p#%u->%u", (void *)(uintptr_t)0x00007fffffffffffull, pgno, (unsigned)i);
      else
        SNPRINTF(tail, end, ".%u", (unsigned)i);
  }
  return tail;
}

int main(void) {
  memset(guard, 'G', sizeof(guard));
  /* Worst case within libmdbx's real domain: top = CURSOR_STACK_SIZE-1,
   * 10-digit pgno, pageptr = true, subcursor pass = true. */
  char *const tail = dump_position(31, 4294967295u, 1, 1);
  printf("tail offset past buf start = %td (buf is 1024, end at 1023)\n", tail - buf);
  printf("overshoot: %s\n", (tail > buf + sizeof(buf) - 1) ? "YES (tail beyond end -> size_t limit wraps)" : "no");
  fflush(stdout);
  printf("guard clobbered: %s\n", (guard[0] != 'G') ? "YES" : "no");
  fflush(stdout);
#ifdef FIXED
  return (tail > buf + sizeof(buf) - 1 || guard[0] != 'G') ? 1 : 0;
#else
  /* The buggy shape is expected to be aborted by glibc's fortify before this
   * line (SIGABRT). Reaching this line at all means the overshoot was not
   * triggered on this libc - report failure so the result is unambiguous. */
  return 1;
#endif
}
