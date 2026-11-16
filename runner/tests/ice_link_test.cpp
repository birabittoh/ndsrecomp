// ice_link_test.cpp -- the online link (net/ice_link.h): relay-only
// filtering, and a hub with two guests linked through libjuice's own TURN
// server on 127.0.0.1, with the lobby's signal relay played by the test.

#include "net/ice_link.h"
#include "net/mp_wire.h"

#include <juice/juice.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using mpnet::IceLink;
using mpnet::PeerAddr;
using mpnet::SeatAddr;

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

bool TestFilter() {
    bool ok = true;
    const std::string sdp =
        "a=ice-ufrag:abcd\r\n"
        "a=ice-pwd:0123456789abcdef012345\r\n"
        "a=candidate:1 1 UDP 2122317823 192.168.1.20 50000 typ host\r\n"
        "a=candidate:2 1 UDP 1686052607 203.0.113.9 50000 typ srflx raddr "
        "0.0.0.0 rport 0\r\n"
        "a=candidate:3 1 UDP 16777215 198.51.100.7 61000 typ relay raddr "
        "0.0.0.0 rport 0\r\n"
        "a=end-of-candidates\r\n";
    const std::string out = mpnet::FilterRelayOnly(sdp);
    ok &= require(out.find("typ host") == std::string::npos,
                  "host candidate stripped from SDP");
    ok &= require(out.find("typ srflx") == std::string::npos,
                  "srflx candidate stripped from SDP");
    ok &= require(out.find("192.168.1.20") == std::string::npos &&
                      out.find("203.0.113.9") == std::string::npos,
                  "no local or reflexive address left in SDP");
    ok &= require(out.find("typ relay") != std::string::npos,
                  "relay candidate kept");
    ok &= require(out.find("a=ice-ufrag:abcd") != std::string::npos,
                  "other lines kept");
    ok &= require(mpnet::IsRelayCandidate(
                      "candidate:3 1 UDP 1 198.51.100.7 61000 typ relay"),
                  "relay trickle line accepted");
    ok &= require(!mpnet::IsRelayCandidate(
                      "candidate:1 1 UDP 1 10.0.0.2 5000 typ host"),
                  "host trickle line refused");
    ok &= require(!mpnet::IsRelayCandidate("candidate:9 garbage"),
                  "untyped line refused");
    for (int seat = 0; seat < mpnet::kMaxSeats; ++seat)
        ok &= require(mpnet::SeatOf(SeatAddr(seat)) == seat,
                      "seat address round trip");
    PeerAddr real;
    real.ip = 0x7F000001u;
    real.port = 26710;
    ok &= require(mpnet::SeatOf(real) == -1, "real address is no seat");
    return ok;
}

// What the lobby server does with `signal`: deliver to the named seat,
// stamped with the sender's seat.
void Relay(std::vector<IceLink*> links) {
    for (size_t from = 0; from < links.size(); ++from) {
        for (mpnet::IceSignal s : links[from]->TakeSignals()) {
            if (s.type == mpnet::kIceSignalCandidate ||
                s.type == mpnet::kIceSignalSdp)
                if (s.text.find("typ host") != std::string::npos ||
                    s.text.find("typ srflx") != std::string::npos)
                    std::fprintf(stderr,
                                 "FAIL: non-relay candidate signalled\n");
            const int to = s.seat;
            s.seat = static_cast<int>(from);
            if (to >= 0 && to < static_cast<int>(links.size()))
                links[to]->PushSignal(s);
        }
    }
}

bool AllRelay(IceLink& link, size_t want) {
    const auto status = link.Status();
    if (status.size() != want) return false;
    for (const auto& s : status)
        if (!s.relay) return false;
    return true;
}

bool RecvFrom(IceLink& link, PeerAddr* from, std::string* text,
              std::vector<IceLink*> pump) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) {
        uint8_t buf[mpnet::kMaxDatagramBytes];
        const int got = link.Recv(buf, sizeof(buf), from);
        if (got > 0) {
            text->assign(reinterpret_cast<const char*>(buf),
                         static_cast<size_t>(got));
            return true;
        }
        Relay(pump);
        link.WaitReadable(10000);
    }
    return false;
}

void Drain(IceLink& link) {
    uint8_t buf[mpnet::kMaxDatagramBytes];
    PeerAddr from;
    while (link.Recv(buf, sizeof(buf), &from) > 0) {
    }
}

