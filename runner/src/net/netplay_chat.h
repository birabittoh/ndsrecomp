// netplay_chat.h -- text chat between the players of a local wireless
// session.
//
// Two paths feed one list of lines:
//  - Members of the online lobby this runner sits in: the lobby server's
//    room chat (op "chat", net/online_lobby.cpp). The server masks the
//    line, gives it an id (`mid`, what a moderation report names) and
//    relays it to everyone in the room, the sender included, so the
//    sender's own line comes back that way.
//  - Runners on the LAN: a chat frame on the local wireless transport
//    (wifi_net.cpp), reliable, to the peers of this radio session (same
//    BSSID). No server: every receiver masks the line on arrival
//    (recomp-net chat_filter.h, online builds), and the sender lists its
//    own line at once. A chat frame that comes over the online link is
//    dropped, since those members get the line from the server.
//
// Lines are single-line UTF-8, control characters removed, at most
// kNdsChatTextMax bytes (the server's own limit is 240 characters).
//
// Every function here is safe to call from any thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr size_t kNdsChatTextMax = 240;  // bytes
constexpr size_t kNdsChatNameMax = 32;   // bytes
// Lines kept for the UI; older ones are dropped.
constexpr size_t kNdsChatKeep = 64;

enum class NdsChatSource : uint8_t {
    Lan,     // a LAN runner, through the transport
    Online,  // a lobby member, through the server
    System,  // the runner itself (an error from the server, ...)
};

struct NdsChatLine {
    uint64_t id = 0;  // increasing from 1, local to this runner
    NdsChatSource source = NdsChatSource::System;
    bool self = false;  // this runner's own line
    std::string from;   // display name, empty for System
    std::string text;   // masked
    std::string mid;    // the server's id, Online lines only
};

enum class NdsChatSendResult : uint8_t {
    Sent,
    Empty,    // nothing left after cleaning
    NoOne,    // neither in an online room nor with LAN peers in session
};

// Sends a line to everyone in this session, by whichever paths reach them.
NdsChatSendResult nds_netplay_chat_send(const std::string& text);

// Lines with an id above `after`, oldest first.
std::vector<NdsChatLine> nds_netplay_chat_since(uint64_t after);
// The newest line's id, 0 for none yet.
uint64_t nds_netplay_chat_last_id();
void nds_netplay_chat_clear();

// The name LAN peers see. Empty (the default): the Discord handle when
// signed in, else "Player". Online lines carry the name the server knows.
void nds_netplay_chat_set_name(const std::string& name);
std::string nds_netplay_chat_name();

// True when a line would reach somebody now.
bool nds_netplay_chat_reachable();

// ---- for the transports ----------------------------------------------------

// Removes control characters, folds the line to one, trims spaces and cuts
// it at `max_bytes` on a character boundary.
std::string nds_netplay_chat_clean(const std::string& text, size_t max_bytes);

// A received line: cleaned and masked, then listed with the next id.
void nds_netplay_chat_push(NdsChatLine line);

// wifi_net.cpp, on the emulation thread: how many LAN peers the session
// has now (0 with the radio off or without a BSSID), and the lines to send
// them. Lines queued while nobody was there are not kept.
void nds_netplay_chat_set_lan_peers(int count);
bool nds_netplay_chat_take_lan(std::string* from, std::string* text);

// online_lobby.cpp, on its thread: whether it sits in a room it can chat
// in, and the lines to send there.
void nds_netplay_chat_set_online_room(bool in_room);
bool nds_netplay_chat_take_online(std::string* text);
