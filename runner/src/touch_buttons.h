// touch_buttons.h -- on-screen buttons for what a keyboard or a pad does
// otherwise, so a touchscreen alone (or a mouse) can do it too.
//
// The frontend draws them on every frame, the same on every platform, over
// the top screen (the shown one in the single-screen layout), shown ones
// only. A press on one goes to it and nowhere else (not to the guest): the
// frontend runs the runner's own buttons (the chat), and a button with keys
// holds those DS keys for as long as the press lasts.
//
// The runner registers its buttons, a title its own (from a static
// initializer, like a setting), and either shows or hides them at any time.
//
// Sizes are in DS pixels whatever the title's UI size, so the buttons stay
// big enough for a finger. Like the other overlays, the layout is a list of
// filled rectangles, the same on every presenter.

#pragma once

#include <cstdint>
#include <vector>

// Where a button sits. BottomRight ones make a row from the bottom right
// corner leftwards, in the order they were registered; the others sit
// halfway down the left or right edge.
enum class NdsTouchButtonPlace : uint8_t { BottomRight, LeftMiddle, RightMiddle };

// Each button, and the room between buttons and to the screen's edges.
constexpr int kNdsTouchButtonWidth = 22;
constexpr int kNdsTouchButtonHeight = 18;
constexpr int kNdsTouchButtonMargin = 2;
// Icons: rows of 11 cells ('#' lit), 2 x 2 DS pixels each, over the button.
constexpr int kNdsTouchButtonArtRows = 9;

struct NdsTouchButtonSpec {
    const char* art[kNdsTouchButtonArtRows];
    NdsTouchButtonPlace place;
    // DS keys held while it is pressed, KEYINPUT bits (0 A ... 8 R, 9 L);
    // 0 for none.
    uint16_t keys;
    // Whether it shows, asked by nds_touch_buttons_update; nullptr for a
    // button shown and hidden with nds_touch_button_set_shown.
    bool (*shown)();
};

constexpr int kNdsTouchButtonMax = 8;
// Registers a button, hidden; `spec` must outlive the program. Returns its
// id, or -1 when full.
int nds_touch_button_register(const NdsTouchButtonSpec* spec);
const NdsTouchButtonSpec* nds_touch_button_spec(int id);

// The runner's chat button: opens the netplay chat (chat_overlay.h).
int nds_touch_button_chat();
// The runner's send button, in the chat button's place while a chat line is
// being typed (in the chat panel or the title's own text entry): sends it.
int nds_touch_button_send();

// Whether a button shows. Safe from any thread.
void nds_touch_button_set_shown(int id, bool shown);
bool nds_touch_button_shown(int id);
// For the frontend, once per frame: asks the buttons with a query.
void nds_touch_buttons_update();

struct NdsTouchButtonRect {
    int x = 0, y = 0, w = 0, h = 0;  // DS pixels
    uint32_t argb = 0;
};

// The shown buttons over a `ds_width` x 192 screen, in DS pixels. Appends
// to `out`.
void nds_touch_buttons_layout(int ds_width,
                              std::vector<NdsTouchButtonRect>* out);

// Whether any button shows, wherever it sits.
bool nds_touch_buttons_any();

// DS pixels from the screen's right edge that the shown BottomRight row
// takes, margins included; 0 with none shown. Other overlays keep clear.
int nds_touch_buttons_extent();

// The shown button at a point of a `ds_width` x 192 screen (DS pixels),
// with some room around each for a finger (the nearest one wins); -1 when
// none.
int nds_touch_buttons_hit(int ds_width, float x, float y);
