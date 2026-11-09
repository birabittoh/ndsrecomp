// mp_wire.h -- datagram format of the local wireless (NiFi) transport.
//
// One datagram carries one radio frame the guest transmitted (a beacon or
// other packet, an MP cmd, reply or ack), or a control message of the
// transport itself (hello, standalone ack). Every datagram also carries the
// sender's acknowledgement of the receiver's stream, see mp_reliable.h.
//
// Layout, little-endian:
//   0  magic 'NIF2'          28 sent_ns hi
//   4  node                  32 seq
//   8  type                  36 next_seq
//  12  len                   40 ack_node
//  16  guest timestamp u64   44 ack_next
//  24  sent_ns lo            48 ack_bits
//                            52 flags
//  56  payload (len bytes)

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpnet {

constexpr uint32_t kMagic = 0x3246494Eu;  // "NIF2"
constexpr size_t kHeaderBytes = 56;
// melonDS's largest TX buffer (12-byte TX header + 802.11 frame).
constexpr size_t kMaxPayloadBytes = 0x948;
constexpr size_t kMaxDatagramBytes = kHeaderBytes + kMaxPayloadBytes;

enum : uint32_t {
    kFlagHello = 1u << 0,    // discovery and keepalive, no payload
    kFlagAckOnly = 1u << 1,  // standalone acknowledgement, no payload
};

// A peer as the link addresses it: IPv4 address and UDP port, host order.
struct PeerAddr {
    uint32_t ip = 0;
    uint16_t port = 0;
};

inline bool operator==(const PeerAddr& a, const PeerAddr& b) {
    return a.ip == b.ip && a.port == b.port;
}
inline bool operator!=(const PeerAddr& a, const PeerAddr& b) {
    return !(a == b);
}

struct Frame {
    // The sender's random id for this transport session. Instances on
    // different machines may share an instance index, never a node id.
    uint32_t node = 0;
    // Low 16 bits: 0 packet/beacon, 1 MP cmd, 2 MP reply (AID in the high
    // 16 bits), 3 MP ack.
    uint32_t type = 0;
    uint64_t timestamp = 0;  // guest timestamp at TX
    // Sender's steady_clock at send, in ns: the latency lab counts its
    // delay from here. Rewritten on every retransmission.
    int64_t sent_ns = 0;
    uint32_t seq = 0;       // reliable sequence number; 0 = unreliable
    uint32_t next_seq = 0;  // the sender's next sequence number at send
    uint32_t ack_node = 0;  // whose stream ack_next acknowledges; 0 = none
    uint32_t ack_next = 0;  // next seq the sender expects from ack_node
    // Selective ack: bit i = ack_next + 1 + i already received (held
    // behind the gap at ack_next), so it needs no retransmission.
    uint32_t ack_bits = 0;
    uint32_t flags = 0;
    PeerAddr from{};        // filled by the receiver, not on the wire
    std::vector<uint8_t> payload;
};

void Encode(const Frame& frame, std::vector<uint8_t>* out);

// False for anything that is not a well-formed datagram of this format.
bool Decode(const uint8_t* data, size_t len, Frame* out);

// Rewrites the per-send fields of an encoded datagram in place.
void Restamp(uint8_t* datagram, int64_t sent_ns, uint32_t ack_node,
             uint32_t ack_next, uint32_t ack_bits);

// Beacons repeat ten times a second and only the latest matters, so they
// are the one radio frame the transport may lose.
bool IsBeacon(uint32_t type, const uint8_t* payload, size_t len);

// Wraparound-safe sequence order.
inline bool SeqBefore(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

}  // namespace mpnet
