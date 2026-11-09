// mp_beacon.h -- the game info a DS MP host advertises in its beacons.
//
// A Nitro SDK (WM) MP parent puts a WMGameInfo in a vendor element of every
// beacon it transmits. That is what other consoles list in their "join"
// menus, and it tells the runner that this instance is hosting, for which
// game (GGID), and whether it accepts new children (the entry attribute).
// The user game info is up to 112 game-defined bytes.
//
// Beacon body as the transport sees it (melonDS TX header first):
//   0  TX header (12 bytes)
//  12  802.11 header (24 bytes)
//  36  timestamp (8), beacon interval (2), capability (2)
//  48  elements; the Nintendo one is id 0xDD:
//        0  OUI 00 09 BF, type 00
//        4  stepping offset (2), LCD video sync (2)
//        8  WMGameInfo: magic 0x0001 (2), ver (1), platform (1), GGID (4),
//           TGID (2), user game info length (1), attribute (1),
//           parent max size (2), child max size (2), user game info

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace mpnet {

constexpr size_t kMaxUserGameInfo = 112;

// WMGameInfo.attribute bits.
enum : uint8_t {
    kAttrEntry = 1u << 0,       // accepts new children
    kAttrMultiboot = 1u << 1,   // download play
    kAttrKeySharing = 1u << 2,
    kAttrContinuous = 1u << 3,  // continuous MP
};

struct GameInfo {
    uint16_t beacon_interval = 0;  // TU (1024 us)
    uint32_t ggid = 0;
    uint16_t tgid = 0;
    uint8_t attribute = 0;
    uint16_t parent_max_size = 0;
    uint16_t child_max_size = 0;
    uint8_t user_len = 0;
    uint8_t user[kMaxUserGameInfo] = {};
};

bool SameGameInfo(const GameInfo& a, const GameInfo& b);

// `payload` is one transport frame of type 0 (TX header + 802.11 frame).
// False unless it is a beacon carrying a well-formed WMGameInfo.
bool ParseBeaconGameInfo(const uint8_t* payload, size_t len, GameInfo* out);

// Follows the beacons this instance transmits: hosting starts with the
// first one, ends when none was sent for kSilence. Not thread-safe.
class BeaconWatch {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    enum class Event { kNone, kStarted, kChanged, kStopped };

    // DS MP hosts beacon every ~200 ms or faster.
    static constexpr std::chrono::milliseconds kSilence{1500};

    Event OnBeacon(const GameInfo& info, Time now);
    Event Poll(Time now);

    bool active() const { return active_; }
    const GameInfo& info() const { return info_; }
    // Beacons since hosting started.
    uint64_t count() const { return count_; }

private:
    bool active_ = false;
    GameInfo info_;
    Time last_{};
    uint64_t count_ = 0;
};

}  // namespace mpnet
