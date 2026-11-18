// touch_buttons.cpp -- see touch_buttons.h.

#include "touch_buttons.h"

#include <atomic>
#include <cstring>

namespace {

constexpr int kCount = static_cast<int>(NdsTouchButton::Count);
constexpr int kScreenHeight = 192;
// The icons: 11 x 9 cells of 2 x 2 DS pixels ('#' lit), over the button.
constexpr int kCell = 2;
constexpr int kArtRows = kNdsTouchButtonHeight / kCell;
static_assert(kNdsTouchButtonWidth == 11 * kCell &&
                  kNdsTouchButtonHeight == 9 * kCell,
              "icons are 11 x 9 cells");
const char* const kArt[kCount][kArtRows] = {
    {
        // Chat: a speech bubble with three dots.
        "...........",
        "..#######..",
        ".#########.",
        ".##.#.#.##.",
        ".#########.",
        "..#######..",
        "...##......",
        "...#.......",
        "...........",
    },
};
constexpr uint32_t kBack = 0xA0000000u;
constexpr uint32_t kLit = 0xFFFFFFFFu;
// Extra DS pixels around a button that still count as a press on it.
constexpr float kSlack = 4.0f;

std::atomic<bool> g_shown[kCount] = {};

// The shown buttons, rightmost first, and how many.
int Shown(NdsTouchButton out[kCount]) {
    int n = 0;
    for (int i = 0; i < kCount; ++i)
        if (g_shown[i].load(std::memory_order_acquire))
            out[n++] = static_cast<NdsTouchButton>(i);
    return n;
}

// Left edge of the `slot`-th button from the right.
int SlotX(int ds_width, int slot) {
    return ds_width - (slot + 1) * (kNdsTouchButtonWidth +
                                    kNdsTouchButtonMargin);
}

constexpr int kTop =
    kScreenHeight - kNdsTouchButtonMargin - kNdsTouchButtonHeight;

}  // namespace

void nds_touch_button_set_shown(NdsTouchButton button, bool shown) {
    const int i = static_cast<int>(button);
    if (i >= 0 && i < kCount) g_shown[i].store(shown, std::memory_order_release);
}

bool nds_touch_button_shown(NdsTouchButton button) {
    const int i = static_cast<int>(button);
    return i >= 0 && i < kCount && g_shown[i].load(std::memory_order_acquire);
}

void nds_touch_buttons_layout(int ds_width,
                              std::vector<NdsTouchButtonRect>* out) {
    NdsTouchButton shown[kCount];
    const int n = Shown(shown);
    for (int slot = 0; slot < n; ++slot) {
        const int x = SlotX(ds_width, slot);
        if (x < 0) break;
        out->push_back({x, kTop, kNdsTouchButtonWidth, kNdsTouchButtonHeight,
                        kBack});
        const char* const* art = kArt[static_cast<int>(shown[slot])];
        for (int r = 0; r < kArtRows; ++r) {
            const char* row = art[r];
            const int len = static_cast<int>(std::strlen(row));
            for (int c = 0; c < len;) {
                if (row[c] != '#') { ++c; continue; }
                int run = 1;
                while (c + run < len && row[c + run] == '#') ++run;
                out->push_back({x + c * kCell, kTop + r * kCell, run * kCell,
                                kCell, kLit});
                c += run;
            }
        }
    }
}

int nds_touch_buttons_extent() {
    NdsTouchButton shown[kCount];
    const int n = Shown(shown);
    return n ? kNdsTouchButtonMargin +
                   n * (kNdsTouchButtonWidth + kNdsTouchButtonMargin)
             : 0;
}

NdsTouchButton nds_touch_buttons_hit(int ds_width, float x, float y) {
    if (y < kTop - kSlack || y >= kScreenHeight || x >= ds_width)
        return NdsTouchButton::Count;
    NdsTouchButton shown[kCount];
    const int n = Shown(shown);
    for (int slot = 0; slot < n; ++slot) {
        const int left = SlotX(ds_width, slot);
        if (left < 0) break;
        // Up to the screen's edge on the right of the first; halfway into
        // the room between two; the slack on the left of the last.
        const float hit_right =
            slot == 0 ? float(ds_width)
                      : float(left + kNdsTouchButtonWidth) +
                            kNdsTouchButtonMargin / 2.0f;
        const float hit_left = slot == n - 1
            ? float(left) - kSlack
            : float(left) - kNdsTouchButtonMargin / 2.0f;
        if (x >= hit_left && x < hit_right) return shown[slot];
    }
    return NdsTouchButton::Count;
}
