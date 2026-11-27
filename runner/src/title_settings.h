// title_settings.h -- integer settings a title adds to the runtime menu.
//
// A title hook source registers each setting from a static initializer and
// reads its value from the hook handler:
//
//     const int kZoom = nds_title_setting_register(&kZoomSetting);
//     NDS_TITLE_HOOK void Dq9Example(uint32_t* sp) {
//         const int zoom = nds_title_setting_value(kZoom);
//         ...
//     }
//
// The menu shows them in a "Game" section. The value comes from [title] in
// the game config, then [title] in the player's settings.toml, and a change
// in the menu is written back to settings.toml's [title].
//
// A setting is host state that changes guest behaviour, which hook handlers
// otherwise must not have (title_hooks.h): keep its default byte-identical to
// the unhooked game, and expect savestates and replays to show the setting
// they are played back with.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct NdsTitleSetting {
    const char* key;          // [title] key; the menu row is "title.<key>"
    const char* label;
    const char* description;
    int minimum;
    int maximum;
    int step;
    // Optional: a choice row instead of a number. choice_values maps each
    // choice to its value; nullptr means minimum + index.
    const char* const* choices;
    size_t choice_count;
    const int* choice_values;
    int default_value;
    // Kept out of the runtime menu (still saved and loaded): a value the
    // title changes some other way, such as a zoom bound to the mouse wheel.
    bool hidden = false;
};

// Returns the setting's id. `setting` must outlive the program. Call only
// from static initializers, before main.
int nds_title_setting_register(const NdsTitleSetting* setting);
// Any thread.
int nds_title_setting_value(int id);

// How tall the frontend shows the main DS screen, in window points (logical
// pixels, so a HiDPI display counts like a standard one), for settings that
// follow the window size. 0 when no window shows it (headless). Host state:
// a hook that reads it makes the guest depend on the window, like a setting.
// Any thread.
int nds_title_display_height();
// For the frontend: once per presented frame.
void nds_title_set_display_height(int points);

// Where the mouse points on the touchscreen, in DS pixels (0..255, 0..191),
// while no mouse button is held: what a touchscreen cannot report and a
// title's menus can use to highlight the entry under the pointer. False when
// the pointer is elsewhere, a button is held (the touch itself reports that
// position) or the mouse is captured. Host state, like the display height.
// Any thread.
bool nds_title_pointer_hover(int* x, int* y);
// For the frontend: on mouse motion and button changes.
void nds_title_set_pointer_hover(int x, int y, bool hovering);

// Whether the right mouse button is held (over any presentation window). Host
// state, like the pointer. Any thread.
bool nds_title_right_mouse_held();
// For the frontend: on right button changes.
void nds_title_set_right_mouse_held(bool held);

// The gamepad's left stick (-32767..32767 per axis, y pointing down) while it
// drives the D-pad: the frontend maps it to the four D-pad bits, and a title
// whose movement is not bound to eight directions can read the stick itself.
// False with no gamepad. Host state,
// like the pointer. Any thread.
bool nds_title_left_stick(int* x, int* y);
// For the frontend: once per polled frame.
void nds_title_set_left_stick(int x, int y, bool active);

// Where the mouse points on the top screen, in DS pixels (0..255, 0..191),
// buttons or not: the top screen is no touchscreen, so a title can read the
// pointer here to make what it shows there hoverable and clickable. False
// when the pointer is elsewhere or the mouse is captured. Host state, like
// the display height. Any thread.
bool nds_title_top_pointer(int* x, int* y);
// A counter of left clicks on the top screen (it increases by one per press,
// wrapping), with the position of the latest one. A title compares it with
// the value it saw last to find new clicks, which a press and release
// between two frames cannot hide. Any thread.
unsigned nds_title_top_click(int* x, int* y);
// For the frontend: on mouse motion and button changes.
void nds_title_set_top_pointer(int x, int y, bool over);
void nds_title_note_top_click(int x, int y);
// While the left button is held after a press on the top screen (a drag):
// true, with the pointer in top-screen DS pixels, not clamped (the drag goes
// on off the screen, where it can be negative or past 255 / 191). False
// otherwise. Any thread.
bool nds_title_top_drag(int* x, int* y);
// For the frontend: on the left press on the top screen, motion while held,
// and the release.
void nds_title_set_top_drag(int x, int y, bool active);

