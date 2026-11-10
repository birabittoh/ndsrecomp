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

// For the frontend.
size_t nds_title_setting_count();
const NdsTitleSetting* nds_title_setting_at(int id);
// -1 when no setting has this key.
int nds_title_setting_find(const char* key);
// False (and no change) when the value is out of range or not a choice.
bool nds_title_setting_set(int id, int value);
