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

const NdsTouchButtonSpec kLeft = {
    {"...........", "....#......", "...##......", "..#######..",
     ".########..", "..#######..", "...##......", "....#......",
     "..........."},
    NdsTouchButtonPlace::LeftMiddle,
    1u << 9,
    nullptr,
};
const NdsTouchButtonSpec kRight = {
    {"...........", "......#....", "......##...", "..#######..",
     "..########.", "..#######..", "......##...", "......#....",
     "..........."},
    NdsTouchButtonPlace::RightMiddle,
    1u << 8,
    nullptr,
};
bool g_extra_shown = false;
const NdsTouchButtonSpec kExtra = {
    {"...........", "...........", "...........", "...#####...",
     "...#####...", "...#####...", "...........", "...........",
     "..........."},
    NdsTouchButtonPlace::BottomRight,
    1u << 0,
    [] { return g_extra_shown; },
};

bool Inside(const std::vector<NdsTouchButtonRect>& rects, size_t first,
            size_t last, int x, int y) {
    for (size_t i = first; i < last; ++i) {
        const auto& r = rects[i];
        if (r.x < x || r.y < y || r.x + r.w > x + 22 || r.y + r.h > y + 18)
            return false;
    }
    return true;
}

bool Test() {
    bool ok = true;
    const int chat = nds_touch_button_chat();
    const int left = nds_touch_button_register(&kLeft);
    const int right = nds_touch_button_register(&kRight);
    const int extra = nds_touch_button_register(&kExtra);
    ok = require(chat == 0 && left == 1 && right == 2 && extra == 3 &&
                     nds_touch_button_chat() == chat &&
                     nds_touch_button_spec(right) == &kRight,
                 "registered") && ok;

    std::vector<NdsTouchButtonRect> out;
    nds_touch_buttons_layout(256, &out);
    ok = require(out.empty() && nds_touch_buttons_extent() == 0 &&
                     nds_touch_buttons_hit(256, 245.0f, 180.0f) == -1,
                 "hidden at first") && ok;

    nds_touch_button_set_shown(chat, true);
    nds_touch_buttons_layout(256, &out);
    ok = require(!out.empty() && out[0].x == 232 && out[0].y == 172 &&
                     out[0].w == 22 && out[0].h == 18 &&
                     Inside(out, 0, out.size(), 232, 172) && out.size() > 1,
                 "chat button bottom right") && ok;
    ok = require(nds_touch_buttons_extent() == 26, "extent") && ok;
    out.clear();
    nds_touch_buttons_layout(342, &out);
    ok = require(!out.empty() && out[0].x == 318, "wide screen") && ok;

    ok = require(nds_touch_buttons_hit(256, 240.0f, 180.0f) == chat &&
                     nds_touch_buttons_hit(256, 255.5f, 191.5f) == chat &&
                     nds_touch_buttons_hit(256, 229.0f, 169.0f) == chat,
                 "hits, with slack") && ok;
    ok = require(nds_touch_buttons_hit(256, 200.0f, 180.0f) == -1 &&
                     nds_touch_buttons_hit(256, 240.0f, 150.0f) == -1 &&
                     nds_touch_buttons_hit(256, 240.0f, 192.0f) == -1 &&
                     nds_touch_buttons_hit(256, 256.0f, 180.0f) == -1,
                 "misses") && ok;

    // The sides, and a second button in the row, left of the first.
    nds_touch_button_set_shown(chat, false);
    nds_touch_button_set_shown(left, true);
    ok = require(nds_touch_buttons_any() && nds_touch_buttons_extent() == 0,
                 "a side button alone counts as shown") && ok;
    nds_touch_button_set_shown(chat, true);
    nds_touch_button_set_shown(right, true);
    nds_touch_buttons_update();
    ok = require(!nds_touch_button_shown(extra) &&
                     nds_touch_button_shown(chat), "query: hidden") && ok;
    g_extra_shown = true;
    nds_touch_buttons_update();
    ok = require(nds_touch_button_shown(extra), "query: shown") && ok;
    ok = require(nds_touch_buttons_extent() == 50, "row of two") && ok;
    ok = require(nds_touch_buttons_hit(256, 10.0f, 96.0f) == left &&
                     nds_touch_buttons_hit(256, 245.0f, 96.0f) == right &&
                     nds_touch_buttons_hit(256, 215.0f, 180.0f) == extra &&
                     nds_touch_buttons_hit(256, 233.0f, 180.0f) == chat &&
                     nds_touch_buttons_hit(256, 128.0f, 96.0f) == -1,
                 "sides and row") && ok;
    out.clear();
    nds_touch_buttons_layout(256, &out);
    bool sides = false;
    for (const auto& r : out)
        sides |= r.x == 2 && r.y == 87 && r.w == 22 && r.h == 18;
    ok = require(sides, "left middle") && ok;

    for (int id : {chat, left, right})
        nds_touch_button_set_shown(id, false);
    g_extra_shown = false;
    nds_touch_buttons_update();
    out.clear();
    nds_touch_buttons_layout(256, &out);
    ok = require(out.empty() && nds_touch_buttons_hit(256, 240.0f, 180.0f) ==
                                    -1, "hidden again") && ok;
    return ok;
}

void Print() {
    std::vector<NdsTouchButtonRect> out;
    nds_touch_buttons_layout(256, &out);
    std::vector<std::string> screen(192, std::string(256, ' '));
    for (const auto& r : out)
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                screen[size_t(y)][size_t(x)] = (r.argb & 0xFFu) ? '#' : '+';
    for (const std::string& row : screen)
        if (row.find_first_not_of(' ') != std::string::npos)
            std::printf("%s\n", row.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
        nds_touch_button_set_shown(nds_touch_button_chat(), true);
        nds_touch_button_set_shown(nds_touch_button_register(&kLeft), true);
        nds_touch_button_set_shown(nds_touch_button_register(&kRight), true);
        Print();
        return 0;
    }
    if (!Test()) return 1;
    std::printf("touch_buttons_test: ok\n");
    return 0;
}
