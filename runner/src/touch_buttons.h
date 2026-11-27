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
// corner leftwards, in the order they were registered; LeftMiddle and
// RightMiddle sit halfway down the left or right edge, TopCenter and
// BottomCenter halfway along the top or bottom edge. TopCenterNext sits on
// the top edge right of the TopCenter button (the fast-forward one), for a
// button that must stay clear of what a title draws at the bottom; the two
// are centred together, as a pair, so neither moves when only one shows.
enum class NdsTouchButtonPlace : uint8_t {
    BottomRight, LeftMiddle, RightMiddle, TopCenter, BottomCenter,
    TopCenterNext
};

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
    // A latch: a tap holds its keys past the press, until the next touch on
    // the touchscreen ends (a pick from a menu those keys bring up) and a
    // few frames more, a second tap on it, or focus loss; drawn as on
    // meanwhile.
    bool latch = false;
    // For a latch: the keys a touchscreen press at (x, y) (DS pixels) adds
    // to its own, instead of the touch, for a pick made with keys (a pad
    // direction); 0, or nullptr, lets the touch through as a touch.
    uint16_t (*touch_keys)(int x, int y) = nullptr;
    // DS keys held while a mouse right click is held on it, instead of its
    // own (never a latch); 0 for a button that ignores right clicks.
    uint16_t right_keys = 0;
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

// The runner's fast-forward button, top center: a press turns fast-forward
// on or off (as holding Tab does while held). Hidden unless a title gives
// it a query (nds_touch_button_set_query).
int nds_touch_button_fast_forward();

// Gives a button (one of the runner's, say) a query that
// nds_touch_buttons_update asks in place of its spec's.
void nds_touch_button_set_query(int id, bool (*shown)());

// Gives a button a query for its opacity, in percent (100 when it has
// none), asked by nds_touch_buttons_layout: a title's setting can fade it.
void nds_touch_button_set_opacity_query(int id, int (*opacity)());

// Whether a button draws as on (lit, dark icon), for a toggle.
void nds_touch_button_set_active(int id, bool active);

// Whether a button shows. Safe from any thread.
void nds_touch_button_set_shown(int id, bool shown);
bool nds_touch_button_shown(int id);
// For the frontend, once per frame: asks the buttons with a query.
void nds_touch_buttons_update();

struct NdsTouchButtonRect {
    int x = 0, y = 0, w = 0, h = 0;  // DS pixels
    uint32_t argb = 0;
};

// What the buttons are laid out against, in the screen's DS pixels (y 0 is
// its top line, 192 the one below its last): the window's top and bottom
// edges, which lie beyond the screen's own where the window has bars above or
// below it (a portrait phone), so the top-center buttons sit at the top of the
// window and the bottom-right ones at its bottom; and the rect the title's
// inset (the map, the other screen) covers, which a bottom button moves up
// above rather than hide behind (inset_w 0: none).
struct NdsTouchButtonFrame {
    int top = 0;
    int bottom = 192;
    int inset_x = 0, inset_y = 0, inset_w = 0, inset_h = 0;
};

// The shown buttons over a `ds_width` x 192 screen, in DS pixels. Appends
// to `out`.
void nds_touch_buttons_layout(int ds_width,
                              std::vector<NdsTouchButtonRect>* out);
// The same against a frame: y goes below 0 or past 192 for a button in the
// bars.
void nds_touch_buttons_layout(int ds_width, const NdsTouchButtonFrame& frame,
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
int nds_touch_buttons_hit(int ds_width, const NdsTouchButtonFrame& frame,
                          float x, float y);
