// online_lobby_test.cpp -- Netplay end to end without the game: one process
// hosts, another joins, through a lobby server and libjuice's TURN server on
// 127.0.0.1 (net/online_lobby.h, net/ice_link.h).
//
// Not a ctest: it needs a lobby server without Discord sign-in, e.g.
// recomp-net-server built from source. From Git Bash:
//
//   BIND_ADDR=127.0.0.1:18765 REQUIRE_AUTH=false recomp-net-server &
//   export NDS_ONLINE_TURN=127.0.0.2:34780:test:secret
//   online_lobby_test host ws://127.0.0.1:18765 &
//   online_lobby_test guest ws://127.0.0.1:18765
//
// The host stands in for a game whose MP host takes 3 more players, so it
// opens "Tester's lobby" with 4 seats. The guest joins the first lobby it
// lists, sends a datagram to the hub over the relay, and the hub echoes it.
// Each process prints PASS and exits 0 when its half worked.
//
// `online_lobby_test turn` runs only that TURN server, for two runners
// playing over a local lobby server (--netplay-lobby-url, NDS_ONLINE_TURN
// as above).

#include "net/ice_link.h"
#include "net/online_lobby.h"
#include "wifi_net.h"

#include <juice/juice.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

namespace {
std::atomic<bool> g_hosting{false};
}

// What the runner's transport would report.
bool nds_netplay_host_info(NdsNetplayHostInfo* out) {
    if (!g_hosting) return false;
    if (out) {
        out->accepting = true;
        out->free_slots = 3;
        std::snprintf(out->host_name, sizeof(out->host_name), "Tester");
    }
    return true;
}

bool nds_wifi_local_mp_radio_on() { return g_hosting; }

// No radio here: no beacons to answer probes with or to replay.
bool nds_wifi_local_mp_own_beacon(std::vector<uint8_t>*) { return false; }
void nds_wifi_local_mp_set_replay_beacons(std::vector<std::vector<uint8_t>>) {}
bool nds_wifi_local_mp_take_join_request(uint8_t*) { return false; }

namespace {

using Clock = std::chrono::steady_clock;

juice_server_t* StartTurn() {
    static juice_server_credentials_t cred;
    std::memset(&cred, 0, sizeof(cred));
    cred.username = "test";
    cred.password = "secret";
    cred.allocations_quota = 8;
    juice_server_config_t sc;
    std::memset(&sc, 0, sizeof(sc));
    sc.credentials = &cred;
    sc.credentials_count = 1;
    sc.max_allocations = 16;
    sc.bind_address = "127.0.0.2";
    sc.external_address = "127.0.0.2";
    sc.port = 34780;
    sc.realm = "test";
    juice_server_t* turn = juice_server_create(&sc);
    if (!turn) std::fprintf(stderr, "FAIL: TURN server on 127.0.0.2:34780\n");
    return turn;
}

// Only the TURN server, until killed: the relay for two runners testing
// Netplay on one machine against a local lobby server.
int Turn() {
    juice_server_t* turn = StartTurn();
    if (!turn) return 1;
    std::printf("TURN server on 127.0.0.2:34780 (user test, password "
                "secret); Ctrl+C to stop\n");
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(60));
}

bool WaitStatus(NdsOnlineState want, int min_linked, int seconds) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        const NdsOnlineStatus s = nds_online_status();
        if (s.state == want && s.linked >= min_linked) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fprintf(stderr, "FAIL: status is \"%s\"\n",
                 nds_online_status().text.c_str());
    return false;
}

bool RecvOne(mpnet::IceLink& link, std::string* text, mpnet::PeerAddr* from,
             int seconds) {
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline) {
        uint8_t buf[mpnet::kMaxDatagramBytes];
        const int got = link.Recv(buf, sizeof(buf), from);
        if (got > 0) {
            text->assign(reinterpret_cast<const char*>(buf),
                         static_cast<size_t>(got));
            return true;
        }
        link.WaitReadable(20000);
    }
    return false;
}

int Host(const std::string& url) {
    juice_server_t* turn = StartTurn();
    if (!turn) return 1;

    NdsOnlineConfig config;
    config.lobby_url = url;
    config.game_name = "OnlineLobbyTest";
    config.game_version = "1";
    config.enabled = true;
    nds_online_start(config);
    g_hosting = true;

    int rc = 1;
    if (WaitStatus(NdsOnlineState::Hosting, 1, 60)) {
        auto link = nds_online_link();
        link->Open();
        std::string text;
        mpnet::PeerAddr from;
        if (RecvOne(*link, &text, &from, 20) && text == "ping from guest") {
            const std::string echo = "echo: " + text;
            link->SendTo(from, reinterpret_cast<const uint8_t*>(echo.data()),
                         echo.size());
            std::printf("host: PASS (guest at seat %d)\n", mpnet::SeatOf(from));
            rc = 0;
            // Let the echo leave before the lobby closes.
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } else {
            std::fprintf(stderr, "FAIL: no datagram from the guest\n");
        }
    }
    g_hosting = false;  // the "game" stops hosting: the lobby closes
    std::this_thread::sleep_for(std::chrono::seconds(1));
    nds_online_stop();
    juice_server_destroy(turn);
    return rc;
}

int Guest(const std::string& url) {
    NdsOnlineConfig config;
    config.lobby_url = url;
    config.game_name = "OnlineLobbyTest";
    config.game_version = "1";
    config.enabled = true;
    config.join = "first";
    nds_online_start(config);

    int rc = 1;
    if (WaitStatus(NdsOnlineState::Guest, 1, 60)) {
        bool listed = false;
        for (const auto& row : nds_online_lobbies())
            if (row.name == "Tester's lobby" && row.max_slots == 4)
                listed = true;
        if (!listed)
            std::fprintf(stderr,
                         "FAIL: \"Tester's lobby\" with 4 seats not listed\n");
        auto link = nds_online_link();
        link->Open();
        // The link carries no reliability of its own (the transport adds
        // it), and the hub may not deliver yet: send until echoed.
        const std::string ping = "ping from guest";
        std::string text;
        mpnet::PeerAddr from;
        bool echoed = false;
        for (int i = 0; i < 40 && !echoed; ++i) {
            link->SendTo(mpnet::SeatAddr(0),
                         reinterpret_cast<const uint8_t*>(ping.data()),
                         ping.size());
            echoed = RecvOne(*link, &text, &from, 1);
        }
        if (echoed && text == "echo: ping from guest" &&
            from == mpnet::SeatAddr(0)) {
            if (listed) {
                std::printf("guest: PASS\n");
                rc = 0;
            }
        } else {
            std::fprintf(stderr, "FAIL: no echo from the hub\n");
        }
        // The host's lobby closes; the guest must notice.
        if (rc == 0 && !WaitStatus(NdsOnlineState::Idle, 0, 20)) rc = 1;
    }
    nds_online_stop();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (argc == 2 && std::string(argv[1]) == "turn") return Turn();
    if (argc != 3) {
        std::fprintf(stderr, "usage: online_lobby_test host|guest ws://...\n"
                             "       online_lobby_test turn\n");
        return 2;
    }
    const std::string role = argv[1];
    if (role == "host") return Host(argv[2]);
    if (role == "guest") return Guest(argv[2]);
    return 2;
}
