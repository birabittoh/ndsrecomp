// touch_buttons.h -- on-screen buttons for what a keyboard or a pad does
// otherwise, so a touchscreen alone (or a mouse) can do it too.
//
// The frontend draws them on every frame, the same on every platform, over
// the top screen (the shown one in the single-screen layout): a row at the
// bottom right, right to left in NdsTouchButton order, shown ones only. A
// press on one goes to it and nowhere else (not to the guest).
//
// Sizes are in DS pixels whatever the title's UI size, so the buttons stay
// big enough for a finger. Like the other overlays, the layout is a list of
// filled rectangles, the same on every presenter.

#pragma once

#include <cstdint>
#include <vector>

enum class NdsTouchButton : uint8_t {
    Chat,  // opens the netplay chat (chat_overlay.h)
    Count,
};

struct NdsTouchButtonRect {
    int x = 0, y = 0, w = 0, h = 0;  // DS pixels
    uint32_t argb = 0;
};

// Each button, and the room between buttons and to the screen's edges.
constexpr int kNdsTouchButtonWidth = 22;
constexpr int kNdsTouchButtonHeight = 18;
constexpr int kNdsTouchButtonMargin = 2;

// Whether a button shows; all are hidden at first. Safe from any thread.
void nds_touch_button_set_shown(NdsTouchButton button, bool shown);
bool nds_touch_button_shown(NdsTouchButton button);

// The shown buttons over a `ds_width` x 192 screen, in DS pixels. Appends
// to `out`.
void nds_touch_buttons_layout(int ds_width,
                              std::vector<NdsTouchButtonRect>* out);

// DS pixels from the screen's right edge that the shown buttons take,
// margins included; 0 with none shown. Other overlays keep clear of them.
int nds_touch_buttons_extent();

// The shown button at a point of a `ds_width` x 192 screen (DS pixels),
// with some room around each for a finger; Count when none.
NdsTouchButton nds_touch_buttons_hit(int ds_width, float x, float y);
