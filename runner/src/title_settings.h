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

// A rectangle of the touchscreen (DS pixels, x0..x1 and y0..y1 exclusive)
// that a title draws at its native place while it scales the rest of its UI
// down (nds_gpu2d_set_fixed_size_layers): a touch there is not mapped back
// through the scale, and only where the scaled UI is not. An empty rectangle
// (the default) is none. The title thread sets it, the frontend reads it.
void nds_title_set_unscaled_touch_rect(int x0, int y0, int x1, int y1);
bool nds_title_unscaled_touch_rect(int* x0, int* y0, int* x1, int* y1);

// For the frontend.
size_t nds_title_setting_count();
const NdsTitleSetting* nds_title_setting_at(int id);
// -1 when no setting has this key.
int nds_title_setting_find(const char* key);
// False (and no change) when the value is out of range or not a choice.
bool nds_title_setting_set(int id, int value);
