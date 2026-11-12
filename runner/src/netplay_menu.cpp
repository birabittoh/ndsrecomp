// netplay_menu.cpp -- see netplay_menu.h.

#include "netplay_menu.h"

#include "net/online_lobby.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kSection = "Netplay";
constexpr const char* kLobbiesPage = "Netplay/Lobbies";
constexpr const char* kEnabledKey = "netplay.enabled";
constexpr const char* kStatusKey = "netplay.status";
constexpr const char* kAccountKey = "netplay.account";
constexpr const char* kNameKey = "netplay.lobby_name";
constexpr const char* kLeaveKey = "netplay.leave";
constexpr const char* kLobbyKeyPrefix = "netplay.lobby.";
constexpr size_t kLobbyRows = 8;
// The software menu draws 6 px per character in a 244 px panel: about 35
// characters fit beside the value column.
constexpr size_t kNameColumn = 24;
constexpr size_t kMaxLobbyName = 32;
constexpr auto kToastTime = std::chrono::seconds(4);

using Text = std::array<char, 96>;

struct LobbyRow {
    std::string key;
    Text label{};
    Text description{};
    NdsOnlineLobbyRow lobby;
    bool joinable = false;
};

struct MenuState {
    Text status_label{};
    Text status_description{};
    Text account_label{};
    Text name_label{};
    Text name_description{};
    std::array<LobbyRow, kLobbyRows> rows;
    NdsOnlineStatus status;
    std::string lobby_name;  // the setting; empty = "<host>'s lobby"
    bool editing = false;
    std::string edit;
    bool dirty = false;
    bool menu_open = false;
    bool keys_ready = false;
    // Toasts.
    NdsOnlineStatus last;
    bool have_last = false;
    Clock::time_point toast_until{};
    bool toast_shown = false;
};

MenuState& state() {
    static MenuState s;
    return s;
}

void set(Text& out, const std::string& text) {
    std::snprintf(out.data(), out.size(), "%s", text.c_str());
}

bool is(const RecompRuntimeUiItem* item, const char* key) {
    return item && item->key && std::strcmp(item->key, key) == 0;
}

// The lobby row index of `item`, or -1.
int lobby_row(const RecompRuntimeUiItem* item) {
    if (!item || !item->key) return -1;
    const size_t n = std::strlen(kLobbyKeyPrefix);
    if (std::strncmp(item->key, kLobbyKeyPrefix, n) != 0) return -1;
    const int row = std::atoi(item->key + n);
    return row >= 0 && static_cast<size_t>(row) < kLobbyRows ? row : -1;
}

bool online_state(NdsOnlineState s) {
    return s == NdsOnlineState::Idle || s == NdsOnlineState::Hosting ||
           s == NdsOnlineState::Joining || s == NdsOnlineState::Guest;
}

std::string count_text(int players, int max_slots) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d/%d", players, max_slots);
    return buf;
}

// A short status for the row; the lobby thread's own text is longer than
// the panel and goes to the log.
std::string short_status(const NdsOnlineStatus& s) {
    const bool in_room = !s.lobby_id.empty();
    switch (s.state) {
    case NdsOnlineState::Off: return "Off (local network)";
    case NdsOnlineState::NeedSignIn: return "Sign in to play online";
    case NdsOnlineState::SigningIn: return "Signing in";
    case NdsOnlineState::Connecting: return "Connecting";
    case NdsOnlineState::Idle: return "Online, not in a lobby";
    case NdsOnlineState::Hosting:
        return in_room ? "Hosting " + count_text(s.players, s.max_slots)
                       : "Opening a lobby";
    case NdsOnlineState::Joining: return "Joining";
    case NdsOnlineState::Guest:
        return s.linked == 0 ? "Reaching the host"
                             : "In a lobby " + count_text(s.players, s.max_slots);
    case NdsOnlineState::Error: return "Problem, see below";
    }
    return "";
}

