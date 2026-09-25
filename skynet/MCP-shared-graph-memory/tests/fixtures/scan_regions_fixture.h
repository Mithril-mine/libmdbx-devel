/* Фикстура условной компиляции для scan_regions.py. */
#ifndef SCAN_REGIONS_GUARD
#define SCAN_REGIONS_GUARD 1

#if IS_WINDOWS
static int win_only(void) { return 1; }
#elif defined(__linux__)
static int linux_only(void) { return 2; }
#else
static int other_os(void) { return 3; }
#endif

#if MDBX_ENABLE_PROFGC
static int profgc_helper(int x) {
#if MDBX_DEBUG
    return x + 1;
#else
    return x * 2;
#endif
}
#endif

static int always_here(void) {
#if MDBX_PNL_ASCENDING
    return 10;
#else
    return 20;
#endif
}

#if defined(__GNUC__)
#if __GNUC__ >= 8
#if defined(__x86_64__)
static int deep_nested_x86(void) { return 1; }
#else
static int deep_nested_other(void) { return 2; }
#endif
#else
static int old_gcc(void) { return 3; }
#endif
#endif

#endif /* SCAN_REGIONS_GUARD */