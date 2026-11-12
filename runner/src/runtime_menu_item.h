// runtime_menu_item.h -- one runtime setting as the frontend lists it.
//
// The frontend keeps every setting it serves (display, audio, input, the
// title's own settings, Netplay) as one list of these; title hooks read it
// through title_menu.h and draw it in the game's own menus.

#pragma once

#include <cstddef>

enum class NdsMenuItemType { Bool, Int, Choice, Action };

struct NdsMenuItem {
    const char* key;
    const char* section;
    const char* label;
    const char* description;
    NdsMenuItemType type;
    int minimum;
    int maximum;
    int step;
    const char* const* choices;
    size_t choice_count;
    // Optional values for sparse choices; null means minimum + index.
    const int* choice_values;
};
