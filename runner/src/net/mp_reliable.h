// mp_reliable.h -- reliable, ordered delivery for the local wireless
// transport (HANDOFF_ONLINE_LOBBY.md Phase 1 step 5).
//
// Late-reply mode answers every MP exchange locally and lets the data
// follow with the link's latency, so nobody retransmits at the radio level
// any more: a lost cmd, reply or ack datagram is lost data. This layer
// makes the link reliable underneath, independent of the link type.
//
// Each node sends one stream of frames to all its peers (the radio is a
// broadcast medium). Every frame except beacons gets a sequence number.
// Each receiver acknowledges, per sender, the next sequence number it
// expects (cumulative). The acknowledgement rides on every datagram to
// that sender, or goes standalone after kAckDelay when there is nothing to
// carry it, with a bitmap of the 32 frames after the gap that did arrive.
// The sender keeps frames until every peer has acknowledged them and
// retransmits to a peer that has neither acknowledged nor selectively
// acknowledged one after its retransmission timeout (smoothed RTT + 4 x
// deviation). The receiver releases frames in order,
// holds the ones after a gap, and drops duplicates.
//
// A receiver joins a sender's stream where it first hears it: from a
// datagram's next_seq (hello, beacon, ack), or from the first reliable
// frame. Frames before that were not for it.
//
// Not thread-safe: the transport calls it from the emulation thread.

#pragma once

#include "net/mp_wire.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

namespace mpnet {

class Reliable {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    // Hands an encoded datagram to the link, addressed to `node`.
    using SendFn =
        std::function<void(uint32_t node, const uint8_t* data, size_t len)>;

    struct Stats {
        uint64_t retransmits = 0;  // datagrams sent again
        uint64_t duplicates = 0;   // received again, dropped
        uint64_t reordered = 0;    // received after a gap, held
        uint64_t gap_skips = 0;    // gaps given up on (data lost)
        uint64_t give_ups = 0;     // sent frames dropped unacknowledged
        size_t held = 0;           // frames waiting behind a gap now
        size_t unacked = 0;        // frames kept for retransmission now
        size_t peers = 0;
    };

    // How long a receiver may wait before acknowledging on its own.
    static constexpr std::chrono::milliseconds kAckDelay{2};
    // Sent frames are kept at least this long even once acknowledged, so a
    // peer that joins the stream just after a gap can still get it.
    static constexpr std::chrono::milliseconds kRetainMin{1000};
    // A sent frame nobody acknowledges is dropped after this long.
    static constexpr std::chrono::milliseconds kGiveUpAfter{10000};
    // A receiver gives up on a gap after this long and skips it.
    static constexpr std::chrono::milliseconds kGapGiveUp{3000};
    static constexpr std::chrono::milliseconds kInitialRto{200};
    static constexpr std::chrono::milliseconds kMinRto{20};
    static constexpr std::chrono::milliseconds kMaxRto{1000};
    static constexpr size_t kMaxUnacked = 4096;
    static constexpr size_t kMaxHeld = 1024;
    static constexpr size_t kMaxRetransmitsPerService = 64;

    explicit Reliable(uint32_t self_node = 0) { Reset(self_node); }

    // Forgets every peer and starts a new stream as `self_node`.
    void Reset(uint32_t self_node);

    uint32_t self() const { return self_; }

    void AddPeer(uint32_t node);
    void RemovePeer(uint32_t node);
    bool HasPeer(uint32_t node) const { return peers_.count(node) != 0; }
    std::vector<uint32_t> Peers() const;

    // Sends `frame` (type, timestamp, payload filled in) to every peer.
    // Fills node, seq and next_seq; reliable frames are kept for
    // retransmission. Returns the number of peers it went to.
    size_t Send(Frame* frame, bool reliable, Time now, const SendFn& send);

    // Sends a payload-less control datagram (kFlagHello, kFlagAckOnly) to
    // one peer, or, with node 0, an unaddressed one through `send` (used
    // for discovery, where the receiver is not a peer yet).
    void SendControl(uint32_t node, uint32_t flags, Time now,
                     const SendFn& send);

    // A validated datagram from a known peer. Frames for the guest, in
    // order, are appended to `ready`; control datagrams produce none.
    void Receive(Frame&& frame, Time now, std::vector<Frame>* ready);

    // Retransmissions, standalone acks, gaps given up on (their held
    // frames are appended to `ready`), sent frames retired.
    void Service(Time now, const SendFn& send, std::vector<Frame>* ready);

    // When Service next has something to do; Time::max() if never.
    Time NextDeadline() const;

    Stats stats() const;

private:
    struct Sent {
        uint32_t seq = 0;
        std::vector<uint8_t> datagram;
        Time first_sent{};
        Time last_sent{};
        uint32_t retransmits = 0;
        // Peers whose selective ack says they hold it behind a gap.
        std::vector<uint32_t> sacked_by;
    };

    struct Peer {
        // Our stream, as this peer acknowledges it.
        bool tx_synced = false;
        uint32_t tx_acked_next = 0;
        bool have_rtt = false;
        Clock::duration srtt{};
        Clock::duration rttvar{};
        Clock::duration rto = kInitialRto;
        // This peer's stream, as we receive it.
        bool rx_synced = false;
        uint32_t rx_expected = 0;
        std::map<uint32_t, Frame> held;
        Time gap_since{};
        bool ack_due = false;
        Time ack_due_since{};
    };

    static int64_t ToNs(Time t) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   t.time_since_epoch())
            .count();
    }

    void StampFor(uint32_t node, Peer& peer, std::vector<uint8_t>* datagram,
                  Time now);
    void OnAck(uint32_t node, Peer& peer, uint32_t ack_next,
               uint32_t ack_bits, Time now);
    static uint32_t AckBits(const Peer& peer);
    Sent* FindSent(uint32_t seq);
    bool PeerLacks(uint32_t node, const Peer& peer, const Sent& sent) const;
    void MarkAckDue(Peer& peer, Time now);
    void ReleaseInOrder(Peer& peer, std::vector<Frame>* ready);
    bool AckedByAll(uint32_t seq) const;
    Clock::duration RetransmitAfter(const Peer& peer,
                                    const Sent& sent) const;

    uint32_t self_ = 0;
    uint32_t next_seq_ = 1;
    std::map<uint32_t, Peer> peers_;
    std::deque<Sent> unacked_;
    Stats stats_{};
};

}  // namespace mpnet