// A rectangle of the touchscreen (DS pixels, x0..x1 and y0..y1 exclusive)
// that a title draws at its native place while it scales the rest of its UI
// down (nds_gpu2d_set_fixed_size_layers): a touch there is not mapped back
// through the scale, and only where the scaled UI is not. An empty rectangle
// (the default) is none. The title thread sets it, the frontend reads it.
void nds_title_set_unscaled_touch_rect(int x0, int y0, int x1, int y1);
bool nds_title_unscaled_touch_rect(int* x0, int* y0, int* x1, int* y1);

// While set, touches (and hover) on the touchscreen are never mapped back
// through the fixed-size layers' scale: the title shows nothing scaled that
// takes touches, and the screen itself does (DQ9's field walks towards the
// stylus anywhere on the screen). Off by default. The title thread sets it,
// the frontend reads it.
void nds_title_set_touch_unscaled(bool unscaled);
bool nds_title_touch_unscaled();

// While set, the touchscreen is a floating stick (DQ9's field walk: held
// anywhere, it walks the way the stylus is dragged from the press). With the
// touchscreen widened past its native 256 x 192 (a wide or tall main view),
// a press anywhere on the view is taken: carried into the native screen,
// away from its edges, and the drag follows by the same offset, held at the
// screen's edge rather than released. Off by default. The title thread sets
// it, the frontend reads it.
void nds_title_set_touch_stick(bool stick);
bool nds_title_touch_stick();
// While set (and no stick), a press on the widened touchscreen view beyond
// the native 256 x 192 is a tap at the nearest point of that screen's edge,
// and the pen follows it there while held: a title's "tap outside the window
// to close it" works over the whole view. Off by default. The title thread
// sets it, the frontend reads it.
void nds_title_set_touch_outside(bool outside);
bool nds_title_touch_outside();
// The stick held on a widened view, where the pointer really is (native DS
// pixels, outside 0..255 x 0..191 beside the DS screen): the press and the
// pen now. The guest's touch differs: the press carried inside, the drag
// from it in the same direction, shortened to stay on the screen. A title
// draws its stick from these to show it under the pointer. False when the
// last press was not such a stick. The frontend sets it, any thread reads it.
void nds_title_set_touch_stick_points(bool active, int press_x, int press_y,
                                      int pen_x, int pen_y);
bool nds_title_touch_stick_points(int* press_x, int* press_y, int* pen_x,
                                  int* pen_y);

// Runs the emulated ARM9 at `multiple` (1, 2 or 4) times its hardware clock
// from the next scheduler round on, for a title whose own frame pacing is
// bound by ARM9 time (a setting that lifts the game's frame-rate cap, say).
// Devices, timers, the 3D engine and the ARM7 stay on the hardware timeline.
// 1, the default, is the hardware. Guest-visible timing, like a setting:
// keep the default untouched. Any thread.
void nds_title_set_arm9_overclock(int multiple);

// Makes gamecard transfers `factor` times as fast as the hardware's (1, the
// default, is the hardware), for a title whose frame loop reads the card
// and waits for it. Guest-visible timing, like nds_title_set_arm9_overclock.
// Any thread.
void nds_title_set_card_speedup(int factor);

// Lets the scheduler run the ARM9 in longer stretches while the ARM7 sleeps
// (io.h nds_request_event_windows): less host time per frame for a title
// that has raised its own load. Any thread.
void nds_title_set_event_windows(bool enabled);

// For a title hook at the head of a loop that polls GXSTAT for a box,
// position or vector test result (G3X_GetBoxTestResult and kin), ARM9 only.
// When the test is still in flight it ends the slice there and the scheduler
// moves the ARM9's clock along with the geometry engine's, as during a
// GXFIFO stall, until the result is ready or an ARM9 IRQ is pending; the
// guest's next poll then sees it. Saves the host cost of thousands of polls,
// not guest time: the CPU still waits as long as the engine takes.
void nds_title_wait_gx_test();