std::string status_hint(const NdsOnlineStatus& s) {
    switch (s.state) {
    case NdsOnlineState::Off: return "Turn Netplay on to play online.";
    case NdsOnlineState::Idle:
        if (s.text.rfind("Online: ", 0) == 0 &&
            s.text.find("host in the game") == std::string::npos)
            return s.text.substr(8);
        return "Host in the game, or pick a lobby.";
    case NdsOnlineState::Hosting:
        return s.lobby_id.empty() ? "" : "\"" + s.lobby_name + "\" is listed.";
    case NdsOnlineState::Guest:
        return s.linked == 0 ? "Connecting through the relay."
                             : "Join the host in the game's menu.";
    default: return s.text;
    }
}

std::string fit(const std::string& text, size_t width) {
    if (text.size() <= width) return text;
    return text.substr(0, width - 1) + "~";
}

void refresh_rows(MenuState& m) {
    const NdsOnlineStatus& s = m.status;
    set(m.status_label, "Status: " + short_status(s));
    set(m.status_description, status_hint(s));

    if (!s.account.empty())
        set(m.account_label, "Sign out (" + fit(s.account, 18) + ")");
    else if (s.state == NdsOnlineState::SigningIn)
        set(m.account_label, "Signing in: see the browser");
    else
        set(m.account_label, "Sign in with Discord");

    if (m.editing) {
        std::string shown = m.edit + "_";
        if (shown.size() > 26) shown = shown.substr(shown.size() - 26);
        set(m.name_label, "Name: " + shown);
        set(m.name_description, "Type. Enter saves, Esc cancels.");
    } else {
        set(m.name_label, "Lobby name: " +
                              (m.lobby_name.empty() ? std::string("Default")
                                                    : fit(m.lobby_name, 22)));
        set(m.name_description, "Default: \"<your name>'s lobby\".");
    }

    std::vector<NdsOnlineLobbyRow> lobbies;
    const bool listing = online_state(s.state);
    if (listing) lobbies = nds_online_lobbies();
    for (size_t i = 0; i < kLobbyRows; ++i) {
        LobbyRow& row = m.rows[i];
        row.joinable = false;
        row.lobby = {};
        row.label[0] = 0;
        row.description[0] = 0;
        if (i >= lobbies.size()) {
            if (i == 0) {
                if (s.state == NdsOnlineState::Off)
                    set(row.label, "Netplay is off");
                else if (!listing)
                    set(row.label, "Not connected");
                else
                    set(row.label, "No open lobbies");
            }
            continue;
        }
        row.lobby = lobbies[i];
        const bool own = row.lobby.lobby_id == s.lobby_id;
        const bool full = row.lobby.players >= row.lobby.max_slots;
        std::string label = fit(row.lobby.name, kNameColumn);
        label.resize(kNameColumn + 1, ' ');
        label += count_text(row.lobby.players, row.lobby.max_slots);
        if (!row.lobby.country.empty())
            label += " " + row.lobby.country.substr(0, 2);
        set(row.label, label);
        if (own) {
            set(row.description, s.state == NdsOnlineState::Hosting
                                     ? "Your lobby."
                                     : "You are in this lobby.");
        } else if (full) {
            set(row.description, "Full.");
        } else if (s.state == NdsOnlineState::Hosting) {
            set(row.description, "Close your own session first.");
        } else {
            set(row.description, "Puts you in radio range of the host.");
            row.joinable = true;
        }
    }
}

