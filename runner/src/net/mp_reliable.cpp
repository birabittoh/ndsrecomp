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
    fast_pending_ = false;
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
    for (auto& [node, peer] : peers_)
        SendWithBundle(node, peer, &datagram, reliable, now, send);
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
        SendWithBundle(node, it->second, &datagram, false, now, send);
        return;
    }
    send(node, datagram.data(), datagram.size());
}

void Reliable::SendWithBundle(uint32_t node, Peer& peer,
                              std::vector<uint8_t>* datagram,
                              bool sequenced, Time now, const SendFn& send) {
    StampFor(node, peer, datagram, now);
    // The newest frames this peer has not acknowledged, the one going out
    // right now (not in unacked_ yet) aside.
    const Sent* pick[kMaxRedundancy];
    uint32_t picked = 0;
    for (auto it = unacked_.rbegin(); peer.tx_synced &&
         it != unacked_.rend() && picked < peer.redundancy; ++it) {
        if (!PeerLacks(node, peer, *it)) {
            // Everything older is acknowledged too, bar holes, and those
            // are the retransmission timers' business.
            if (SeqBefore(it->seq, peer.tx_acked_next)) break;
            continue;
        }
        pick[picked++] = &*it;
    }
    if (picked != 0 && !sequenced) {
        if (pick[0]->seq == peer.control_bundled_seq) picked = 0;
        else peer.control_bundled_seq = pick[0]->seq;
    }
    if (picked == 0) {
        send(node, datagram->data(), datagram->size());
        return;
    }
    scratch_ = *datagram;
    while (picked > 0) {
        const Sent& sent = *pick[--picked];
        const size_t before = scratch_.size();
        if (!AppendBundled(&scratch_, sent.datagram.data())) continue;
        stats_.redundant_bytes += scratch_.size() - before;
    }
    send(node, scratch_.data(), scratch_.size());
}

void Reliable::NoteLoss(Peer& peer) { ++peer.window_losses; }

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
    // The peer holds frames after ack_next but not ack_next itself, nor
    // the ones its bitmap leaves out below the highest it has: lost, once
    // a round trip has passed since they last went out (before that the
    // ack may simply predate their arrival).
    if (ack_bits && peer.tx_synced) {
        const Clock::duration guard =
            peer.have_rtt ? peer.srtt + peer.rttvar : kInitialRto / 2;
        uint32_t top = 31;
        while ((ack_bits & (1u << top)) == 0) --top;
        for (uint32_t i = 0; i <= top; ++i) {
            if (i > 0 && (ack_bits & (1u << (i - 1u)))) continue;
            Sent* sent = FindSent(ack_next + i);
            if (!sent || !PeerLacks(node, peer, *sent)) continue;
            if (now - sent->last_sent < guard) continue;
            if (std::find(sent->fast_for.begin(), sent->fast_for.end(),
                          node) == sent->fast_for.end())
                sent->fast_for.push_back(node);
            fast_pending_ = true;
        }
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
    peer.rto = std::clamp<Clock::duration>(
        peer.srtt + std::max<Clock::duration>(peer.rttvar * 4, kRtoMargin),
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

    // Where this receiver joins the sender's stream, before anything the
    // datagram bundles: frames before that were not for it.
    if (!peer.rx_synced && (frame.seq != 0 || frame.next_seq != 0)) {
        peer.rx_synced = true;
        peer.rx_expected = frame.seq != 0 ? frame.seq : frame.next_seq;
        // Tell the sender where we joined, so it can retransmit.
        MarkAckDue(peer, now);
    }
    // Bundled frames are older than the datagram's own: first, in order.
    std::vector<Frame> bundled = std::move(frame.bundled);
    frame.bundled.clear();
    if (peer.rx_synced) {
        for (Frame& b : bundled) {
            b.from = frame.from;
            Accept(peer, std::move(b), true, now, ready);
        }
    }

    if (frame.seq == 0) {
        if (frame.flags & (kFlagHello | kFlagAckOnly)) return;
        ready->push_back(std::move(frame));
        return;
    }
    Accept(peer, std::move(frame), false, now, ready);
}

void Reliable::Accept(Peer& peer, Frame&& frame, bool bundled, Time now,
                      std::vector<Frame>* ready) {
    if (SeqBefore(frame.seq, peer.rx_expected)) {
        // A bundled copy of a frame already here is the normal case.
        if (!bundled) {
            ++stats_.duplicates;
            MarkAckDue(peer, now);
        }
        return;
    }
    if (frame.seq != peer.rx_expected) {
        if (peer.held.count(frame.seq)) {
            if (!bundled) {
                ++stats_.duplicates;
                MarkAckDue(peer, now);
            }
        } else if (peer.held.size() < kMaxHeld) {
            if (peer.held.empty()) peer.gap_since = now;
            peer.held.emplace(frame.seq, std::move(frame));
            ++stats_.reordered;
            if (bundled) ++stats_.recovered;
            MarkAckDue(peer, now);
        }
        return;
    }
    MarkAckDue(peer, now);
    if (bundled) ++stats_.recovered;
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
    // Backing off is for congestion, and this stream is a trickle: double
    // only so far that a dead path is not flooded.
    const uint32_t shift = std::min<uint32_t>(sent.retransmits, 2u);
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
            const bool fast =
                std::find(sent.fast_for.begin(), sent.fast_for.end(),
                          node) != sent.fast_for.end();
            if (!fast &&
                now - sent.last_sent < RetransmitAfter(peer, sent))
                continue;
            StampFor(node, peer, &sent.datagram, now);
            send(node, sent.datagram.data(), sent.datagram.size());
            ++stats_.retransmits;
            if (fast) {
                ++stats_.fast_retransmits;
            } else if (now - peer.rto_backed_off >= peer.rto) {
                peer.rto_backed_off = now;
                peer.rto = std::min<Clock::duration>(peer.rto * 2, kMaxRto);
            }
            NoteLoss(peer);
            resent = true;
            if (budget) --budget;
        }
        sent.fast_for.clear();
        if (resent) {
            sent.last_sent = now;
            ++sent.retransmits;
        }
    }

    fast_pending_ = false;
    for (const Sent& sent : unacked_) {
        if (!sent.fast_for.empty()) {
            fast_pending_ = true;  // over this call's budget
            break;
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
        // Redundancy follows the loss it did not cover.
        if (peer.window_start == Time{}) peer.window_start = now;
        if (peer.window_losses >= kRedundancyRaiseAt) {
            if (peer.redundancy < kMaxRedundancy) ++peer.redundancy;
            peer.window_losses = 0;
            peer.window_start = now;
        } else if (now - peer.window_start >= kRedundancyWindow) {
            if (peer.window_losses == 0 && peer.redundancy > kMinRedundancy)
                --peer.redundancy;
            peer.window_losses = 0;
            peer.window_start = now;
        }
    }
}

Reliable::Time Reliable::NextDeadline() const {
    if (fast_pending_) return Time::min();
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
    out.redundancy = 0;
    for (const auto& [node, peer] : peers_) {
        out.held += peer.held.size();
        out.redundancy = std::max(out.redundancy, peer.redundancy);
    }
    out.unacked = unacked_.size();
    out.peers = peers_.size();
    return out;
}

}  // namespace mpnet
