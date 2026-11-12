// netplay_menu.cpp -- see netplay_menu.h.

#include "netplay_menu.h"

#include "net/online_lobby.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kSection = "Netplay";
constexpr const char* kEnabledKey = "netplay.enabled";
constexpr const char* kAccountKey = "netplay.account";
// Signing out takes a second press within this time.
constexpr auto kConfirmTime = std::chrono::seconds(3);

using Text = std::array<char, 96>;

struct MenuState {
    Text account_label{};
    NdsOnlineStatus status;
    // The setting, kept as loaded so saving [online] does not clear it.
    std::string lobby_name;
    bool dirty = false;
    Clock::time_point confirm_until{};
};

MenuState& state() {
    static MenuState s;
    return s;
}

bool is(const NdsMenuItem* item, const char* key) {
    return item && item->key && std::strcmp(item->key, key) == 0;
}

std::string fit(const std::string& text, size_t width) {
    if (text.size() <= width) return text;
    return text.substr(0, width - 1) + "~";
}

bool confirming(const MenuState& m) {
    return !m.status.account.empty() && Clock::now() < m.confirm_until;
}

void refresh_label(MenuState& m) {
    std::string label;
    if (confirming(m))
        label = "Press again to sign out";
    else if (!m.status.account.empty())
        label = "Sign out (" + fit(m.status.account, 18) + ")";
    else if (m.status.state == NdsOnlineState::SigningIn)
        label = "Signing in: see the browser";
    else
        label = "Sign in with Discord";
    std::snprintf(m.account_label.data(), m.account_label.size(), "%s",
                  label.c_str());
}

}  // namespace

bool nds_netplay_menu_available() { return nds_online_available(); }

void nds_netplay_menu_append_items(std::vector<NdsMenuItem>* items) {
    MenuState& m = state();
    m.lobby_name = nds_online_lobby_name();
    m.status = nds_online_status();
    refresh_label(m);
    NdsMenuItem enabled{};
    enabled.key = kEnabledKey;
    enabled.section = kSection;
    enabled.label = "Netplay";
    enabled.description = "Online lobbies. Off: the local network.";
    enabled.type = NdsMenuItemType::Bool;
    enabled.maximum = 1;
    enabled.step = 1;
    items->push_back(enabled);
    NdsMenuItem account{};
    account.key = kAccountKey;
    account.section = kSection;
    account.label = m.account_label.data();
    account.description = "The lobby server needs a Discord account.";
    account.type = NdsMenuItemType::Action;
    items->push_back(account);
}

bool nds_netplay_menu_owns(const NdsMenuItem* item) {
    return item && item->key && std::strncmp(item->key, "netplay.", 8) == 0;
}

int nds_netplay_menu_get_value(const NdsMenuItem* item, int* value) {
    if (!is(item, kEnabledKey)) return 0;
    *value = nds_online_enabled() ? 1 : 0;
    return 1;
}

int nds_netplay_menu_set_value(const NdsMenuItem* item, int value) {
    if (!is(item, kEnabledKey) || value < 0 || value > 1) return 0;
    nds_online_set_enabled(value != 0);
    state().dirty = true;
    std::fprintf(stderr, "[online] Netplay %s from the menu\n",
                 value ? "on" : "off");
    return 1;
}

int nds_netplay_menu_run_action(const NdsMenuItem* item) {
    MenuState& m = state();
    if (!is(item, kAccountKey)) return 0;
    if (m.status.account.empty()) {
        nds_online_sign_in();
    } else if (!confirming(m)) {
        m.confirm_until = Clock::now() + kConfirmTime;
    } else {
        m.confirm_until = {};
        nds_online_sign_out();
    }
    refresh_label(m);
    return 0;
}

int nds_netplay_menu_is_enabled(const NdsMenuItem* item) {
    const MenuState& m = state();
    if (is(item, kEnabledKey)) return 1;
    // Only a server that wants Discord sign-in has a use for the row; one that
    // does not offer it (a private server) leaves it disabled.
    if (is(item, kAccountKey))
        return m.status.state != NdsOnlineState::Off &&
                       (m.status.state == NdsOnlineState::NeedSignIn ||
                        m.status.state == NdsOnlineState::SigningIn ||
                        !m.status.account.empty())
                   ? 1 : 0;
    return 0;
}

void nds_netplay_menu_tick() {
    if (!nds_online_available()) return;
    MenuState& m = state();
    m.status = nds_online_status();
    refresh_label(m);
}

bool nds_netplay_menu_settings(bool* enabled, std::string* lobby_name) {
    const MenuState& m = state();
    if (!m.dirty) return false;
    *enabled = nds_online_enabled();
    *lobby_name = m.lobby_name;
    return true;
}