// For a title hook placed where the guest sound driver waits for its next
// periodic tick (in the NITRO SDK's SndThread, the OS_ReceiveMessage call).
// True when the host audio device is short of sound because the emulation
// runs below real time: `tick_system_cycles` of SPU output (one driver tick,
// 2728 * 64 for the SDK's 5.2 ms interval) has just been mixed, and the hook
// should skip the wait and run the tick at once. False at full speed, in
// fast-forward and without audio. See spu.h nds_spu_catchup. ARM7 only.
bool nds_title_audio_catchup(uint32_t tick_system_cycles);

// The language the title shows the game in, as a firmware language code
// (0 Japanese .. 5 Spanish), for what the frontend shows of its own, such as
// the window title (the game's name from the ROM banner in that language).
// -1, the default, follows the firmware language option. Host state; any
// thread.
void nds_title_set_language(int code);
int nds_title_language();

// Registers a query the frontend calls once per presented frame: true while
// the game is in a mode that wants both screens stacked vertically at the
// same size (a title screen, say), whatever the screen_layout option says.
// It applies to the single-screen layout only, where it shows the top LCD
// above the touchscreen; the other layouts already show both. The query
// runs on the frontend thread and may only read guest memory. nullptr
// removes it. Call it from a static initializer, like a setting.
typedef bool (*NdsTitleStackedLayoutQuery)();
void nds_title_set_stacked_layout_query(NdsTitleStackedLayoutQuery query);
// For the frontend: the query's answer (false with none registered).
bool nds_title_stacked_layout_wanted();
// A gap between the two stacked screens, in LCD pixels, for the modes that
// want one: asked each frame while the stacked layout is on. Same rules as
// the stacked query; 0 with none registered.
typedef int (*NdsTitleStackedGapQuery)();
void nds_title_set_stacked_gap_query(NdsTitleStackedGapQuery query);
int nds_title_stacked_gap();

// display.second_screen_size (10..100, the inset beside the main screen as
// a percent of it), for a title that sizes its own inset over the main
// screen by it. The frontend sets it as the option changes, any thread reads
// it.
void nds_title_set_second_screen_size(int percent);
int nds_title_second_screen_size();

// A game mode that wants the second screen below the main one in a portrait
// window (the default puts it above), at the usual second_screen_size.
typedef bool (*NdsTitleInsetBelowQuery)();
void nds_title_set_inset_below_query(NdsTitleInsetBelowQuery query);
bool nds_title_inset_below_wanted();

// Single-screen layout: a title can draw the second screen over the main one,
// see-through, in the main screen's bottom-left corner, instead of beside
// it. The query is handed the second screen's pixels (`screen`: 0 the top
// LCD, 1 the bottom one; `width` x 192 ARGB, the alpha byte not meaningful)
// and writes them to `out`, the same size, with the alpha each pixel is drawn
// at. It returns the inset's size as a percent of the main screen, or 0 for
// the usual inset beside it (`out` is then unused). Frontend thread, once per
// presented frame; it may only read settings or guest memory.
//
// It may also cut the second screen up instead: each piece draws `src` (in
// the second screen's pixels) at `dst` (in the main screen's), scaled to fit,
// and only the pieces are drawn. A point on a piece is a touch at the matching
// point of its `src`. The query writes up to kNdsTitleMaxInsetPieces of them
// to `pieces` and their number to `*piece_count` (0, as passed in, for none).
struct NdsTitleInsetPiece {
    float src_x, src_y, src_w, src_h;
    float dst_x, dst_y, dst_w, dst_h;
};
enum { kNdsTitleMaxInsetPieces = 16 };
typedef int (*NdsTitleInsetOverlayQuery)(int screen, const uint32_t* pixels,
                                         int width, uint32_t* out,
                                         NdsTitleInsetPiece* pieces,
                                         int* piece_count);
void nds_title_set_inset_overlay_query(NdsTitleInsetOverlayQuery query);
// For the frontend: the query's answer, 0 or 10..100 (0 with none
// registered; 100 with pieces), and the pieces it cut (*piece_count, 0 for
// none).
int nds_title_inset_overlay(int screen, const uint32_t* pixels, int width,
                            uint32_t* out, NdsTitleInsetPiece* pieces,
                            int* piece_count);

// Where the see-through second screen sits in the main screen: 0 bottom-left
// (the default and with no query), 1 bottom-right, 2 top-left, 3 top-right.
// Asked each presented frame, before the layout, on the frontend thread; same
// rules as the overlay query. It does not apply to a cut-up inset (the pieces
// have their own dst).
typedef int (*NdsTitleInsetCornerQuery)();
void nds_title_set_inset_corner_query(NdsTitleInsetCornerQuery query);
int nds_title_inset_corner();

