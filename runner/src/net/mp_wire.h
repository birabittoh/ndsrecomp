// mp_wire.h -- datagram format of the local wireless (NiFi) transport.
//
// One datagram carries one radio frame the guest transmitted (a beacon or
// other packet, an MP cmd, reply or ack), or a control message of the
// transport itself (hello, standalone ack). Every datagram also carries the
// sender's acknowledgement of the receiver's stream, see mp_reliable.h.
//
// Online most frames are small (an MP cmd with no data, an ack: 46 bytes),
// sent sixty times a second each way, so the header is kept short.
// Layout ("var" = unsigned LEB128, "zvar" = zigzag LEB128):
//   magic 'N' '5'
//   wire flags u8 (kWire* below)
//   node u32
//   type: u8 (low type | AID << 4), or 0xFF and the u32
//   payload length as sent, var
//   guest timestamp, var
//   seq, var; next_seq - seq, var
//   with kWireAck: ack_node u32, ack_next var, ack_bits var
//   with kWireSentNs: sent_ns u64 (only while the latency lab is on)
//   payload
//   then zero or more bundled frames, to the end of the datagram: earlier
//   reliable frames of the same sender that the receiver has not
//   acknowledged yet, sent again so that a lost datagram costs no round
//   trip (mp_reliable.h). Each: next_seq - seq (var), type (as above),
//   timestamp - the datagram's (zvar), length << 2 | radio << 1 | packed
//   (var), payload.
//
// A payload is sent in the shortest of these forms, each undone in reverse:
//   radio: the three 802.11 addresses (payload 16..33, after the 12-byte TX
//     header, frame control and duration) as one byte of 2-bit codes and
//     what each code needs: 0 the 6 bytes, 1 the last byte of a Nintendo MP
//     multicast address (03:09:BF:00:00:xx), 2 same as address 1, 3 same as
//     address 2. MP cmds and acks name the host twice and a multicast once.
//   packed: every run of zero bytes becomes a 0 and the run's length
//     (1-255). Radio frames are mostly zeros (an MP dataset of a two-player
//     game: the two empty member blocks, the unused message slots).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpnet {

constexpr uint8_t kMagic0 = 'N';
constexpr uint8_t kMagic1 = '5';
// Longest header Encode writes (every field at its widest).
constexpr size_t kMaxHeaderBytes = 2 + 1 + 4 + 5 + 3 + 10 + 5 + 5 + 4 + 5 +
                                   5 + 8;
// melonDS's largest TX buffer (12-byte TX header + 802.11 frame).
constexpr size_t kMaxPayloadBytes = 0x948;
constexpr size_t kMaxDatagramBytes = kMaxHeaderBytes + kMaxPayloadBytes + 1;
// Frames are bundled only while the datagram stays below this, so the
// redundancy never makes a datagram fragment on the way (a TURN relay
// path carries about 1200 bytes whole).
constexpr size_t kBundleLimitBytes = 1100;

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
    // Sender's steady_clock at send, in ns, or 0: the latency lab counts its
    // delay from here. Rewritten on every retransmission.
    int64_t sent_ns = 0;
    uint32_t seq = 0;       // reliable sequence number; 0 = unreliable
    uint32_t next_seq = 0;  // the sender's next sequence number at send
    uint32_t ack_node = 0;  // whose stream ack_next acknowledges; 0 = none
    uint32_t ack_next = 0;  // next seq the sender expects from ack_node
    // Selective ack: bit i = ack_next + 1 + i already received (held
    // behind the gap at ack_next), so it needs no retransmission.
    uint32_t ack_bits = 0;
    uint32_t flags = 0;     // kFlagHello / kFlagAckOnly
    PeerAddr from{};        // filled by the receiver, not on the wire
    std::vector<uint8_t> payload;
    // Bundled earlier frames (node, type, timestamp, seq, payload set),
    // oldest first. Filled by Decode only; see AppendBundled.
    std::vector<Frame> bundled;
};

void Encode(const Frame& frame, std::vector<uint8_t>* out);

// False for anything that is not a well-formed datagram of this format.
bool Decode(const uint8_t* data, size_t len, Frame* out);

// Appends the frame of the encoded datagram `source` (not what it bundles)
// to the encoded datagram `out` as a bundled frame. False, and `out`
// untouched, if that would pass kBundleLimitBytes or `source` is not a
// reliable frame `out` can carry (seq 0, or not before out's next_seq).
bool AppendBundled(std::vector<uint8_t>* out,
                   const std::vector<uint8_t>& source);

// Bytes AppendBundled adds for `source`, at most.
size_t BundleCost(const std::vector<uint8_t>& source);

// Rewrites the per-send fields of an encoded datagram.
void Restamp(std::vector<uint8_t>* datagram, int64_t sent_ns,
             uint32_t ack_node, uint32_t ack_next, uint32_t ack_bits);

// The zero-run packing of a payload (see the layout above). Unpack is false
// for a malformed packing or one that unpacks past `max_len`.
void PackZeros(const uint8_t* data, size_t len, std::vector<uint8_t>* out);
bool UnpackZeros(const uint8_t* data, size_t len, size_t max_len,
                 std::vector<uint8_t>* out);

// The address packing of a radio frame (see the layout above). Pack is
// false when it would not save anything; Unpack false when malformed.
bool PackRadio(const std::vector<uint8_t>& payload, std::vector<uint8_t>* out);
bool UnpackRadio(const std::vector<uint8_t>& packed, size_t max_len,
                 std::vector<uint8_t>* out);

// Beacons repeat ten times a second and only the latest matters, so they
// are the one radio frame the transport may lose.
bool IsBeacon(uint32_t type, const uint8_t* payload, size_t len);

// Wraparound-safe sequence order.
inline bool SeqBefore(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
}

}  // namespace mpnet
