// mp_beacon.cpp -- see mp_beacon.h.

#include "net/mp_beacon.h"

#include <cstring>

namespace mpnet {

namespace {

constexpr size_t kTxHeader = 12;
constexpr size_t kMacHeader = 24;
constexpr size_t kFixedFields = 12;  // timestamp, interval, capability
constexpr size_t kGameInfoOffset = 8;  // in the Nintendo element
constexpr size_t kGameInfoFixed = 16;  // WMGameInfo up to userGameInfo

uint16_t Le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

bool SameGameInfo(const GameInfo& a, const GameInfo& b) {
    return a.beacon_interval == b.beacon_interval && a.ggid == b.ggid &&
           a.tgid == b.tgid && a.attribute == b.attribute &&
           a.parent_max_size == b.parent_max_size &&
           a.child_max_size == b.child_max_size && a.user_len == b.user_len &&
           std::memcmp(a.user, b.user, a.user_len) == 0;
}

bool ParseBeaconGameInfo(const uint8_t* payload, size_t len, GameInfo* out) {
    if (!payload || !out) return false;
    const size_t body = kTxHeader + kMacHeader;
    if (len < body + kFixedFields) return false;
    if ((payload[kTxHeader] & 0xFCu) != 0x80u) return false;  // beacon
    // The TX header's length field counts the 802.11 frame with its FCS.
    const size_t frame_len = Le16(payload + 10);
    size_t end = len;
    if (frame_len >= kMacHeader + 4 && kTxHeader + frame_len - 4 < end)
        end = kTxHeader + frame_len - 4;

    GameInfo info;
    info.beacon_interval = Le16(payload + body + 8);
    size_t pos = body + kFixedFields;
    while (pos + 2 <= end) {
        const uint8_t id = payload[pos];
        const size_t elen = payload[pos + 1];
        const uint8_t* e = payload + pos + 2;
        if (pos + 2 + elen > end) return false;
        pos += 2 + elen;
        if (id != 0xDD || elen < kGameInfoOffset + kGameInfoFixed) continue;
        if (e[0] != 0x00 || e[1] != 0x09 || e[2] != 0xBF || e[3] != 0x00)
            continue;
        const uint8_t* g = e + kGameInfoOffset;
        if (Le16(g) != 0x0001) return false;
        info.ggid = Le32(g + 4);
        info.tgid = Le16(g + 8);
        info.user_len = g[10];
        info.attribute = g[11];
        info.parent_max_size = Le16(g + 12);
        info.child_max_size = Le16(g + 14);
        if (info.user_len > kMaxUserGameInfo ||
            kGameInfoOffset + kGameInfoFixed + info.user_len > elen)
            return false;
        std::memcpy(info.user, g + kGameInfoFixed, info.user_len);
        *out = info;
        return true;
    }
    return false;
}

BeaconWatch::Event BeaconWatch::OnBeacon(const GameInfo& info, Time now) {
    last_ = now;
    ++count_;
    if (!active_) {
        active_ = true;
        count_ = 1;
        info_ = info;
        return Event::kStarted;
    }
    if (SameGameInfo(info, info_)) return Event::kNone;
    info_ = info;
    return Event::kChanged;
}

BeaconWatch::Event BeaconWatch::Poll(Time now) {
    if (!active_ || now - last_ < kSilence) return Event::kNone;
    active_ = false;
    return Event::kStopped;
}

}  // namespace mpnet
