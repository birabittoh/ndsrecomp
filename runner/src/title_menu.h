// title_menu.h -- the runtime menu's rows, for a title that shows them in
// its own menus.
//
// The runtime menu (recomp-ui) and its values live on the frontend thread.
// A title hook that draws the same settings with the game's own windows
// runs on the title thread, so the two meet here: the frontend publishes a
// copy of the rows once per host frame, and the hook asks for a change,
// which the frontend applies (and saves) on its next frame through the
// menu's own callbacks. A change therefore shows up in the rows one host
// frame later.
//
// Like a title setting (title_settings.h), this is host state a hook can
// read: a hook that only draws it and sends changes back leaves the guest
// as deterministic as the menu does.
#pragma once

#include <string>
#include <vector>

enum class NdsTitleMenuRowType { Bool, Int, Choice, Action, Text };

struct NdsTitleMenuRow {
    std::string key;          // "display.scaling", "title.<key>", ...
    std::string section;      // "Display", "Audio", "Input", "Game", ...
    std::string label;        // English
    std::string description;  // English
    NdsTitleMenuRowType type = NdsTitleMenuRowType::Action;
    int minimum = 0;
    int maximum = 0;
    int step = 1;
    // Choice rows: the names and the value each one stands for.
    std::vector<std::string> choices;
    std::vector<int> choice_values;
    int value = 0;        // Bool, Int and Choice rows
    bool enabled = true;  // false: shown, but cannot be changed now
};

// For the title thread. The rows in menu order; empty before the frontend
// has published them (and in a headless run).
std::vector<NdsTitleMenuRow> nds_title_menu_rows();
// Ask the frontend to set a row's value or to run an Action row. Unknown
// keys, values out of range and disabled rows are ignored there.
void nds_title_menu_set(const std::string& key, int value);
void nds_title_menu_run(const std::string& key);

// For the frontend.
void nds_title_menu_publish(std::vector<NdsTitleMenuRow> rows);
struct NdsTitleMenuRequest {
    std::string key;
    bool run = false;  // false: set `value`
    int value = 0;
};
// The requests made since the last call, oldest first.
std::vector<NdsTitleMenuRequest> nds_title_menu_take_requests();
