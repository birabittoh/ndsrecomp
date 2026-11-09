// online_lobby.cpp -- see online_lobby.h.

#include "net/online_lobby.h"

#include "net/ice_link.h"
#include "net/lobby_json.h"
#include "net/lobby_ws.h"
#include "wifi_net.h"

#include "recomp_net/auth.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using mpnet::Json;
using mpnet::JsonOp;

constexpr auto kListInterval = std::chrono::seconds(3);
constexpr auto kPingInterval = std::chrono::seconds(20);
constexpr auto kConnectBackoffMin = std::chrono::seconds(5);
constexpr auto kConnectBackoffMax = std::chrono::seconds(60);
// Beacons stop while the host's game is paused, not only when it stops
// hosting. With the radio still on, wait this long before closing.
constexpr auto kHostGoneWithRadioOn = std::chrono::seconds(30);
constexpr int kConnectTimeoutMs = 8000;

struct Member {
    int slot = -1;
    std::string player_id;
    std::string name;
};

struct Room {
    std::string lobby_id;
    std::string name;
    bool is_host = false;
    int local_slot = -1;
    std::string host_player_id;
    int max_slots = 0;
    std::vector<Member> members;
    bool linked = false;   // IceLink::SetRoom done for this room
    int linked_slot = -1;  // the local seat it was done with
    int linked_hub = -1;
};

const char* StateName(NdsOnlineState s) {
    switch (s) {
    case NdsOnlineState::Off: return "off";
    case NdsOnlineState::NeedSignIn: return "need sign-in";
    case NdsOnlineState::SigningIn: return "signing in";
    case NdsOnlineState::Connecting: return "connecting";
    case NdsOnlineState::Idle: return "idle";
    case NdsOnlineState::Hosting: return "hosting";
    case NdsOnlineState::Joining: return "joining";
    case NdsOnlineState::Guest: return "guest";
    case NdsOnlineState::Error: return "error";
    }
    return "?";
}

std::string ErrorText(const std::string& code) {
    if (code == "full") return "that lobby is full";
    if (code == "gone") return "that lobby is gone";
    if (code == "version_mismatch" || code == "game_mismatch")
        return "that lobby runs a different build";
    if (code == "name_rejected" || code == "lobby_name_rejected")
        return "the server refused the lobby name; change it in the menu";
    if (code == "login_required") return "the server wants you signed in again";
    if (code == "already_in_lobby") return "already in a lobby";
    if (code == "lobby_limit") return "the server has no room for more lobbies";
    return "server error: " + code;
}

class OnlineLobby {
public:
    void Start(const NdsOnlineConfig& config) {
        std::lock_guard<std::mutex> lock(mu_);
        if (thread_.joinable()) return;
        config_ = config;
        enabled_ = config.enabled;
        lobby_name_ = config.lobby_name;
        sign_in_request_ = config.sign_in;
        auto_join_ = config.join;
        if (enabled_) NewLinkLocked();
        quit_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!thread_.joinable()) return;
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    void SetEnabled(bool on) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (enabled_ == on) return;
            enabled_ = on;
            if (on) NewLinkLocked();
            else DropLinkLocked();
        }
        cv_.notify_all();
    }

    bool Enabled() {
        std::lock_guard<std::mutex> lock(mu_);
        return enabled_;
    }

    void SetLobbyName(const std::string& name) {
        std::lock_guard<std::mutex> lock(mu_);
        lobby_name_ = name;
    }

    void SignIn() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            sign_in_request_ = true;
        }
        cv_.notify_all();
    }

    void SignOut() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            sign_out_request_ = true;
        }
        cv_.notify_all();
    }

    void SetBrowsing(bool on) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            browsing_ = on;
        }
        cv_.notify_all();
    }

    std::vector<NdsOnlineLobbyRow> Lobbies() {
        std::lock_guard<std::mutex> lock(mu_);
        return rows_;
    }

    void Join(const std::string& id) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            join_request_ = id;
        }
        cv_.notify_all();
    }

    void Leave() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            leave_request_ = true;
        }
        cv_.notify_all();
    }

    NdsOnlineStatus Status() {
        std::lock_guard<std::mutex> lock(mu_);
        return status_;
    }

    std::shared_ptr<mpnet::IceLink> Link() {
        std::lock_guard<std::mutex> lock(mu_);
        return link_;
    }

    uint32_t LinkEpoch() const { return link_epoch_.load(); }

