// mp_wire.cpp -- see mp_wire.h.

#include "net/mp_wire.h"

#include <utility>

namespace mpnet {

namespace {

// The wire flags byte.
enum : uint8_t {
    kWireHello = 1u << 0,    // = kFlagHello
    kWireAckOnly = 1u << 1,  // = kFlagAckOnly
    kWirePacked = 1u << 2,   // payload zero-run packed
    kWireRadio = 1u << 3,    // payload addresses packed
    kWireAck = 1u << 4,      // ack_node, ack_next, ack_bits follow
    kWireSentNs = 1u << 5,   // sent_ns follows
    kWireKnown = 0x3Fu,
};

constexpr uint8_t kTypeEscape = 0xFF;
constexpr size_t kRadioAddrAt = 16;  // TX header 12 + frame control + duration
constexpr size_t kRadioAddrEnd = kRadioAddrAt + 18;
constexpr uint8_t kNintendoMulticast[5] = {0x03, 0x09, 0xBF, 0x00, 0x00};

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

void PutLe32(std::vector<uint8_t>* out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void PutVar(std::vector<uint8_t>* out, uint64_t value) {
    while (value >= 0x80u) {
        out->push_back(static_cast<uint8_t>(value | 0x80u));
        value >>= 7u;
    }
    out->push_back(static_cast<uint8_t>(value));
}

void PutZvar(std::vector<uint8_t>* out, int64_t value) {
    PutVar(out, (static_cast<uint64_t>(value) << 1u) ^
                    static_cast<uint64_t>(value >> 63));
}

// Bounds-checked reader over one datagram.
struct Reader {
    const uint8_t* data;
    size_t len;
    size_t at;

    bool Byte(uint8_t* v) {
        if (at >= len) return false;
        *v = data[at++];
        return true;
    }
    bool Le32(uint32_t* v) {
        if (len - at < 4u) return false;
        *v = ReadLe32(data + at);
        at += 4;
        return true;
    }
    bool Le64(uint64_t* v) {
        uint32_t lo = 0, hi = 0;
        if (!Le32(&lo) || !Le32(&hi)) return false;
        *v = lo | (static_cast<uint64_t>(hi) << 32u);
        return true;
    }
    bool Var(uint64_t* v) {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            uint8_t b = 0;
            if (!Byte(&b)) return false;
            value |= static_cast<uint64_t>(b & 0x7Fu) << shift;
            if ((b & 0x80u) == 0) {
                *v = value;
                return true;
            }
        }
        return false;
    }
    bool Var32(uint32_t* v) {
        uint64_t value = 0;
        if (!Var(&value) || value > 0xFFFFFFFFull) return false;
        *v = static_cast<uint32_t>(value);
        return true;
    }
    bool Zvar(int64_t* v) {
        uint64_t value = 0;
        if (!Var(&value)) return false;
        *v = static_cast<int64_t>(value >> 1u) ^ -static_cast<int64_t>(value & 1u);
        return true;
    }
    bool Type(uint32_t* v) {
        uint8_t b = 0;
        if (!Byte(&b)) return false;
        if (b == kTypeEscape) return Le32(v);
        *v = (b & 0x0Fu) | (static_cast<uint32_t>(b >> 4u) << 16u);
        return true;
    }
};

void PutType(std::vector<uint8_t>* out, uint32_t type) {
    const uint32_t low = type & 0xFFFFu;
    const uint32_t aid = type >> 16u;
    if (low < 15u && aid < 16u) {
        out->push_back(static_cast<uint8_t>(low | (aid << 4u)));
    } else {
        out->push_back(kTypeEscape);
        PutLe32(out, type);
    }
}

// Everything before the payload.
struct Header {
    uint8_t wire = 0;
    uint32_t node = 0;
    uint32_t type = 0;
    uint32_t body_len = 0;
    uint64_t timestamp = 0;
    uint32_t seq = 0;
    uint32_t next_seq = 0;
    uint32_t ack_node = 0;
    uint32_t ack_next = 0;
    uint32_t ack_bits = 0;
    int64_t sent_ns = 0;
    size_t body_at = 0;
};

void PutHeader(std::vector<uint8_t>* out, Header h) {
    h.wire &= static_cast<uint8_t>(~(kWireAck | kWireSentNs));
    if (h.ack_node || h.ack_next || h.ack_bits) h.wire |= kWireAck;
    if (h.sent_ns != 0) h.wire |= kWireSentNs;
    out->push_back(kMagic0);
    out->push_back(kMagic1);
    out->push_back(h.wire);
    PutLe32(out, h.node);
    PutType(out, h.type);
    PutVar(out, h.body_len);
    PutVar(out, h.timestamp);
    PutVar(out, h.seq);
    PutVar(out, static_cast<uint32_t>(h.next_seq - h.seq));
    if (h.wire & kWireAck) {
        PutLe32(out, h.ack_node);
        PutVar(out, h.ack_next);
        PutVar(out, h.ack_bits);
    }
    if (h.wire & kWireSentNs) {
        const uint64_t ns = static_cast<uint64_t>(h.sent_ns);
        PutLe32(out, static_cast<uint32_t>(ns));
        PutLe32(out, static_cast<uint32_t>(ns >> 32u));
    }
}

bool ParseHeader(const uint8_t* data, size_t len, Header* h) {
    if (!data || len < 3u || data[0] != kMagic0 || data[1] != kMagic1)
        return false;
    Reader r{data, len, 2};
    uint32_t delta = 0;
    uint64_t sent = 0;
    if (!r.Byte(&h->wire) || (h->wire & ~kWireKnown) != 0 ||
        !r.Le32(&h->node) || !r.Type(&h->type) || !r.Var32(&h->body_len) ||
        !r.Var(&h->timestamp) || !r.Var32(&h->seq) || !r.Var32(&delta))
        return false;
    h->next_seq = h->seq + delta;
    h->ack_node = h->ack_next = h->ack_bits = 0;
    if ((h->wire & kWireAck) &&
        (!r.Le32(&h->ack_node) || !r.Var32(&h->ack_next) ||
         !r.Var32(&h->ack_bits)))
        return false;
    h->sent_ns = 0;
    if (h->wire & kWireSentNs) {
        if (!r.Le64(&sent)) return false;
        h->sent_ns = static_cast<int64_t>(sent);
    }
    if (h->body_len > len - r.at) return false;
    h->body_at = r.at;
    return true;
}

// The payload in its shortest form, and the forms used.
void EncodeBody(const std::vector<uint8_t>& payload,
                std::vector<uint8_t>* body, bool* radio, bool* packed) {
    std::vector<uint8_t> addressed;
    *radio = PackRadio(payload, &addressed);
    const std::vector<uint8_t>& cur = *radio ? addressed : payload;
    std::vector<uint8_t> zeros;
    PackZeros(cur.data(), cur.size(), &zeros);
    *packed = zeros.size() < cur.size();
    *body = *packed ? std::move(zeros) : cur;
}

bool DecodeBody(const uint8_t* data, size_t len, bool radio, bool packed,
                std::vector<uint8_t>* out) {
    std::vector<uint8_t> step;
    if (packed) {
        if (!UnpackZeros(data, len, kMaxPayloadBytes + 1, &step)) return false;
    } else {
        step.assign(data, data + len);
    }
    if (radio) return UnpackRadio(step, kMaxPayloadBytes, out);
    if (step.size() > kMaxPayloadBytes) return false;
    *out = std::move(step);
    return true;
}

}  // namespace

void PackZeros(const uint8_t* data, size_t len, std::vector<uint8_t>* out) {
    out->clear();
    out->reserve(len);
    for (size_t i = 0; i < len;) {
        if (data[i] != 0) {
            out->push_back(data[i++]);
            continue;
        }
        size_t run = 0;
        while (i < len && data[i] == 0 && run < 255) {
            ++i;
            ++run;
        }
        out->push_back(0);
        out->push_back(static_cast<uint8_t>(run));
    }
}

bool UnpackZeros(const uint8_t* data, size_t len, size_t max_len,
                 std::vector<uint8_t>* out) {
    out->clear();
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != 0) {
            if (out->size() >= max_len) return false;
            out->push_back(data[i]);
            continue;
        }
        if (++i == len || data[i] == 0) return false;
        if (out->size() + data[i] > max_len) return false;
        out->insert(out->end(), data[i], 0);
    }
    return true;
}

