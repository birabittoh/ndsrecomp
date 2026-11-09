// online_lobby.h -- Netplay: local wireless with the players of an online
// lobby (HANDOFF_ONLINE_LOBBY.md in DQ9Recomp, "The goal").
//
// With Netplay on, the lobby replaces LAN discovery: only the members of
// the lobby this runner sits in are in radio range. Hosting is automatic:
// when the game becomes an MP host that takes players (nds_netplay_host_info,
// the title's beacon decoder) a lobby opens with one seat per free player
// slot, and it closes when hosting ends. Joining a lobby only puts the two
// consoles in range; the player then joins in the game's own menus.
//
// One thread owns the lobby server connection (WebSocket, JSON ops) and
// the ICE link's signalling. The lobby server requires Discord sign-in
// (recomp-net auth.h): sign-in opens the browser once and stores a device
// key in `secret_path`; later runs sign in from that key.
//
// Privacy: datagrams go through the TURN relay only (net/ice_link.h).
// This client sends no endpoint of its own and no match_caps. The lobby
// server still learns, and in its current version publishes, the
// connection's address (host_endpoint / guest_endpoint), see the handoff.
//
// Every function here is safe to call from any thread.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mpnet {
class IceLink;
}

struct NdsOnlineConfig {
    std::string lobby_url = "ws://netplay.retcomm.net:8765";
    // Lobby list filter and join check. Empty game_name: Netplay unusable.
    std::string game_name;
    std::string game_version;
    // Lobby name; empty = "<host's name>'s lobby".
    std::string lobby_name;
    // Discord device key file (recomp-net auth.h). Empty = next to the exe.
    std::string secret_path;
    bool enabled = false;
    // Test aids from the command line: begin Discord sign-in at startup
    // when there is no stored key; join this lobby id, or "first" for the
    // first listed lobby of this game that is not our own.
    bool sign_in = false;
    std::string join;
};

enum class NdsOnlineState : uint8_t {
    Off,
    NeedSignIn,
    SigningIn,
    Connecting,
    Idle,
    Hosting,
    Joining,
    Guest,
    Error,
};

struct NdsOnlineStatus {
    NdsOnlineState state = NdsOnlineState::Off;
    std::string text;     // one line for the menu and the log
    std::string account;  // Discord handle when signed in
    std::string lobby_name;
    int players = 0;
    int max_slots = 0;
    int linked = 0;       // members reachable through the relay
};

struct NdsOnlineLobbyRow {
    std::string lobby_id;
    std::string name;
    std::string country;
    int players = 0;
    int max_slots = 0;
};

void nds_online_start(const NdsOnlineConfig& config);
void nds_online_stop();

void nds_online_set_enabled(bool enabled);
bool nds_online_enabled();
void nds_online_set_lobby_name(const std::string& name);
void nds_online_sign_in();
void nds_online_sign_out();
// While browsing, the lobby list is refreshed every few seconds.
void nds_online_set_browsing(bool browsing);
std::vector<NdsOnlineLobbyRow> nds_online_lobbies();
void nds_online_join(const std::string& lobby_id);
void nds_online_leave();
NdsOnlineStatus nds_online_status();

// For the local wireless transport: the link to use while Netplay is on,
// null while it is off. The epoch changes whenever the answer changes.
std::shared_ptr<mpnet::IceLink> nds_online_link();
uint32_t nds_online_link_epoch();