private:
    // ---- link ownership (mu_ held) ----------------------------------------
    void NewLinkLocked() {
        link_ = std::make_shared<mpnet::IceLink>();
        link_epoch_.fetch_add(1);
    }
    void DropLinkLocked() {
        link_.reset();
        link_epoch_.fetch_add(1);
    }

    // ---- the thread ---------------------------------------------------------
    void Run() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        if (!config_.secret_path.empty())
            rnet_account_set_secret_path(config_.secret_path.c_str());
        rnet_account_init(config_.lobby_url.c_str());
        std::fprintf(stderr, "[online] lobby server %s, game \"%s\" %s\n",
                     config_.lobby_url.c_str(), config_.game_name.c_str(),
                     config_.game_version.c_str());

        for (;;) {
            bool enabled, sign_in, sign_out, leave, browsing;
            std::string join, lobby_name;
            std::shared_ptr<mpnet::IceLink> link;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (quit_) break;
                enabled = enabled_;
                sign_in = sign_in_request_;
                sign_out = sign_out_request_;
                leave = leave_request_;
                browsing = browsing_;
                join.swap(join_request_);
                lobby_name = lobby_name_;
                link = link_;
                sign_in_request_ = sign_out_request_ = leave_request_ = false;
            }
            link_ref_ = link;
            lobby_name_now_ = lobby_name;
            browsing_now_ = browsing || !auto_join_.empty();
            Step(enabled, sign_in, sign_out, leave, join);
            if (!ws_.IsOpen()) {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_for(lock, std::chrono::milliseconds(100));
            }
        }
        LeaveRoom("shutting down");
        ws_.Close();
        if (link_ref_) link_ref_->ClearRoom();
        link_ref_.reset();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void Step(bool enabled, bool sign_in, bool sign_out, bool leave,
              const std::string& join) {
        rnet_account_pump();
        const auto now = Clock::now();

        if (sign_out) {
            Disconnect("signed out");
            rnet_account_sign_out();
        }
        if (sign_in && rnet_account_state() != RNET_ACCOUNT_SIGNED_IN &&
            rnet_account_state() != RNET_ACCOUNT_WAITING) {
            std::fprintf(stderr,
                         "[online] Discord sign-in: opening the browser\n");
            rnet_account_login_begin();
        }

        if (!enabled) {
            if (ws_.IsOpen()) Disconnect("Netplay off");
            Publish(NdsOnlineState::Off, "Off");
            return;
        }
        if (config_.game_name.empty()) {
            Publish(NdsOnlineState::Error, "this build has no game name for "
                                           "the lobby server");
            return;
        }

        // A server that wants Discord sign-in says so (login_required);
        // until then, and on servers that do not, connect as a guest.
        const char* session = rnet_account_session();
        const std::string session_now = session ? session : "";
        if (need_login_ && !session_now.empty() &&
            session_now != refused_session_) {
            need_login_ = false;
            next_connect_ = {};
        }
        if (need_login_) {
            if (rnet_account_state() == RNET_ACCOUNT_WAITING) {
                Publish(NdsOnlineState::SigningIn,
                        "Waiting for Discord sign-in in the browser");
            } else if (rnet_account_state() == RNET_ACCOUNT_FAILED) {
                const char* err = rnet_account_error();
                Publish(NdsOnlineState::NeedSignIn,
                        std::string("Sign-in failed: ") +
                            (err && err[0] ? err : "unknown error"));
            } else {
                Publish(NdsOnlineState::NeedSignIn,
                        "Sign in with Discord to play online");
            }
            return;
        }

        if (!ws_.IsOpen()) {
            if (now < next_connect_) return;
            // A stored key is being redeemed: connect with its session.
            if (rnet_account_state() == RNET_ACCOUNT_WAITING &&
                session_now.empty()) {
                Publish(NdsOnlineState::SigningIn, "Signing in");
                return;
            }
            Publish(NdsOnlineState::Connecting,
                    "Connecting to the lobby server");
            std::string error;
            if (!ws_.Connect(config_.lobby_url, kConnectTimeoutMs, &error)) {
                connect_backoff_ = std::min<Clock::duration>(
                    kConnectBackoffMax,
                    std::max<Clock::duration>(kConnectBackoffMin,
                                              connect_backoff_ * 2));
                next_connect_ = Clock::now() + connect_backoff_;
                Publish(NdsOnlineState::Error,
                        "Lobby server unreachable (" + error + "), retrying");
                return;
            }
            std::fprintf(stderr, "[online] connected to the lobby server\n");
            welcomed_ = false;
            player_id_.clear();
            turn_ok_ = false;
            turn_error_.clear();
            last_ping_ = Clock::now();
            last_list_ = {};
        }

        std::vector<std::string> messages;
        std::string error;
        if (!ws_.Poll(20, &messages, &error)) {
            OnConnectionLost(error);
            return;
        }
        for (const std::string& text : messages) Handle(Json::Parse(text));
        if (!ws_.IsOpen()) return;
        if (!welcomed_) {
            Publish(NdsOnlineState::Connecting,
                    "Connecting to the lobby server");
            return;
        }
        connect_backoff_ = {};

        if (now - last_ping_ >= kPingInterval) {
            Send(JsonOp("ping").Text());
            last_ping_ = now;
        }
        if (leave && !room_.lobby_id.empty()) LeaveRoom("left");
        if (!join.empty()) RequestJoin(join);
        if (browsing_now_ && now - last_list_ >= kListInterval) {
            Send(JsonOp("list").Str("game_name", config_.game_name)
                     .Str("game_version", config_.game_version)
                     .Text());
            last_list_ = now;
        }
        HostLogic(now);
        LinkLogic(now);
        PublishRoom();
    }

    // ---- connection -----------------------------------------------------

    bool Send(const std::string& text) {
        if (!ws_.IsOpen()) return false;
        if (!ws_.SendText(text)) {
            OnConnectionLost("send failed");
            return false;
        }
        return true;
    }

    void Disconnect(const char* why) {
        if (!ws_.IsOpen()) return;
        LeaveRoom(why);
        ws_.Close();
        welcomed_ = false;
        std::fprintf(stderr, "[online] disconnected from the lobby server (%s)\n",
                     why);
    }

    void OnConnectionLost(const std::string& why) {
        std::fprintf(stderr, "[online] lobby connection lost: %s\n",
                     why.c_str());
        ws_.Close();
        welcomed_ = false;
        ClearRoom();
        connect_backoff_ =
            std::max<Clock::duration>(kConnectBackoffMin, connect_backoff_);
        next_connect_ = Clock::now() + connect_backoff_;
    }

    // ---- server messages --------------------------------------------------

    void Handle(const Json& msg) {
        if (!msg.IsObject()) return;
        const std::string op = msg.Str("op", "");
        if (op == "welcome") {
            player_id_ = msg.Str("player_id", "");
            welcomed_ = !player_id_.empty();
            const char* handle = rnet_account_handle();
            JsonOp hello("hello");
            hello.Str("display_name", handle && handle[0] ? handle : "Player")
                .Str("game_name", config_.game_name);
            const char* session = rnet_account_session();
            if (session && session[0]) hello.Str("session", session);
            Send(hello.Text());
            if (!TurnOverride()) Send(JsonOp("get_turn_credentials").Text());
        } else if (op == "turn_credentials") {
            OnTurn(msg);
        } else if (op == "lobby_list") {
            OnList(msg);
        } else if (op == "created") {
            OnCreated(msg);
        } else if (op == "joined") {
            OnJoined(msg);
        } else if (op == "lobby_update") {
            OnUpdate(msg);
        } else if (op == "lobby_closed" || op == "kicked") {
            if (msg.Str("lobby_id", "") == room_.lobby_id ||
                msg.Str("lobby_id", "").empty()) {
                std::fprintf(stderr, "[online] lobby closed\n");
                ClearRoom();
            }
        } else if (op == "signal") {
            OnSignal(msg);
        } else if (op == "error") {
            OnError(msg.Str("code", ""));
        }
        // hello_ok, pong, left, chat, server_chat, probe_signal: nothing.
    }

    // Test aid: NDS_ONLINE_TURN=host:port:user:pass replaces the server's
    // TURN credentials (a local server has no coturn; online_lobby_test
    // runs libjuice's TURN server instead).
    bool TurnOverride() {
        const char* env = std::getenv("NDS_ONLINE_TURN");
        if (!env || !env[0]) return false;
        const std::string text = env;
        const size_t a = text.find(':');
        const size_t b = a == std::string::npos ? a : text.find(':', a + 1);
        const size_t c = b == std::string::npos ? b : text.find(':', b + 1);
        if (c == std::string::npos) {
            std::fprintf(stderr, "[online] NDS_ONLINE_TURN must be "
                                 "host:port:user:pass, ignored\n");
            return false;
        }
        turn_.host = text.substr(0, a);
        turn_.port = static_cast<uint16_t>(
            std::atoi(text.substr(a + 1, b - a - 1).c_str()));
        turn_.user = text.substr(b + 1, c - b - 1);
        turn_.pass = text.substr(c + 1);
        turn_ok_ = true;
        std::fprintf(stderr, "[online] relay %s:%u (NDS_ONLINE_TURN)\n",
                     turn_.host.c_str(), static_cast<unsigned>(turn_.port));
        return true;
    }

    void OnTurn(const Json& msg) {
        if (!msg.Bool("ok", false)) {
            turn_error_ = "The lobby server has no relay (" +
                          msg.Str("error", "no TURN credentials") + ")";
            turn_ok_ = false;
            return;
        }
        turn_error_.clear();
        turn_.host = msg.Str("turn_host", "");
        turn_.port = static_cast<uint16_t>(msg.Int("turn_port", 3478));
        turn_.user = msg.Str("username", "");
        turn_.pass = msg.Str("password", "");
        turn_ok_ = !turn_.host.empty() && !turn_.user.empty();
        if (turn_ok_)
            std::fprintf(stderr, "[online] relay %s:%u\n", turn_.host.c_str(),
                         static_cast<unsigned>(turn_.port));
    }

    void OnList(const Json& msg) {
        std::vector<NdsOnlineLobbyRow> rows;
        const Json& lobbies = msg["lobbies"];
        for (size_t i = 0; i < lobbies.size(); ++i) {
            const Json& l = lobbies[i];
            if (l.Str("game_name", "") != config_.game_name ||
                l.Str("game_version", "") != config_.game_version ||
                l.Bool("has_password", false))
                continue;
            NdsOnlineLobbyRow row;
            row.lobby_id = l.Str("lobby_id", "");
            row.name = l.Str("name", "");
            row.country = l.Str("host_country", "");
            row.players = l.Int("player_count", 0);
            row.max_slots = l.Int("max_slots", 0);
            if (row.lobby_id.empty()) continue;
            rows.push_back(std::move(row));
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            rows_ = rows;
        }
        if (!auto_join_.empty() && room_.lobby_id.empty() && !join_pending_) {
            for (const auto& row : rows) {
                if (auto_join_ == "first" ? row.players < row.max_slots
                                          : row.lobby_id == auto_join_) {
                    std::fprintf(stderr,
                                 "[online] --netplay-join: joining \"%s\"\n",
                                 row.name.c_str());
                    auto_join_.clear();
                    RequestJoin(row.lobby_id);
                    break;
                }
            }
        }
    }

    static std::vector<Member> ParseSlots(const Json& slots) {
        std::vector<Member> out;
        for (size_t i = 0; i < slots.size(); ++i) {
            Member m;
            m.slot = slots[i].Int("slot", -1);
            m.player_id = slots[i].Str("player_id", "");
            m.name = slots[i].Str("display_name", "");
            if (m.slot >= 0 && m.slot < mpnet::kMaxSeats &&
                !m.player_id.empty())
                out.push_back(std::move(m));
        }
        return out;
    }

    void OnCreated(const Json& msg) {
        create_pending_ = false;
        room_ = Room{};
        room_.lobby_id = msg.Str("lobby_id", "");
        room_.name = pending_name_;
        room_.is_host = true;
        room_.local_slot = msg.Int("local_slot", 0);
        room_.host_player_id = player_id_;
        room_.max_slots = msg.Int("max_slots", 0);
        room_.members = ParseSlots(msg["slots"]);
        std::fprintf(stderr, "[online] lobby \"%s\" open, %d seats\n",
                     room_.name.c_str(), room_.max_slots);
    }

    void OnJoined(const Json& msg) {
        join_pending_ = false;
        const std::string id = msg.Str("lobby_id", "");
        if (msg.Bool("spectator", false)) {
            // A gallery seat is no radio; never sit in one.
            Send(JsonOp("leave").Text());
            Publish(NdsOnlineState::Error, "that lobby is full");
            return;
        }
        room_ = Room{};
        room_.lobby_id = id;
        room_.name = pending_name_;
        room_.is_host = false;
        room_.local_slot = msg.Int("local_slot", -1);
        std::fprintf(stderr, "[online] joined \"%s\", seat %d\n",
                     room_.name.c_str(), room_.local_slot);
    }

    void OnUpdate(const Json& msg) {
        if (room_.lobby_id.empty() ||
            msg.Str("lobby_id", "") != room_.lobby_id)
            return;
        const auto before = room_.members.size();
        room_.members = ParseSlots(msg["slots"]);
        room_.host_player_id = msg.Str("host_player_id", room_.host_player_id);
        room_.max_slots = msg.Int("max_slots", room_.max_slots);
        for (const auto& m : room_.members)
            if (m.player_id == player_id_) room_.local_slot = m.slot;
        if (room_.members.size() != before)
            std::fprintf(stderr, "[online] lobby \"%s\": %zu of %d seats taken\n",
                         room_.name.c_str(), room_.members.size(),
                         room_.max_slots);
    }

    void OnSignal(const Json& msg) {
        if (room_.lobby_id.empty() || !link_ref_) return;
        const int type = msg.Int("type", 0);
        if (type != mpnet::kIceSignalSdp && type != mpnet::kIceSignalCandidate &&
            type != mpnet::kIceSignalDone)
            return;
        const std::string from = msg.Str("from_player_id", "");
        const Member* m = MemberById(from);
        if (!m) return;
        mpnet::IceSignal s;
        s.seat = m->slot;
        s.type = type;
        s.gen = msg.Int("flag", 0);
        s.text = msg.Str("text", "");
        link_ref_->PushSignal(s);
    }

    void OnError(const std::string& code) {
        std::fprintf(stderr, "[online] server error: %s\n", code.c_str());
        if (code == "login_required") {
            // Wait for a session this server has not refused yet.
            const char* session = rnet_account_session();
            refused_session_ = session ? session : "";
            need_login_ = true;
            Disconnect("sign-in required");
            return;
        }
        if (create_pending_) {
            create_pending_ = false;
            // Do not reopen until the name or the hosting state changes.
            create_refused_name_ = pending_name_;
        }
        if (join_pending_) join_pending_ = false;
        last_error_ = ErrorText(code);
        last_error_at_ = Clock::now();
    }

    // ---- hosting ------------------------------------------------------------

    std::string LobbyName(const NdsNetplayHostInfo& info) const {
        if (!lobby_name_now_.empty()) return lobby_name_now_;
        if (info.host_name[0]) return std::string(info.host_name) + "'s lobby";
        return "Lobby";
    }

    void HostLogic(Clock::time_point now) {
        NdsNetplayHostInfo info;
        const bool hosting = nds_netplay_host_info(&info);
        if (hosting) host_seen_ = now;

        if (room_.is_host && !room_.lobby_id.empty()) {
            // Closed in the game: the radio went off, or beacons have been
            // gone too long for a pause.
            const bool radio = nds_wifi_local_mp_radio_on();
            if (!hosting && (!radio || now - host_seen_ > kHostGoneWithRadioOn))
                LeaveRoom("hosting ended in the game");
            return;
        }
        if (!hosting) {
            create_refused_name_.clear();
            return;
        }
        if (!room_.lobby_id.empty() || join_pending_) {
            if (!host_while_guest_logged_) {
                std::fprintf(stderr,
                             "[online] hosting in the game while in another "
                             "lobby: no lobby opened\n");
                host_while_guest_logged_ = true;
            }
            return;
        }
        host_while_guest_logged_ = false;
        if (create_pending_ || !turn_ok_ || !info.accepting ||
            info.free_slots <= 0)
            return;
        const std::string name = LobbyName(info);
        if (name == create_refused_name_) return;
        pending_name_ = name;
        create_pending_ = true;
        const int max_slots = std::min(1 + info.free_slots, mpnet::kMaxSeats);
        std::fprintf(stderr, "[online] opening lobby \"%s\" (%d seats)\n",
                     name.c_str(), max_slots);
        Send(JsonOp("create")
                 .Str("name", name)
                 .Str("game_name", config_.game_name)
                 .Str("game_version", config_.game_version)
                 .Int("max_slots", max_slots)
                 .Text());
    }

    void RequestJoin(const std::string& id) {
        NdsNetplayHostInfo info;
        if (nds_netplay_host_info(&info) || room_.is_host) {
            last_error_ = "close your own multiplayer session first";
            last_error_at_ = Clock::now();
            return;
        }
        if (!room_.lobby_id.empty()) {
            if (room_.lobby_id == id) return;
            LeaveRoom("joining another lobby");
        }
        pending_name_ = id;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& row : rows_)
                if (row.lobby_id == id) pending_name_ = row.name;
        }
        join_pending_ = true;
        std::fprintf(stderr, "[online] joining \"%s\"\n", pending_name_.c_str());
        Send(JsonOp("join")
                 .Str("lobby_id", id)
                 .Str("game_name", config_.game_name)
                 .Str("game_version", config_.game_version)
                 .Text());
    }

    // Tells the server (host: close, guest: leave) and forgets the room.
    void LeaveRoom(const char* why) {
        if (room_.lobby_id.empty()) return;
        std::fprintf(stderr, "[online] leaving \"%s\" (%s)\n",
                     room_.name.c_str(), why);
        if (ws_.IsOpen())
            ws_.SendText(JsonOp(room_.is_host ? "close" : "leave").Text());
        ClearRoom();
    }

    void ClearRoom() {
        room_ = Room{};
        create_pending_ = false;
        join_pending_ = false;
        if (link_ref_) link_ref_->ClearRoom();
    }

    // ---- the link -----------------------------------------------------------

    const Member* MemberById(const std::string& id) const {
        for (const auto& m : room_.members)
            if (m.player_id == id) return &m;
        return nullptr;
    }

    const Member* MemberBySlot(int slot) const {
        for (const auto& m : room_.members)
            if (m.slot == slot) return &m;
        return nullptr;
    }

    void LinkLogic(Clock::time_point now) {
        if (!link_ref_) return;
        if (room_.lobby_id.empty() || !turn_ok_) {
            if (link_ref_->InRoom()) link_ref_->ClearRoom();
            return;
        }
        const Member* hub = MemberById(room_.host_player_id);
        if (!hub || room_.local_slot < 0) return;
        if (!link_ref_->InRoom() || room_.linked_slot != room_.local_slot ||
            room_.linked_hub != hub->slot) {
            link_ref_->SetRoom(room_.local_slot, hub->slot, turn_);
            room_.linked = true;
            room_.linked_slot = room_.local_slot;
            room_.linked_hub = hub->slot;
        }
        std::vector<int> seats;
        if (room_.is_host) {
            for (const auto& m : room_.members)
                if (m.slot != room_.local_slot) seats.push_back(m.slot);
        } else {
            seats.push_back(hub->slot);
        }
        std::sort(seats.begin(), seats.end());
        if (seats != linked_seats_ || !peers_set_) {
            link_ref_->SetPeers(seats);
            linked_seats_ = seats;
            peers_set_ = true;
        }
        link_ref_->Poll(now);
        for (const mpnet::IceSignal& s : link_ref_->TakeSignals()) {
            const Member* to = MemberBySlot(s.seat);
            if (!to) continue;
            Send(JsonOp("signal")
                     .Str("lobby_id", room_.lobby_id)
                     .Str("to_player_id", to->player_id)
                     .Int("type", s.type)
                     .Int("flag", s.gen)
                     .Str("text", s.text)
                     .Text());
        }
    }

    // ---- status ---------------------------------------------------------------

    void PublishRoom() {
        int linked = 0;
        if (link_ref_)
            for (const auto& p : link_ref_->Status())
                if (p.relay) ++linked;
        if (room_.lobby_id.empty()) {
            linked_seats_.clear();
            peers_set_ = false;
            if (join_pending_) {
                Publish(NdsOnlineState::Joining,
                        "Joining \"" + pending_name_ + "\"");
            } else if (create_pending_) {
                Publish(NdsOnlineState::Hosting,
                        "Opening lobby \"" + pending_name_ + "\"");
            } else if (!turn_ok_) {
                if (turn_error_.empty())
                    Publish(NdsOnlineState::Connecting, "Waiting for the relay");
                else
                    Publish(NdsOnlineState::Error, turn_error_);
            } else if (!last_error_.empty() &&
                       Clock::now() - last_error_at_ < std::chrono::seconds(10)) {
                Publish(NdsOnlineState::Idle, "Online: " + last_error_);
            } else {
                Publish(NdsOnlineState::Idle,
                        "Online: host in the game, or join a lobby");
            }
            return;
        }
        const int players = static_cast<int>(room_.members.size());
        char count[32];
        std::snprintf(count, sizeof(count), " (%d/%d)", players,
                      room_.max_slots);
        if (room_.is_host) {
            Publish(NdsOnlineState::Hosting,
                    "Hosting \"" + room_.name + "\"" + count, players,
                    linked);
        } else if (linked == 0) {
            Publish(NdsOnlineState::Guest,
                    "In \"" + room_.name + "\"" + count +
                        ": connecting to the host",
                    players, linked);
        } else {
            Publish(NdsOnlineState::Guest,
                    "In \"" + room_.name + "\"" + count +
                        ": join the host from the game's multiplayer menu",
                    players, linked);
        }
    }

    void Publish(NdsOnlineState state, const std::string& text, int players = 0,
                 int linked = 0) {
        NdsOnlineStatus s;
        s.state = state;
        s.text = text;
        const char* handle = rnet_account_handle();
        if (rnet_account_state() == RNET_ACCOUNT_SIGNED_IN && handle)
            s.account = handle;
        s.lobby_name = room_.name;
        s.players = players;
        s.max_slots = room_.max_slots;
        s.linked = linked;
        bool changed;
        {
            std::lock_guard<std::mutex> lock(mu_);
            changed = status_.state != s.state || status_.text != s.text ||
                      status_.linked != s.linked;
            status_ = s;
        }
        if (changed)
            std::fprintf(stderr, "[online] status: %s: %s\n", StateName(state),
                         text.c_str());
    }

    // ---- members ------------------------------------------------------------

    std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    bool quit_ = false;
    NdsOnlineConfig config_;
    // Requests, under mu_.
    bool enabled_ = false;
    bool sign_in_request_ = false;
    bool sign_out_request_ = false;
    bool leave_request_ = false;
    bool browsing_ = false;
    std::string join_request_;
    std::string lobby_name_;
    // Published, under mu_.
    NdsOnlineStatus status_;
    std::vector<NdsOnlineLobbyRow> rows_;
    std::shared_ptr<mpnet::IceLink> link_;
    std::atomic<uint32_t> link_epoch_{0};

    // The thread's own.
    std::shared_ptr<mpnet::IceLink> link_ref_;
    std::string lobby_name_now_;
    bool browsing_now_ = false;
    std::string auto_join_;
    mpnet::WsClient ws_;
    bool welcomed_ = false;
    bool need_login_ = false;
    std::string refused_session_;
    std::string player_id_;
    mpnet::TurnConfig turn_;
    bool turn_ok_ = false;
    std::string turn_error_;
    Clock::time_point next_connect_{};
    Clock::duration connect_backoff_{};
    Clock::time_point last_ping_{};
    Clock::time_point last_list_{};
    Clock::time_point host_seen_{};
    Room room_;
    bool create_pending_ = false;
    bool join_pending_ = false;
    std::string pending_name_;
    std::string create_refused_name_;
    bool host_while_guest_logged_ = false;
    std::vector<int> linked_seats_;
    bool peers_set_ = false;
    std::string last_error_;
    Clock::time_point last_error_at_{};
};

OnlineLobby& Instance() {
    static OnlineLobby lobby;
    return lobby;
}

}  // namespace

void nds_online_start(const NdsOnlineConfig& config) {
    Instance().Start(config);
}
void nds_online_stop() { Instance().Stop(); }
void nds_online_set_enabled(bool enabled) { Instance().SetEnabled(enabled); }
bool nds_online_enabled() { return Instance().Enabled(); }
void nds_online_set_lobby_name(const std::string& name) {
    Instance().SetLobbyName(name);
}
void nds_online_sign_in() { Instance().SignIn(); }
void nds_online_sign_out() { Instance().SignOut(); }
void nds_online_set_browsing(bool browsing) {
    Instance().SetBrowsing(browsing);
}
std::vector<NdsOnlineLobbyRow> nds_online_lobbies() {
    return Instance().Lobbies();
}
void nds_online_join(const std::string& lobby_id) { Instance().Join(lobby_id); }
void nds_online_leave() { Instance().Leave(); }
NdsOnlineStatus nds_online_status() { return Instance().Status(); }
std::shared_ptr<mpnet::IceLink> nds_online_link() { return Instance().Link(); }
uint32_t nds_online_link_epoch() { return Instance().LinkEpoch(); }
