// mp_reliable.cpp -- see mp_reliable.h.

#include "net/mp_reliable.h"

#include <algorithm>
#include <utility>

namespace mpnet {

namespace {

uint32_t NextSeq(uint32_t seq) {
    ++seq;
    return seq == 0 ? 1 : seq;
}

}  // namespace

void Reliable::Reset(uint32_t self_node) {
    self_ = self_node;
    next_seq_ = 1;
    peers_.clear();
    unacked_.clear();
    stats_ = Stats{};
}

void Reliable::AddPeer(uint32_t node) {
    if (node == 0 || node == self_) return;
    peers_.emplace(node, Peer{});
}

void Reliable::RemovePeer(uint32_t node) { peers_.erase(node); }

std::vector<uint32_t> Reliable::Peers() const {
    std::vector<uint32_t> out;
    out.reserve(peers_.size());
    for (const auto& [node, peer] : peers_) out.push_back(node);
    return out;
}

uint32_t Reliable::AckBits(const Peer& peer) {
    uint32_t bits = 0;
    for (const auto& [seq, frame] : peer.held) {
        const uint32_t offset = seq - peer.rx_expected - 1u;
        if (offset >= 32u) break;
        bits |= 1u << offset;
    }
    return bits;
}

void Reliable::StampFor(uint32_t node, Peer& peer,
                        std::vector<uint8_t>* datagram, Time now) {
    Restamp(datagram->data(), ToNs(now), peer.rx_synced ? node : 0u,
            peer.rx_synced ? peer.rx_expected : 0u,
            peer.rx_synced ? AckBits(peer) : 0u);
    // Whatever goes to the peer carries the acknowledgement.
    if (peer.rx_synced) peer.ack_due = false;
}

size_t Reliable::Send(Frame* frame, bool reliable, Time now,
                      const SendFn& send) {
    frame->node = self_;
    frame->flags = 0;
    frame->ack_node = 0;
    frame->ack_next = 0;
    if (reliable) {
        frame->seq = next_seq_;
        next_seq_ = NextSeq(next_seq_);
    } else {
        frame->seq = 0;
    }
    frame->next_seq = next_seq_;
    frame->sent_ns = ToNs(now);

    std::vector<uint8_t> datagram;
    Encode(*frame, &datagram);
    for (auto& [node, peer] : peers_) {
        StampFor(node, peer, &datagram, now);
        send(node, datagram.data(), datagram.size());
    }
    // With no peer there is nobody to retransmit to: a peer that turns up
    // later joins the stream after this frame.
    if (reliable && !peers_.empty()) {
        if (unacked_.size() >= kMaxUnacked) {
            unacked_.pop_front();
            ++stats_.give_ups;
        }
        Sent sent;
        sent.seq = frame->seq;
        sent.datagram = std::move(datagram);
        sent.first_sent = now;
        sent.last_sent = now;
        unacked_.push_back(std::move(sent));
    }
    return peers_.size();
}

void Reliable::SendControl(uint32_t node, uint32_t flags, Time now,
                           const SendFn& send) {
    Frame frame;
    frame.node = self_;
    frame.flags = flags;
    frame.next_seq = next_seq_;
    frame.sent_ns = ToNs(now);
    std::vector<uint8_t> datagram;
    Encode(frame, &datagram);
    if (node != 0) {
        const auto it = peers_.find(node);
        if (it == peers_.end()) return;
        StampFor(node, it->second, &datagram, now);
    }
    send(node, datagram.data(), datagram.size());
}

void Reliable::MarkAckDue(Peer& peer, Time now) {
    if (peer.ack_due) return;
    peer.ack_due = true;
    peer.ack_due_since = now;
}

Reliable::Sent* Reliable::FindSent(uint32_t seq) {
    if (unacked_.empty()) return nullptr;
    // Frames are stored in sequence order, without holes while peers
    // exist; a hole just makes the lookup miss.
    const uint32_t offset = seq - unacked_.front().seq;
    if (offset >= unacked_.size()) return nullptr;
    Sent& sent = unacked_[offset];
    return sent.seq == seq ? &sent : nullptr;
}

bool Reliable::PeerLacks(uint32_t node, const Peer& peer,
                         const Sent& sent) const {
    if (!peer.tx_synced || SeqBefore(sent.seq, peer.tx_acked_next))
        return false;
    for (uint32_t n : sent.sacked_by)
        if (n == node) return false;
    return true;
}

void Reliable::OnAck(uint32_t node, Peer& peer, uint32_t ack_next,
                     uint32_t ack_bits, Time now) {
    // Never acknowledges what we have not sent yet.
    if (SeqBefore(next_seq_, ack_next)) return;
    // One RTT sample per acknowledgement, from the newest frame it is the
    // first news of, and only if that frame went out once (Karn). A frame
    // already known to be held behind a gap is no sample: the cumulative
    // ack covering it waited for the gap, not for the network.
    const Sent* sample_from = nullptr;
    for (uint32_t i = 0; ack_bits && i < 32u; ++i) {
        if ((ack_bits & (1u << i)) == 0) continue;
        Sent* sent = FindSent(ack_next + 1u + i);
        if (!sent || !PeerLacks(node, peer, *sent)) continue;
        sent->sacked_by.push_back(node);
        if (sent->retransmits == 0) sample_from = sent;
    }
    if (!peer.tx_synced) {
        peer.tx_synced = true;
        peer.tx_acked_next = ack_next;
        return;
    }
    if (SeqBefore(peer.tx_acked_next, ack_next)) {
        const Sent* newest = FindSent(ack_next - 1u);
        if (newest && newest->retransmits == 0 &&
            PeerLacks(node, peer, *newest) &&
            (!sample_from || SeqBefore(sample_from->seq, newest->seq)))
            sample_from = newest;
        peer.tx_acked_next = ack_next;
    }
    if (!sample_from) return;

    const Clock::duration sample = now - sample_from->first_sent;
    if (!peer.have_rtt) {
        peer.have_rtt = true;
        peer.srtt = sample;
        peer.rttvar = sample / 2;
    } else {
        const Clock::duration err =
            sample > peer.srtt ? sample - peer.srtt : peer.srtt - sample;
        peer.rttvar = (peer.rttvar * 3 + err) / 4;
        peer.srtt = (peer.srtt * 7 + sample) / 8;
    }
    peer.rto = std::clamp<Clock::duration>(peer.srtt + peer.rttvar * 4,
                                           kMinRto, kMaxRto);
}

void Reliable::ReleaseInOrder(Peer& peer, std::vector<Frame>* ready) {
    while (!peer.held.empty()) {
        auto it = peer.held.begin();
        if (SeqBefore(it->first, peer.rx_expected)) {
            peer.held.erase(it);
            continue;
        }
        if (it->first != peer.rx_expected) break;
        ready->push_back(std::move(it->second));
        peer.held.erase(it);
        peer.rx_expected = NextSeq(peer.rx_expected);
    }
}

void Reliable::Receive(Frame&& frame, Time now, std::vector<Frame>* ready) {
    const auto it = peers_.find(frame.node);
    if (it == peers_.end()) return;
    Peer& peer = it->second;

    if (frame.ack_node == self_ && frame.ack_next != 0)
        OnAck(frame.node, peer, frame.ack_next, frame.ack_bits, now);

    if (frame.seq == 0) {
        if (!peer.rx_synced && frame.next_seq != 0) {
            peer.rx_synced = true;
            peer.rx_expected = frame.next_seq;
            // Tell the sender where we joined, so it can retransmit.
            MarkAckDue(peer, now);
        }
        if (frame.flags & (kFlagHello | kFlagAckOnly)) return;
        ready->push_back(std::move(frame));
        return;
    }

    if (!peer.rx_synced) {
        peer.rx_synced = true;
        peer.rx_expected = frame.seq;
    }
    MarkAckDue(peer, now);
    if (SeqBefore(frame.seq, peer.rx_expected)) {
        ++stats_.duplicates;
        return;
    }
    if (frame.seq != peer.rx_expected) {
        if (peer.held.count(frame.seq)) {
            ++stats_.duplicates;
        } else if (peer.held.size() < kMaxHeld) {
            if (peer.held.empty()) peer.gap_since = now;
            peer.held.emplace(frame.seq, std::move(frame));
            ++stats_.reordered;
        }
        return;
    }
    ready->push_back(std::move(frame));
    peer.rx_expected = NextSeq(peer.rx_expected);
    const size_t held_before = peer.held.size();
    ReleaseInOrder(peer, ready);
    // The gap timer runs from the last progress, not the first hole.
    if (!peer.held.empty() && peer.held.size() != held_before)
        peer.gap_since = now;
}

bool Reliable::AckedByAll(uint32_t seq) const {
    for (const auto& [node, peer] : peers_) {
        if (peer.tx_synced && !SeqBefore(seq, peer.tx_acked_next))
            return false;
    }
    return true;
}

Reliable::Clock::duration Reliable::RetransmitAfter(const Peer& peer,
                                                    const Sent& sent) const {
    const uint32_t shift = std::min<uint32_t>(sent.retransmits, 4u);
    return std::min<Clock::duration>(peer.rto * (1u << shift), kMaxRto * 2);
}

void Reliable::Service(Time now, const SendFn& send,
                       std::vector<Frame>* ready) {
    size_t budget = kMaxRetransmitsPerService;
    for (Sent& sent : unacked_) {
        if (budget == 0) break;
        bool resent = false;
        for (auto& [node, peer] : peers_) {
            if (!PeerLacks(node, peer, sent)) continue;
            if (now - sent.last_sent < RetransmitAfter(peer, sent)) continue;
            StampFor(node, peer, &sent.datagram, now);
            send(node, sent.datagram.data(), sent.datagram.size());
            ++stats_.retransmits;
            resent = true;
            if (budget) --budget;
        }
        if (resent) {
            sent.last_sent = now;
            ++sent.retransmits;
        }
    }

    while (!unacked_.empty()) {
        const Sent& front = unacked_.front();
        const auto age = now - front.first_sent;
        if (AckedByAll(front.seq)) {
            if (age < kRetainMin) break;
        } else if (age < kGiveUpAfter) {
            break;
        } else {
            ++stats_.give_ups;
        }
        unacked_.pop_front();
    }

    for (auto& [node, peer] : peers_) {
        if (!peer.held.empty() && now - peer.gap_since >= kGapGiveUp) {
            ++stats_.gap_skips;
            peer.rx_expected = peer.held.begin()->first;
            ReleaseInOrder(peer, ready);
            peer.gap_since = now;
            MarkAckDue(peer, now);
        }
        if (peer.ack_due && now - peer.ack_due_since >= kAckDelay)
            SendControl(node, kFlagAckOnly, now, send);
    }
}

Reliable::Time Reliable::NextDeadline() const {
    Time next = Time::max();
    for (const auto& [node, peer] : peers_) {
        if (peer.ack_due)
            next = std::min(next, peer.ack_due_since + kAckDelay);
        if (!peer.held.empty())
            next = std::min(next, peer.gap_since + kGapGiveUp);
        if (!peer.tx_synced) continue;
        // Frames the peer lacks are the ones from tx_acked_next on; a few
        // of them are enough to find the earliest timer.
        size_t looked = 0;
        for (const Sent& sent : unacked_) {
            if (!PeerLacks(node, peer, sent)) continue;
            next = std::min(next, sent.last_sent + RetransmitAfter(peer, sent));
            if (++looked >= 16) break;
        }
    }
    return next;
}

Reliable::Stats Reliable::stats() const {
    Stats out = stats_;
    out.held = 0;
    for (const auto& [node, peer] : peers_) out.held += peer.held.size();
    out.unacked = unacked_.size();
    out.peers = peers_.size();
    return out;
}

}  // namespace mpnet
