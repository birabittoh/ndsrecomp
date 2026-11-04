#include "frontend_screen_route.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}
}  // namespace

int main() {
    NdsMainScreenDebounce d{};
    // Boot fade: blank frames do not prime.
    check(!nds_main_screen_update(d, 1, true), "blank does not prime");
    check(!d.primed, "still unprimed");
    check(!nds_main_screen_update(d, 1, false), "first frame primes");
    check(d.main == 1, "primed to the first routing");

    // A three-frame flip is ignored; the fourth frame switches.
    d = {};
    nds_main_screen_update(d, 0, false);
    check(!nds_main_screen_update(d, 1, false), "flip 1");
    check(!nds_main_screen_update(d, 1, false), "flip 2");
    check(!nds_main_screen_update(d, 1, false), "flip 3");
    check(nds_main_screen_update(d, 1, false) && d.main == 1,
          "switch on the fourth held frame");

    // A blip back resets the count.
    d = {};
    nds_main_screen_update(d, 0, false);
    nds_main_screen_update(d, 1, false);
    nds_main_screen_update(d, 1, false);
    nds_main_screen_update(d, 0, false);
    nds_main_screen_update(d, 1, false);
    nds_main_screen_update(d, 1, false);
    check(!nds_main_screen_update(d, 1, false) && d.main == 0,
          "interrupted run does not switch");

    // Blank frames inside a run neither count nor reset.
    d = {};
    nds_main_screen_update(d, 0, false);
    nds_main_screen_update(d, 1, false);
    nds_main_screen_update(d, 1, false);
    nds_main_screen_update(d, 1, true);
    nds_main_screen_update(d, 0, true);
    nds_main_screen_update(d, 1, false);
    check(nds_main_screen_update(d, 1, false) && d.main == 1,
          "blank frames are transparent to the count");

    check(nds_single_shown_screen(0, false, false) == 0, "main top");
    check(nds_single_shown_screen(1, false, false) == 1, "main bottom");
    check(nds_single_shown_screen(0, true, false) == 1, "toggle from top");
    check(nds_single_shown_screen(1, true, false) == 0, "toggle from bottom");
    check(nds_single_shown_screen(0, false, true) == 1, "peek bottom");
    check(nds_single_shown_screen(0, true, true) == 1, "peek wins over toggle");
    check(nds_single_shown_screen(1, false, true) == 1,
          "no peek change when main is bottom");

    if (failures) return 1;
    std::puts("frontend screen route checks passed");
    return 0;
}
