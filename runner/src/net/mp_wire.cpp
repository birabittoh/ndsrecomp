// mp_wire.cpp -- see mp_wire.h.

#include "net/mp_wire.h"

#include <utility>

namespace mpnet {

namespace {

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

uint64_t ReadLe64(const uint8_t* p) {
    return static_cast<uint64_t>(ReadLe32(p)) |
           (static_cast<uint64_t>(ReadLe32(p + 4u)) << 32u);
}

void PutLe32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8u);
    p[2] = static_cast<uint8_t>(value >> 16u);
    p[3] = static_cast<uint8_t>(value >> 24u);
}

void PutLe64(uint8_t* p, uint64_t value) {
    PutLe32(p, static_cast<uint32_t>(value));
    PutLe32(p + 4u, static_cast<uint32_t>(value >> 32u));
}

}  // namespace

void Encode(const Frame& frame, std::vector<uint8_t>* out) {
    out->resize(kHeaderBytes + frame.payload.size());
    uint8_t* p = out->data();
    PutLe32(p + 0, kMagic);
    PutLe32(p + 4, frame.node);
    PutLe32(p + 8, frame.type);
    PutLe32(p + 12, static_cast<uint32_t>(frame.payload.size()));
    PutLe64(p + 16, frame.timestamp);
    PutLe64(p + 24, static_cast<uint64_t>(frame.sent_ns));
    PutLe32(p + 32, frame.seq);
    PutLe32(p + 36, frame.next_seq);
    PutLe32(p + 40, frame.ack_node);
    PutLe32(p + 44, frame.ack_next);
    PutLe32(p + 48, frame.ack_bits);
    PutLe32(p + 52, frame.flags);
    for (size_t i = 0; i < frame.payload.size(); ++i)
        p[kHeaderBytes + i] = frame.payload[i];
}

bool Decode(const uint8_t* data, size_t len, Frame* out) {
    if (!data || len < kHeaderBytes) return false;
    if (ReadLe32(data) != kMagic) return false;
    const uint32_t payload_len = ReadLe32(data + 12);
    if (payload_len > kMaxPayloadBytes) return false;
    if (kHeaderBytes + payload_len > len) return false;
    if (len > kMaxDatagramBytes) return false;
    out->node = ReadLe32(data + 4);
    if (out->node == 0) return false;
    out->type = ReadLe32(data + 8);
    out->timestamp = ReadLe64(data + 16);
    out->sent_ns = static_cast<int64_t>(ReadLe64(data + 24));
    out->seq = ReadLe32(data + 32);
    out->next_seq = ReadLe32(data + 36);
    out->ack_node = ReadLe32(data + 40);
    out->ack_next = ReadLe32(data + 44);
    out->ack_bits = ReadLe32(data + 48);
    out->flags = ReadLe32(data + 52);
    size_t at = kHeaderBytes + payload_len;
    out->payload.assign(data + kHeaderBytes, data + at);
    out->bundled.clear();
    while (at != len) {
        if (len - at < kBundleHeaderBytes) return false;
        const size_t blen = static_cast<size_t>(data[at + 16]) |
                            (static_cast<size_t>(data[at + 17]) << 8u);
        if (len - at - kBundleHeaderBytes < blen) return false;
        Frame b;
        b.node = out->node;
        b.seq = ReadLe32(data + at);
        if (b.seq == 0) return false;
        b.type = ReadLe32(data + at + 4);
        b.timestamp = ReadLe64(data + at + 8);
        b.sent_ns = out->sent_ns;
        at += kBundleHeaderBytes;
        b.payload.assign(data + at, data + at + blen);
        at += blen;
        out->bundled.push_back(std::move(b));
    }
    return true;
}

bool AppendBundled(std::vector<uint8_t>* out, const uint8_t* source) {
    const size_t blen = ReadLe32(source + 12);
    if (out->size() + kBundleHeaderBytes + blen > kBundleLimitBytes)
        return false;
    const size_t at = out->size();
    out->resize(at + kBundleHeaderBytes + blen);
    uint8_t* p = out->data() + at;
    PutLe32(p, ReadLe32(source + 32));
    PutLe32(p + 4, ReadLe32(source + 8));
    PutLe64(p + 8, ReadLe64(source + 16));
    p[16] = static_cast<uint8_t>(blen);
    p[17] = static_cast<uint8_t>(blen >> 8u);
    for (size_t i = 0; i < blen; ++i)
        p[kBundleHeaderBytes + i] = source[kHeaderBytes + i];
    return true;
}

void Restamp(uint8_t* datagram, int64_t sent_ns, uint32_t ack_node,
             uint32_t ack_next, uint32_t ack_bits) {
    PutLe64(datagram + 24, static_cast<uint64_t>(sent_ns));
    PutLe32(datagram + 40, ack_node);
    PutLe32(datagram + 44, ack_next);
    PutLe32(datagram + 48, ack_bits);
}

bool IsBeacon(uint32_t type, const uint8_t* payload, size_t len) {
    // Payload = melonDS TX header (12 bytes) + 802.11 frame; frame control
    // at 12. Management (type 0) subtype 8 = beacon.
    if ((type & 0xFFFFu) != 0u || len < 12u + 2u || !payload) return false;
    return (payload[12] & 0xFCu) == 0x80u;
}

}  // namespace mpnet