// Toasts for what changes while the player is in the game.
void toast_changes(MenuState& m, RecompRuntimeUi* ui) {
    const NdsOnlineStatus& now = m.status;
    if (!m.have_last) {
        m.last = now;
        m.have_last = true;
        return;
    }
    const NdsOnlineStatus& was = m.last;
    std::string title, body;
    const bool hosting = now.state == NdsOnlineState::Hosting;
    if (was.lobby_id != now.lobby_id) {
        if (!was.lobby_id.empty() && now.lobby_id.empty()) {
            title = was.state == NdsOnlineState::Hosting ? "Lobby closed"
                                                         : "Left the lobby";
            body = was.lobby_name;
        } else if (!now.lobby_id.empty()) {
            title = hosting ? "Lobby open" : "Joined a lobby";
            body = now.lobby_name + " (" +
                   count_text(now.players, now.max_slots) + ")";
            if (!hosting) body += "\nConnecting to the host";
        }
    } else if (!now.lobby_id.empty() && now.players != was.players) {
        title = now.players > was.players ? "A player entered the lobby"
                                          : "A player left the lobby";
        body = now.lobby_name + " (" +
               count_text(now.players, now.max_slots) + ")";
    } else if (!now.lobby_id.empty() && !hosting &&
               (was.linked == 0) != (now.linked == 0)) {
        if (now.linked > 0) {
            title = "Connected to the host";
            body = "Join from the game's multiplayer menu";
        } else {
            title = "Lost the host";
            body = "Reconnecting through the relay";
        }
    } else if (now.state == NdsOnlineState::NeedSignIn &&
               was.state != NdsOnlineState::NeedSignIn) {
        title = "Netplay";
        body = "Sign in with Discord in the Netplay menu";
    }
    m.last = now;
    if (!title.empty()) {
        recomp_runtime_ui_set_toast(ui, title.c_str(),
                                    body.empty() ? nullptr : body.c_str());
        m.toast_until = Clock::now() + kToastTime;
        m.toast_shown = true;
    } else if (m.toast_shown && Clock::now() >= m.toast_until) {
        recomp_runtime_ui_set_toast(ui, nullptr, nullptr);
        m.toast_shown = false;
    }
}

}  // namespace

bool nds_netplay_menu_available() { return nds_online_available(); }

void nds_netplay_menu_append_items(std::vector<RecompRuntimeUiItem>* items) {
    MenuState& m = state();
    m.lobby_name = nds_online_lobby_name();
    m.status = nds_online_status();
    if (!m.keys_ready) {
        for (size_t i = 0; i < kLobbyRows; ++i)
            m.rows[i].key = kLobbyKeyPrefix + std::to_string(i);
        m.keys_ready = true;
    }
    refresh_rows(m);
    const auto action = [](const char* key, const char* section,
                           const char* label, const char* description) {
        RecompRuntimeUiItem item{};
        item.key = key;
        item.section = section;
        item.label = label;
        item.description = description;
        item.type = RECOMP_RUNTIME_UI_ACTION;
        return item;
    };
    RecompRuntimeUiItem enabled{};
    enabled.key = kEnabledKey;
    enabled.section = kSection;
    enabled.label = "Netplay";
    enabled.description = "Online lobbies. Off: the local network.";
    enabled.type = RECOMP_RUNTIME_UI_BOOL;
    enabled.maximum = 1;
    enabled.step = 1;
    items->push_back(enabled);
    items->push_back(action(kStatusKey, kSection, m.status_label.data(),
                            m.status_description.data()));
    items->push_back(action(kAccountKey, kSection, m.account_label.data(),
                            "The lobby server needs a Discord account."));
    items->push_back(action(kNameKey, kSection, m.name_label.data(),
                            m.name_description.data()));
    for (LobbyRow& row : m.rows)
        items->push_back(action(row.key.c_str(), kLobbiesPage,
                                row.label.data(), row.description.data()));
    items->push_back(action(kLeaveKey, kSection, "Leave lobby",
                            "Out of radio range of the host."));
}

bool nds_netplay_menu_owns(const RecompRuntimeUiItem* item) {
    return item && item->key && std::strncmp(item->key, "netplay.", 8) == 0;
}

int nds_netplay_menu_get_value(const RecompRuntimeUiItem* item, int* value) {
    if (!is(item, kEnabledKey)) return 0;
    *value = nds_online_enabled() ? 1 : 0;
    return 1;
}

int nds_netplay_menu_set_value(const RecompRuntimeUiItem* item, int value) {
    if (!is(item, kEnabledKey) || value < 0 || value > 1) return 0;
    nds_online_set_enabled(value != 0);
    nds_online_set_browsing(value != 0 && state().menu_open);
    state().dirty = true;
    std::fprintf(stderr, "[online] Netplay %s from the menu\n",
                 value ? "on" : "off");
    return 1;
}

