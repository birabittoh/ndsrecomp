// mp_link_test.cpp -- UDP links of the local wireless transport
// (net/mp_link.h): discovery, unicast and the datagram header over real
// sockets on 127.0.0.1, in loopback and lan mode, and reliable delivery
// (net/mp_reliable.h) on top of them.

#include "net/mp_link.h"
#include "net/mp_reliable.h"
#include "net/mp_wire.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#endif

namespace {

using mpnet::Frame;
using mpnet::PeerAddr;

constexpr uint32_t kLoopbackIp = 0x7F000001u;

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

// Waits up to 1 s for one datagram on `link`.
bool RecvOne(mpnet::MpLink& link, std::vector<uint8_t>* out,
             PeerAddr* from) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t buf[mpnet::kMaxDatagramBytes];
        const int got = link.Recv(buf, sizeof(buf), from);
        if (got > 0) {
            out->assign(buf, buf + got);
            return true;
        }
        if (got < 0) return false;
        link.WaitReadable(10000);
    }
    return false;
}

void DrainAll(mpnet::MpLink& link) {
    uint8_t buf[mpnet::kMaxDatagramBytes];
    PeerAddr from;
    while (link.Recv(buf, sizeof(buf), &from) > 0) {
    }
}

bool TestDiscoveryAndUnicast(bool lan, uint16_t base_port) {
    const char* label = lan ? "lan" : "loopback";
    mpnet::UdpLinkConfig c0;
    c0.lan = lan;
    c0.instance = 0;
    c0.base_port = base_port;
    mpnet::UdpLinkConfig c1 = c0;
    c1.instance = 1;
    auto l0 = mpnet::MakeUdpLink(c0);
    auto l1 = mpnet::MakeUdpLink(c1);
    if (!require(l0->Open() && l1->Open(), "links open")) return false;

    Frame hello;
    hello.node = 0xAAAA0000u;
    hello.flags = mpnet::kFlagHello;
    hello.next_seq = 1;
    std::vector<uint8_t> bytes;
    mpnet::Encode(hello, &bytes);
    l0->SendDiscovery(bytes.data(), bytes.size());

    // Instance 1 hears it from instance 0's port (from 127.0.0.1, or in
    // lan mode possibly from this machine's LAN address via broadcast).
    std::vector<uint8_t> got;
    PeerAddr from;
    bool ok = require(RecvOne(*l1, &got, &from), "discovery reaches peer");
    Frame decoded;
    ok = ok && require(mpnet::Decode(got.data(), got.size(), &decoded) &&
                           decoded.node == hello.node &&
                           decoded.flags == mpnet::kFlagHello,
                       "hello decodes");
    ok = ok && require(from.port == base_port, "source is instance 0's port");
    if (!lan)
        ok = ok && require(from.ip == kLoopbackIp, "loopback source address");
    if (!ok) {
        std::fprintf(stderr, "  (%s)\n", label);
        return false;
    }

    // Unicast back to where the hello came from, with a full payload.
    Frame reply;
    reply.node = 0xBBBB0000u;
    reply.type = 2u | (1u << 16u);
    reply.timestamp = 123456789;
    reply.seq = 1;
    reply.payload.resize(mpnet::kMaxPayloadBytes);
    for (size_t i = 0; i < reply.payload.size(); ++i)
        reply.payload[i] = static_cast<uint8_t>(i * 7);
    mpnet::Encode(reply, &bytes);
    ok = require(l1->SendTo(from, bytes.data(), bytes.size()),
                 "unicast send");
    // Instance 0 may also have heard its own broadcast in lan mode; skip
    // anything that is not the reply.
    bool found = false;
    for (int i = 0; i < 4 && !found; ++i) {
        PeerAddr reply_from;
        if (!RecvOne(*l0, &got, &reply_from)) break;
        if (!mpnet::Decode(got.data(), got.size(), &decoded)) continue;
        found = decoded.node == reply.node;
    }
    ok = ok && require(found, "unicast reaches the sender") &&
         require(decoded.payload == reply.payload &&
                     decoded.type == reply.type &&
                     decoded.timestamp == reply.timestamp,
                 "largest frame round-trips intact");
    if (!ok) std::fprintf(stderr, "  (%s)\n", label);
    return ok;
}

