// mp_link.h -- how local wireless datagrams reach other runners.
//
// The transport (LocalMpTransport in wifi_net.cpp) owns the radio-level
// logic, the peer table and reliability (mp_reliable.h); a link only moves
// datagrams. Peers are found with discovery datagrams (hellos) and then
// addressed one by one with the address their datagrams came from.
//
// UdpMpLink, two modes:
//   loopback: bound to 127.0.0.1:(base_port + instance). Discovery goes to
//             the other 15 instance ports on this machine.
//   lan:      bound to 0.0.0.0:(base_port + instance). Discovery also goes
//             to the subnet broadcast address of every Ethernet / Wi-Fi
//             network this machine is on, on all 16 instance ports, and
//             to the configured peers (for networks that drop broadcast,
//             and for peers behind a VPN).
//             Other instances on this machine are found as in loopback.
//
// Not thread-safe: the transport calls it from the emulation thread.

#pragma once

#include "net/mp_wire.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mpnet {

class MpLink {
public:
    virtual ~MpLink() = default;

    virtual bool Open() = 0;
    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;

    virtual bool SendTo(const PeerAddr& to, const uint8_t* data,
                        size_t len) = 0;
    // To every address a peer might be listening on.
    virtual void SendDiscovery(const uint8_t* data, size_t len) = 0;
    // > 0: a datagram of that length, from `from`. 0: nothing pending.
    // < 0: socket error (logged).
    virtual int Recv(uint8_t* data, size_t capacity, PeerAddr* from) = 0;
    // True when a datagram arrived within `timeout_us`.
    virtual bool WaitReadable(int64_t timeout_us) = 0;

    virtual std::string Describe() const = 0;
};

struct UdpLinkConfig {
    bool lan = false;
    uint32_t instance = 0;
    uint16_t base_port = 26710;
    // Extra discovery targets (lan mode), e.g. from --local-wireless-peer.
    std::vector<PeerAddr> peers;
};

constexpr uint32_t kMaxInstances = 16;

std::unique_ptr<MpLink> MakeUdpLink(const UdpLinkConfig& config);

// "host:port" or "host" (port = base_port) -> IPv4 address. Resolves host
// names. Needs Winsock started on Windows.
bool ParsePeerAddress(const std::string& text, uint16_t default_port,
                      PeerAddr* out, std::string* error);

std::string FormatPeerAddress(const PeerAddr& addr);

}  // namespace mpnet