int nds_netplay_menu_run_action(RecompRuntimeUi* ui,
                                const RecompRuntimeUiItem* item) {
    MenuState& m = state();
    if (is(item, kAccountKey)) {
        if (!m.status.account.empty()) {
            if (!recomp_runtime_ui_confirm(ui, item, "Press again to sign out"))
                return 0;
            nds_online_sign_out();
        } else {
            nds_online_sign_in();
            recomp_runtime_ui_set_status(ui, "See the browser");
            return 0;
        }
        return 1;
    }
    if (is(item, kNameKey)) {
        m.editing = true;
        m.edit = m.lobby_name;
        refresh_rows(m);
        return 0;
    }
    if (is(item, kLeaveKey)) {
        nds_online_leave();
        return 1;
    }
    const int row = lobby_row(item);
    if (row >= 0 && m.rows[row].joinable) {
        nds_online_join(m.rows[row].lobby.lobby_id);
        recomp_runtime_ui_set_status(ui, "Joining");
        return 0;
    }
    return 0;
}

int nds_netplay_menu_is_enabled(const RecompRuntimeUiItem* item) {
    const MenuState& m = state();
    const bool on = m.status.state != NdsOnlineState::Off;
    if (is(item, kEnabledKey) || is(item, kNameKey)) return 1;
    if (is(item, kStatusKey)) return 0;
    // Only a server that wants Discord sign-in has a use for the row; one that
    // does not offer it (a private server) leaves it disabled.
    if (is(item, kAccountKey))
        return on && (m.status.state == NdsOnlineState::NeedSignIn ||
                      m.status.state == NdsOnlineState::SigningIn ||
                      !m.status.account.empty())
                   ? 1 : 0;
    if (is(item, kLeaveKey))
        return m.status.state == NdsOnlineState::Guest ||
               m.status.state == NdsOnlineState::Joining;
    const int row = lobby_row(item);
    if (row >= 0) return m.rows[row].joinable ? 1 : 0;
    return 1;
}

void nds_netplay_menu_visibility(bool open) {
    MenuState& m = state();
    m.menu_open = open;
    if (!open) m.editing = false;
    nds_online_set_browsing(open && nds_online_enabled());
}

void nds_netplay_menu_tick(RecompRuntimeUi* ui) {
    MenuState& m = state();
    m.status = nds_online_status();
    if (m.menu_open) refresh_rows(m);
    if (ui) toast_changes(m, ui);
}

bool nds_netplay_menu_editing() { return state().editing; }

void nds_netplay_menu_edit_text(const char* utf8) {
    MenuState& m = state();
    if (!m.editing || !utf8) return;
    // The menu font has ASCII only.
    for (const char* p = utf8; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e) continue;
        if (m.edit.size() >= kMaxLobbyName) break;
        m.edit.push_back(*p);
    }
    refresh_rows(m);
}

bool nds_netplay_menu_edit_key(NdsNetplayEditKey key) {
    MenuState& m = state();
    if (!m.editing) return false;
    bool committed = false;
    switch (key) {
    case NdsNetplayEditKey::Erase:
        if (!m.edit.empty()) m.edit.pop_back();
        break;
    case NdsNetplayEditKey::Cancel:
        m.editing = false;
        break;
    case NdsNetplayEditKey::Commit: {
        const size_t first = m.edit.find_first_not_of(' ');
        const size_t last = m.edit.find_last_not_of(' ');
        m.lobby_name = first == std::string::npos
                           ? std::string()
                           : m.edit.substr(first, last - first + 1);
        nds_online_set_lobby_name(m.lobby_name);
        m.editing = false;
        m.dirty = true;
        committed = true;
        std::fprintf(stderr, "[online] lobby name set to \"%s\"\n",
                     m.lobby_name.c_str());
        break;
    }
    }
    refresh_rows(m);
    return committed;
}

bool nds_netplay_menu_settings(bool* enabled, std::string* lobby_name) {
    const MenuState& m = state();
    if (!m.dirty) return false;
    *enabled = nds_online_enabled();
    *lobby_name = m.lobby_name;
    return true;
}