bool PackRadio(const std::vector<uint8_t>& payload, std::vector<uint8_t>* out) {
    if (payload.size() < kRadioAddrEnd) return false;
    const uint8_t* a[3] = {payload.data() + kRadioAddrAt,
                           payload.data() + kRadioAddrAt + 6,
                           payload.data() + kRadioAddrAt + 12};
    const auto same = [](const uint8_t* x, const uint8_t* y) {
        for (int i = 0; i < 6; ++i)
            if (x[i] != y[i]) return false;
        return true;
    };
    const auto multicast = [](const uint8_t* x) {
        for (int i = 0; i < 5; ++i)
            if (x[i] != kNintendoMulticast[i]) return false;
        return true;
    };
    uint8_t codes[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (i >= 1 && same(a[i], a[0])) codes[i] = 2;
        else if (i == 2 && same(a[i], a[1])) codes[i] = 3;
        else if (multicast(a[i])) codes[i] = 1;
    }
    if (codes[0] == 0 && codes[1] == 0 && codes[2] == 0) return false;
    out->assign(payload.begin(), payload.begin() + kRadioAddrAt);
    out->push_back(static_cast<uint8_t>(codes[0] | (codes[1] << 2u) |
                                        (codes[2] << 4u)));
    for (int i = 0; i < 3; ++i) {
        if (codes[i] == 0) out->insert(out->end(), a[i], a[i] + 6);
        else if (codes[i] == 1) out->push_back(a[i][5]);
    }
    out->insert(out->end(), payload.begin() + kRadioAddrEnd, payload.end());
    return true;
}

