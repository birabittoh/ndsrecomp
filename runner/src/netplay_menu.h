// netplay_menu.h -- the runtime settings' Netplay rows (net/online_lobby.h):
// the Netplay toggle and the Discord sign-in. Title hooks show them in the
// game's own menus through title_menu.h; hosting, joining and leaving follow
// the game's own multiplayer menus. Built only with NDS_ONLINE.
//
// Main thread only.

#pragma once

#include <string>
#include <vector>

#include "runtime_menu_item.h"

// False when the lobby thread is not running (local wireless off, or a
// non-interactive run): there are no Netplay rows then.
bool nds_netplay_menu_available();
void nds_netplay_menu_append_items(std::vector<NdsMenuItem>* items);

// The settings callbacks, for items whose key starts with "netplay.".
bool nds_netplay_menu_owns(const NdsMenuItem* item);
int nds_netplay_menu_get_value(const NdsMenuItem* item, int* value);
int nds_netplay_menu_set_value(const NdsMenuItem* item, int value);
int nds_netplay_menu_run_action(const NdsMenuItem* item);
int nds_netplay_menu_is_enabled(const NdsMenuItem* item);

// Once per host frame: rewrites the sign-in row's label from the lobby
// status.
void nds_netplay_menu_tick();

// The values to write to settings.toml's [online] once the player changed
// one of them here; false before that, so command-line flags such as
// --netplay on are never saved by a change elsewhere in the settings.
bool nds_netplay_menu_settings(bool* enabled, std::string* lobby_name);