// Widescreen owned by the title, in the single-screen layout: while the query
// returns true the main screen takes the window's shape. Wider than 4:3 it
// gains columns (Hor+, up to 448x192, or any width up to
// kNdsGpu3dMaxViewWidth with the HD renderer: the guest's projection keeps
// its vertical field of view); taller, with the HD renderer, it gains lines
// (Ver+, up to kNdsGpu3dMaxViewHeight lines, keeping the horizontal one). The extra columns or
// lines show 3D only; the 2D keeps its native size and proportions in the
// centred 256x192, and the second screen stays native. With a query registered the title needs no ROM-hash
// capability entry. The title is responsible for its own frustum culling at
// the wider aspect (nds_title_wide_width gives the width in use). Frontend
// thread, once per presented frame; it may only read settings or guest
// memory, and should not flip from one frame to the next (each change
// resizes the 3D). Call it from a static initializer, like a setting.
typedef bool (*NdsTitleWideQuery)();
void nds_title_set_wide_query(NdsTitleWideQuery query);
// For the frontend: whether a query is registered, and its answer.
bool nds_title_wide_owned();
bool nds_title_wide_wanted();
// The main screen's view width in LCD pixels, 256 to kNdsGpu3dMaxViewWidth,
// of the frame being drawn (256 while not wide). Inset pieces are in a main
// screen this wide. Any thread.
int nds_title_wide_width();
// Its height, 192 to kNdsGpu3dMaxViewHeight (192 while not taller than 4:3). Inset pieces
// (nds_title_set_inset_overlay_query) are in a main screen this tall.
int nds_title_wide_height();

// Hands 3D motion smoothing (display.smooth_3d) to the title, for a title
// whose own setting pairs it with its frame rate: while a query is
// registered the frontend hides its Smooth 3D row, ignores the option and
// smooths exactly while the query returns true. The query runs on the
// frontend thread once per presented frame and may only read settings or
// guest memory. Call it from a static initializer, like a setting.
typedef bool (*NdsTitleSmooth3dQuery)();
void nds_title_set_smooth_3d_query(NdsTitleSmooth3dQuery query);
// For the frontend: whether a query is registered, and its answer.
bool nds_title_smooth_3d_owned();
bool nds_title_smooth_3d_wanted();

// Binds the frontend's zoom inputs (mouse wheel, gamepad right stick up/down,
// two-finger pinch on a touchscreen)
// to a number setting: an add changes `value_id`, but only while the
// setting `mode_id` holds `active_mode_value` (a title's "Manual" mode). Call
// from a static initializer, like a setting. Unbound by default.
void nds_title_set_zoom_binding(int value_id, int mode_id,
                                int active_mode_value);
// For the frontend: adds `delta` (in the value's own units, may be negative)
// to the bound value, clamped to its range. True when the value changed (the
// frontend then saves the settings).
bool nds_title_zoom_add(int delta);
// For the frontend: the bound value, or -1 when unbound or not in the active
// mode (then nds_title_zoom_add changes nothing).
int nds_title_zoom_value();

// Binds the gamepad's right stick to keys a title turns its camera with:
// while `query` answers true, stick X past a dead zone holds `left_keys` or
// `right_keys` and pressing the stick holds both, on top of whatever else
// presses them (the pad's own buttons included). The query
// runs on the frontend thread once per polled frame and may only read
// settings or what the title sampled of the guest (host state). Call from a
// static initializer, like a setting. Unbound by default.
typedef bool (*NdsTitleCameraStickQuery)();
void nds_title_set_camera_stick(NdsTitleCameraStickQuery query,
                                uint16_t left_keys, uint16_t right_keys);
// For the frontend: true while the query answers true, with the keys.
bool nds_title_camera_stick(uint16_t* left_keys, uint16_t* right_keys);

// For the frontend.
size_t nds_title_setting_count();
const NdsTitleSetting* nds_title_setting_at(int id);
// -1 when no setting has this key.
int nds_title_setting_find(const char* key);
// False (and no change) when the value is out of range or not a choice.
bool nds_title_setting_set(int id, int value);