// Two Reliable endpoints over two loopback links, frames delivered in order.
bool TestReliableOverLinks(uint16_t base_port) {
    mpnet::UdpLinkConfig c0;
    c0.base_port = base_port;
    mpnet::UdpLinkConfig c1 = c0;
    c1.instance = 1;
    auto l0 = mpnet::MakeUdpLink(c0);
    auto l1 = mpnet::MakeUdpLink(c1);
    if (!require(l0->Open() && l1->Open(), "links open")) return false;
    DrainAll(*l0);
    DrainAll(*l1);

    mpnet::Reliable r0(0x1000), r1(0x2000);
    r0.AddPeer(0x2000);
    r1.AddPeer(0x1000);
    const PeerAddr a0{kLoopbackIp, base_port};
    const PeerAddr a1{kLoopbackIp, static_cast<uint16_t>(base_port + 1)};
    auto send0 = [&](uint32_t, const uint8_t* d, size_t n) {
        l0->SendTo(a1, d, n);
    };
    auto send1 = [&](uint32_t, const uint8_t* d, size_t n) {
        l1->SendTo(a0, d, n);
    };

    std::vector<uint32_t> got;
    std::vector<Frame> ready;
    auto pump = [&](mpnet::MpLink& link, mpnet::Reliable& rel,
                    const mpnet::Reliable::SendFn& send, bool collect) {
        uint8_t buf[mpnet::kMaxDatagramBytes];
        PeerAddr from;
        int n;
        const auto now = std::chrono::steady_clock::now();
        while ((n = link.Recv(buf, sizeof(buf), &from)) > 0) {
            Frame f;
            if (mpnet::Decode(buf, static_cast<size_t>(n), &f))
                rel.Receive(std::move(f), now, &ready);
        }
        rel.Service(now, send, &ready);
        if (collect) {
            for (const Frame& f : ready) {
                uint32_t v = 0;
                std::memcpy(&v, f.payload.data(), 4);
                got.push_back(v);
            }
        }
        ready.clear();
    };

    const auto t0 = std::chrono::steady_clock::now();
    r0.SendControl(0x2000, mpnet::kFlagHello, t0, send0);
    r1.SendControl(0x1000, mpnet::kFlagHello, t0, send1);
    constexpr uint32_t kFrames = 300;
    for (uint32_t i = 0; i < kFrames; ++i) {
        Frame f;
        f.type = 1;
        f.payload.resize(16);
        std::memcpy(f.payload.data(), &i, 4);
        r0.Send(&f, true, std::chrono::steady_clock::now(), send0);
        pump(*l1, r1, send1, true);
        pump(*l0, r0, send0, false);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (got.size() < kFrames &&
           std::chrono::steady_clock::now() < deadline) {
        l1->WaitReadable(5000);
        pump(*l1, r1, send1, true);
        pump(*l0, r0, send0, false);
    }
    bool in_order = got.size() == kFrames;
    for (size_t i = 0; in_order && i < got.size(); ++i)
        in_order = got[i] == i;
    if (!in_order)
        std::fprintf(stderr, "  got %zu of %u frames\n", got.size(),
                     kFrames);
    return require(in_order, "reliable frames over loopback, in order");
}

bool TestParsePeerAddress() {
    PeerAddr addr;
    std::string error;
    bool ok = require(mpnet::ParsePeerAddress("127.0.0.1:5000", 26710, &addr,
                                              &error) &&
                          addr.ip == kLoopbackIp && addr.port == 5000,
                      "host:port");
    ok = ok && require(mpnet::ParsePeerAddress("192.168.1.20", 26710, &addr,
                                               &error) &&
                           addr.ip == 0xC0A80114u && addr.port == 26710,
                       "host alone takes the base port");
    ok = ok && require(mpnet::ParsePeerAddress("localhost:26711", 26710,
                                               &addr, &error) &&
                           addr.ip == kLoopbackIp && addr.port == 26711,
                       "host name resolves");
    ok = ok && require(!mpnet::ParsePeerAddress("127.0.0.1:0", 26710, &addr,
                                                &error),
                       "port 0 rejected") &&
         require(!mpnet::ParsePeerAddress("127.0.0.1:x", 26710, &addr,
                                          &error),
                 "bad port rejected") &&
         require(!mpnet::ParsePeerAddress(":5000", 26710, &addr, &error),
                 "missing host rejected");
    ok = ok && require(mpnet::FormatPeerAddress(PeerAddr{0xC0A80114u, 80}) ==
                           "192.168.1.20:80",
                       "format");
    return ok;
}

}  // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "FAIL: WSAStartup\n");
        return 1;
    }
#endif
    // Ports away from the runner's default 26710.
    bool ok = TestParsePeerAddress();
    ok = TestDiscoveryAndUnicast(false, 41710) && ok;
    ok = TestDiscoveryAndUnicast(true, 41730) && ok;
    ok = TestReliableOverLinks(41750) && ok;
#ifdef _WIN32
    WSACleanup();
#endif
    if (!ok) return 1;
    std::printf("mp_link_test: ok\n");
    return 0;
}