bool UnpackRadio(const std::vector<uint8_t>& packed, size_t max_len,
                 std::vector<uint8_t>* out) {
    if (packed.size() < kRadioAddrAt + 1u) return false;
    const uint8_t codes = packed[kRadioAddrAt];
    if (codes >> 6u) return false;
    out->assign(packed.begin(), packed.begin() + kRadioAddrAt);
    size_t at = kRadioAddrAt + 1u;
    for (int i = 0; i < 3; ++i) {
        const uint8_t code = (codes >> (2 * i)) & 3u;
        if ((code == 2 && i == 0) || (code == 3 && i != 2)) return false;
        if (code == 0) {
            if (packed.size() - at < 6u) return false;
            out->insert(out->end(), packed.begin() + at, packed.begin() + at + 6);
            at += 6;
        } else if (code == 1) {
            if (at >= packed.size()) return false;
            out->insert(out->end(), kNintendoMulticast, kNintendoMulticast + 5);
            out->push_back(packed[at++]);
        } else {
            const size_t from = kRadioAddrAt + (code == 2 ? 0u : 6u);
            for (size_t k = 0; k < 6; ++k) {
                const uint8_t v = (*out)[from + k];
                out->push_back(v);
            }
        }
    }
    if (out->size() + (packed.size() - at) > max_len) return false;
    out->insert(out->end(), packed.begin() + at, packed.end());
    return true;
}

void Encode(const Frame& frame, std::vector<uint8_t>* out) {
    std::vector<uint8_t> body;
    bool radio = false, packed = false;
    EncodeBody(frame.payload, &body, &radio, &packed);
    Header h;
    h.wire = static_cast<uint8_t>(frame.flags & (kFlagHello | kFlagAckOnly));
    if (packed) h.wire |= kWirePacked;
    if (radio) h.wire |= kWireRadio;
    h.node = frame.node;
    h.type = frame.type;
    h.body_len = static_cast<uint32_t>(body.size());
    h.timestamp = frame.timestamp;
    h.seq = frame.seq;
    h.next_seq = frame.next_seq;
    h.ack_node = frame.ack_node;
    h.ack_next = frame.ack_next;
    h.ack_bits = frame.ack_bits;
    h.sent_ns = frame.sent_ns;
    out->clear();
    out->reserve(kMaxHeaderBytes + body.size());
    PutHeader(out, h);
    out->insert(out->end(), body.begin(), body.end());
}

