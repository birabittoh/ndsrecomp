// netplay_menu.h -- the runtime menu's Netplay section (net/online_lobby.h):
// the Netplay toggle, the lobby status, Discord sign-in, the lobby name,
// the lobby list and Leave, plus toasts when a lobby opens, closes, or a
// player enters or leaves it. Built only with NDS_ONLINE.
//
// recomp-ui's menu has a static item table and no list rows, so the lobby
// list is a fixed set of ACTION rows whose labels this module rewrites
// every frame (the menu reads labels and descriptions each time it draws).
// Its software presentation cannot edit TEXT rows either, so the lobby
// name is edited here: the frontend forwards typed text and Enter / Esc /
// Backspace while nds_netplay_menu_editing() is true.
//
// Main thread only.

#pragma once

#include <string>
#include <vector>

#include "recomp_runtime_ui.h"

// False when the lobby thread is not running (local wireless off, or a
// non-interactive run): the menu then has no Netplay section.
bool nds_netplay_menu_available();
void nds_netplay_menu_append_items(std::vector<RecompRuntimeUiItem>* items);

// The menu callbacks, for items whose key starts with "netplay.".
bool nds_netplay_menu_owns(const RecompRuntimeUiItem* item);
int nds_netplay_menu_get_value(const RecompRuntimeUiItem* item, int* value);
int nds_netplay_menu_set_value(const RecompRuntimeUiItem* item, int value);
int nds_netplay_menu_run_action(RecompRuntimeUi* ui,
                                const RecompRuntimeUiItem* item);
int nds_netplay_menu_is_enabled(const RecompRuntimeUiItem* item);
void nds_netplay_menu_visibility(bool open);

// Once per host frame: rewrites the rows from the lobby status and shows
// or clears the toast.
void nds_netplay_menu_tick(RecompRuntimeUi* ui);

// Lobby name editing.
enum class NdsNetplayEditKey { Commit, Cancel, Erase };
bool nds_netplay_menu_editing();
void nds_netplay_menu_edit_text(const char* utf8);
// True when the edit was committed (the settings should be saved).
bool nds_netplay_menu_edit_key(NdsNetplayEditKey key);

// The values to write to settings.toml's [online] once the player changed
// one of them here; false before that, so command-line flags such as
// --netplay on are never saved by a change elsewhere in the menu.
bool nds_netplay_menu_settings(bool* enabled, std::string* lobby_name);
