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
// Waiting for that timeout stalls the guest: frames behind the hole are
// held, the emulation thread of an MP client blocks on the host's next
// frame and an MP host on a client's reply. Two things keep a lost
// datagram from costing a timeout:
//  - Redundancy. Every datagram to a peer also carries the newest frames
//    that peer has not acknowledged yet (1 to kMaxRedundancy of them, as
//    long as the datagram stays small, mp_wire.h), so the next datagram
//    fills the hole on arrival. The count per peer follows the loss seen:
//    it goes up when retransmissions were needed anyway and back down
//    after quiet seconds.
//  - Fast retransmit. A selective ack showing later frames arrived while
//    an earlier one did not means that one is lost; it is sent again as
//    soon as a round trip has passed since it last went out, without
//    waiting for the timeout.
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
        uint64_t fast_retransmits = 0;  // of retransmits, on a selective ack
        uint64_t recovered = 0;    // frames first received as a bundled copy
        uint64_t redundant_bytes = 0;   // bundled copies sent
        uint32_t redundancy = 0;   // highest per-peer redundancy now
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
    // Until the first round trip is measured. A timeout that fires backs
    // the peer's timeout off until the next measurement: a frame that was
    // retransmitted gives none, so with a round trip above the timeout
    // every frame would otherwise go out twice and none would ever
    // measure it.
    static constexpr std::chrono::milliseconds kInitialRto{300};
    static constexpr std::chrono::milliseconds kMinRto{20};
    // The timeout leaves at least this much room over the smoothed round
    // trip: an acknowledgement that is itself lost is followed by the
    // next one a frame later, and that is no reason to send again.
    static constexpr std::chrono::milliseconds kRtoMargin{40};
    static constexpr std::chrono::milliseconds kMaxRto{1000};
    static constexpr size_t kMaxUnacked = 4096;
    static constexpr size_t kMaxHeld = 1024;
    static constexpr size_t kMaxRetransmitsPerService = 64;
    // Earlier frames carried again per datagram: the range, the window
    // over which loss is judged, and the retransmissions within one
    // window that raise it (a hole costs the guest a whole round trip of
    // stall, a bundled frame some bytes). A window without any lowers it.
    static constexpr uint32_t kMinRedundancy = 1;
    static constexpr uint32_t kMaxRedundancy = 4;
    static constexpr std::chrono::milliseconds kRedundancyWindow{5000};
    static constexpr uint32_t kRedundancyRaiseAt = 1;

    explicit Reliable(uint32_t self_node = 0) { Reset(self_node); }

    // Forgets every peer and starts a new stream as `self_node`.
    void Reset(uint32_t self_node);

    uint32_t self() const { return self_; }

    void AddPeer(uint32_t node);
    void RemovePeer(uint32_t node);
    bool HasPeer(uint32_t node) const { return peers_.count(node) != 0; }
    // Frames from `node` are held behind a gap: a retransmission of the
    // missing one is on its way.
    bool HasGap(uint32_t node) const {
        const auto it = peers_.find(node);
        return it != peers_.end() && !it->second.held.empty();
    }
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
        // Peers a selective ack showed to be missing it.
        std::vector<uint32_t> fast_for;
    };

    struct Peer {
        // Our stream, as this peer acknowledges it.
        bool tx_synced = false;
        uint32_t tx_acked_next = 0;
        bool have_rtt = false;
        Clock::duration srtt{};
        Clock::duration rttvar{};
        Clock::duration rto = kInitialRto;
        Time rto_backed_off{};
        // This peer's stream, as we receive it.
        bool rx_synced = false;
        uint32_t rx_expected = 0;
        std::map<uint32_t, Frame> held;
        Time gap_since{};
        bool ack_due = false;
        Time ack_due_since{};
        // Redundancy towards this peer.
        uint32_t redundancy = kMinRedundancy;
        uint32_t window_losses = 0;
        uint32_t control_bundled_seq = 0;
        Time window_start{};
    };

    static int64_t ToNs(Time t) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   t.time_since_epoch())
            .count();
    }

    void StampFor(uint32_t node, Peer& peer, std::vector<uint8_t>* datagram,
                  Time now);
    // Stamps `datagram` for the peer and sends it with the peer's newest
    // unacknowledged frames bundled behind it.
    // Datagrams without a frame of the stream (acks, hellos, beacons)
    // carry each newest frame once only, which covers the end of a burst
    // of traffic without repeating it on every ack.
    void SendWithBundle(uint32_t node, Peer& peer,
                        std::vector<uint8_t>* datagram, bool sequenced,
                        Time now, const SendFn& send);
    void Accept(Peer& peer, Frame&& frame, bool bundled, Time now,
                std::vector<Frame>* ready);
    void NoteLoss(Peer& peer);
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
    std::vector<uint8_t> scratch_;
    // A selective ack found frames to send again at the next Service.
    bool fast_pending_ = false;
    Stats stats_{};
};

}  // namespace mpnet
