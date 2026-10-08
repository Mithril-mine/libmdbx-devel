/* Фикстура C++ для теста сканера карты исходников. */
#include <cstddef>

#define SCAN_FIXTURE_CXX_MAGIC 7
#define SCAN_FIXTURE_CXX_SQUARE(x) ((x) * (x))

namespace fixture {
class Widget {
public:
    explicit Widget(int base) : base_(base) {}

    int compute(int n) const;

    static int helper(int x) { return x + SCAN_FIXTURE_CXX_MAGIC; }

private:
    int base_;
};

int Widget::compute(int n) const {
    int r = base_;
    for (int i = 0; i < n; i++) {
        if (r > 0) {
            r = helper(i) - r;
        } else {
            r += i;
        }
    }
    return r * SCAN_FIXTURE_CXX_SQUARE(n);
}
}  // namespace fixture

int scan_fixture_cxx_entry(int a, int b) {
    fixture::Widget w(a);
    return w.compute(b);
}