bool TestHubAndGuests() {
    bool ok = true;
    juice_server_credentials_t cred;
    std::memset(&cred, 0, sizeof(cred));
    cred.username = "test";
    cred.password = "secret";
    cred.allocations_quota = 8;
    juice_server_config_t server_config;
    std::memset(&server_config, 0, sizeof(server_config));
    server_config.credentials = &cred;
    server_config.credentials_count = 1;
    server_config.max_allocations = 16;
    server_config.bind_address = "127.0.0.2";
    server_config.external_address = "127.0.0.2";
    server_config.port = 0;
    server_config.realm = "test";
    juice_server_t* server = juice_server_create(&server_config);
    if (!require(server != nullptr, "TURN server starts")) return false;

    mpnet::TurnConfig turn;
    turn.host = "127.0.0.2";
    turn.port = juice_server_get_port(server);
    turn.user = "test";
    turn.pass = "secret";

    IceLink hub, a, b;
    std::vector<IceLink*> links = {&hub, &a, &b};
    hub.SetRoom(0, 0, turn);
    a.SetRoom(1, 0, turn);
    b.SetRoom(2, 0, turn);
    for (IceLink* l : links) l->Open();
    hub.SetPeers({1, 2});
    a.SetPeers({0});
    b.SetPeers({0});

    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (Clock::now() < deadline &&
           !(AllRelay(hub, 2) && AllRelay(a, 1) && AllRelay(b, 1))) {
        Relay(links);
        for (IceLink* l : links) l->Poll(Clock::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ok &= require(AllRelay(hub, 2), "hub linked to both guests over relay");
    ok &= require(AllRelay(a, 1) && AllRelay(b, 1),
                  "guests linked to the hub over relay");
    if (!ok) {
        juice_server_destroy(server);
        return false;
    }

    PeerAddr from;
    std::string text;
    const std::string hello = "guest a to hub";
    ok &= require(a.SendTo(SeatAddr(0),
                           reinterpret_cast<const uint8_t*>(hello.data()),
                           hello.size()),
                  "guest sends to the hub");
    ok &= require(RecvFrom(hub, &from, &text, links) && text == hello &&
                      from == SeatAddr(1),
                  "hub hears guest a from seat 1");

    const std::string all = "hub to everyone";
    hub.SendDiscovery(reinterpret_cast<const uint8_t*>(all.data()),
                      all.size());
    ok &= require(RecvFrom(a, &from, &text, links) && text == all &&
                      from == SeatAddr(0),
                  "guest a hears the hub's broadcast");
    ok &= require(RecvFrom(b, &from, &text, links) && text == all &&
                      from == SeatAddr(0),
                  "guest b hears the hub's broadcast");

    const std::string direct = "guest a to guest b";
    a.SendTo(SeatAddr(2), reinterpret_cast<const uint8_t*>(direct.data()),
             direct.size());
    ok &= require(RecvFrom(b, &from, &text, links) && text == direct &&
                      from == SeatAddr(1),
                  "hub forwards guest a to guest b");

    const std::string bcast = "guest b to everyone";
    b.SendDiscovery(reinterpret_cast<const uint8_t*>(bcast.data()),
                    bcast.size());
    ok &= require(RecvFrom(hub, &from, &text, links) && text == bcast &&
                      from == SeatAddr(2),
                  "hub hears guest b's broadcast");
    ok &= require(RecvFrom(a, &from, &text, links) && text == bcast &&
                      from == SeatAddr(2),
                  "hub forwards guest b's broadcast to guest a");

    // Largest transport datagram, through the relay.
    std::vector<uint8_t> big(mpnet::kMaxDatagramBytes);
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<uint8_t>(i * 7);
    hub.SendTo(SeatAddr(1), big.data(), big.size());
    ok &= require(RecvFrom(a, &from, &text, links) &&
                      text.size() == big.size() &&
                      std::memcmp(text.data(), big.data(), big.size()) == 0,
                  "largest datagram arrives intact");

    // Leaving drops the seat; the hub no longer reaches it.
    hub.SetPeers({1});
    Drain(b);
    hub.SendDiscovery(reinterpret_cast<const uint8_t*>(all.data()),
                      all.size());
    ok &= require(RecvFrom(a, &from, &text, links) && text == all,
                  "remaining guest still reached");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint8_t buf[mpnet::kMaxDatagramBytes];
    ok &= require(b.Recv(buf, sizeof(buf), &from) == 0,
                  "dropped guest hears nothing");

    for (IceLink* l : links) l->ClearRoom();
    juice_server_destroy(server);
    return ok;
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    bool ok = true;
    ok &= TestFilter();
    ok &= TestHubAndGuests();
    std::printf("%s\n", ok ? "ice_link_test: ok" : "ice_link_test: FAILED");
    return ok ? 0 : 1;
}
