// touch_buttons_test.cpp -- layout and hit test of the on-screen buttons
// (touch_buttons.h). `touch_buttons_test --print` also prints them as text.

#include "touch_buttons.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

bool Test() {
    bool ok = true;
    std::vector<NdsTouchButtonRect> out;
    nds_touch_buttons_layout(256, &out);
    ok = require(out.empty() && nds_touch_buttons_extent() == 0,
                 "hidden at first") && ok;
    ok = require(nds_touch_buttons_hit(256, 245.0f, 180.0f) ==
                     NdsTouchButton::Count, "no hit while hidden") && ok;

    nds_touch_button_set_shown(NdsTouchButton::Chat, true);
    nds_touch_buttons_layout(256, &out);
    ok = require(!out.empty() && out[0].x == 232 && out[0].y == 172 &&
                     out[0].w == 22 && out[0].h == 18,
                 "chat button bottom right") && ok;
    bool inside = true;
    for (const auto& r : out)
        inside &= r.x >= 232 && r.y >= 172 && r.x + r.w <= 254 &&
                  r.y + r.h <= 190;
    ok = require(inside && out.size() > 1, "icon inside the button") && ok;
    ok = require(nds_touch_buttons_extent() == 26, "extent") && ok;

    // Wider screens: still at the right edge.
    out.clear();
    nds_touch_buttons_layout(342, &out);
    ok = require(!out.empty() && out[0].x == 318, "wide screen") && ok;

    auto hit = [](float x, float y) {
        return nds_touch_buttons_hit(256, x, y) == NdsTouchButton::Chat;
    };
    ok = require(hit(240.0f, 180.0f) && hit(255.5f, 191.5f) &&
                     hit(229.0f, 169.0f), "hits, with slack") && ok;
    ok = require(!hit(200.0f, 180.0f) && !hit(240.0f, 150.0f) &&
                     !hit(240.0f, 192.0f) && !hit(256.0f, 180.0f),
                 "misses") && ok;

    nds_touch_button_set_shown(NdsTouchButton::Chat, false);
    out.clear();
    nds_touch_buttons_layout(256, &out);
    ok = require(out.empty() && !hit(240.0f, 180.0f), "hidden again") && ok;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
        nds_touch_button_set_shown(NdsTouchButton::Chat, true);
        std::vector<NdsTouchButtonRect> out;
        nds_touch_buttons_layout(256, &out);
        std::vector<std::string> screen(20, std::string(26, ' '));
        for (const auto& r : out)
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x)
                    screen[size_t(y - 172)][size_t(x - 232)] =
                        (r.argb & 0xFFu) ? '#' : '+';
        for (const std::string& row : screen) std::printf("%s\n", row.c_str());
        return 0;
    }
    if (!Test()) return 1;
    std::printf("touch_buttons_test: ok\n");
    return 0;
}
