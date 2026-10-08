/* Фикстура для теста сканера карты исходников. */
#include <stddef.h>

#define SCAN_FIXTURE_MAGIC 42
#define SCAN_FIXTURE_DOUBLE(x) ((x) * 2)

static int helper_impl(int x) {
    return x * SCAN_FIXTURE_MAGIC;
}

int scan_fixture_entry(int a, int b) {
    int r = 0;
    for (int i = 0; i < a; i++) {
        if (i == b) {
            r += helper_impl(i);
        } else {
            r -= i;
        }
    }
    while (r < 0) {
        r += 1;
    }
    return r;
}

struct fixture_record {
    int value;
};