bool Decode(const uint8_t* data, size_t len, Frame* out) {
    if (len > kMaxDatagramBytes) return false;
    Header h;
    if (!ParseHeader(data, len, &h) || h.node == 0) return false;
    if (!DecodeBody(data + h.body_at, h.body_len, (h.wire & kWireRadio) != 0,
                    (h.wire & kWirePacked) != 0, &out->payload))
        return false;
    out->node = h.node;
    out->type = h.type;
    out->timestamp = h.timestamp;
    out->sent_ns = h.sent_ns;
    out->seq = h.seq;
    out->next_seq = h.next_seq;
    out->ack_node = h.ack_node;
    out->ack_next = h.ack_next;
    out->ack_bits = h.ack_bits;
    out->flags = h.wire & (kWireHello | kWireAckOnly);
    out->bundled.clear();
    Reader r{data, len, h.body_at + h.body_len};
    while (r.at != len) {
        Frame b;
        uint32_t back = 0;
        int64_t dts = 0;
        uint64_t word = 0;
        if (!r.Var32(&back) || back == 0 || !r.Type(&b.type) ||
            !r.Zvar(&dts) || !r.Var(&word))
            return false;
        const uint64_t blen = word >> 2u;
        if (blen > len - r.at) return false;
        b.node = h.node;
        b.seq = h.next_seq - back;
        if (b.seq == 0) return false;
        b.timestamp = h.timestamp + static_cast<uint64_t>(dts);
        b.sent_ns = h.sent_ns;
        if (!DecodeBody(data + r.at, static_cast<size_t>(blen),
                        (word & 2u) != 0, (word & 1u) != 0, &b.payload))
            return false;
        r.at += static_cast<size_t>(blen);
        out->bundled.push_back(std::move(b));
    }
    return true;
}

bool AppendBundled(std::vector<uint8_t>* out,
                   const std::vector<uint8_t>& source) {
    Header main, src;
    if (!ParseHeader(out->data(), out->size(), &main) ||
        !ParseHeader(source.data(), source.size(), &src) || src.seq == 0 ||
        !SeqBefore(src.seq, main.next_seq))
        return false;
    std::vector<uint8_t> entry;
    PutVar(&entry, static_cast<uint32_t>(main.next_seq - src.seq));
    PutType(&entry, src.type);
    PutZvar(&entry, static_cast<int64_t>(src.timestamp - main.timestamp));
    PutVar(&entry, (static_cast<uint64_t>(src.body_len) << 2u) |
                       ((src.wire & kWireRadio) ? 2u : 0u) |
                       ((src.wire & kWirePacked) ? 1u : 0u));
    if (out->size() + entry.size() + src.body_len > kBundleLimitBytes)
        return false;
    out->insert(out->end(), entry.begin(), entry.end());
    out->insert(out->end(), source.begin() + src.body_at,
                source.begin() + src.body_at + src.body_len);
    return true;
}

size_t BundleCost(const std::vector<uint8_t>& source) {
    Header h;
    if (!ParseHeader(source.data(), source.size(), &h)) return source.size();
    // Sequence back-step, type, timestamp step and length: a few bytes.
    return h.body_len + 8u;
}

void Restamp(std::vector<uint8_t>* datagram, int64_t sent_ns,
             uint32_t ack_node, uint32_t ack_next, uint32_t ack_bits) {
    Header h;
    if (!ParseHeader(datagram->data(), datagram->size(), &h)) return;
    h.sent_ns = sent_ns;
    h.ack_node = ack_node;
    h.ack_next = ack_next;
    h.ack_bits = ack_bits;
    std::vector<uint8_t> out;
    out.reserve(datagram->size() + 16u);
    PutHeader(&out, h);
    out.insert(out.end(), datagram->begin() + h.body_at, datagram->end());
    *datagram = std::move(out);
}

bool IsBeacon(uint32_t type, const uint8_t* payload, size_t len) {
    // Payload = melonDS TX header (12 bytes) + 802.11 frame; frame control
    // at 12. Management (type 0) subtype 8 = beacon.
    if ((type & 0xFFFFu) != 0u || len < 12u + 2u || !payload) return false;
    return (payload[12] & 0xFCu) == 0x80u;
}

}  // namespace mpnet
