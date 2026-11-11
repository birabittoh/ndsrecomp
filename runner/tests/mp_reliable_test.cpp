// mp_reliable_test.cpp -- the local wireless datagram format and reliable
// delivery (net/mp_wire.h, net/mp_reliable.h) over a simulated network with
// loss, delay, jitter and reordering, in virtual time.

#include "net/mp_reliable.h"
#include "net/mp_wire.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <vector>

namespace {

using mpnet::Frame;
using mpnet::Reliable;
using Time = Reliable::Time;
using std::chrono::milliseconds;

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

bool TestWireRoundTrip() {
    Frame in;
    in.node = 0xA1B2C3D4u;
    in.type = 2u | (3u << 16u);
    in.timestamp = 0x0123456789ABCDEFull;
    in.sent_ns = -42;
    in.seq = 7;
    in.next_seq = 8;
    in.ack_node = 0x11223344u;
    in.ack_next = 99;
    in.ack_bits = 0x80000001u;
    in.flags = mpnet::kFlagAckOnly;
    in.payload = {1, 2, 3, 4, 5};
    std::vector<uint8_t> bytes;
    mpnet::Encode(in, &bytes);
    Frame out;
    bool ok = require(bytes.size() == mpnet::kHeaderBytes + 5, "size") &&
              require(mpnet::Decode(bytes.data(), bytes.size(), &out),
                      "decodes") &&
              require(out.node == in.node && out.type == in.type &&
                          out.timestamp == in.timestamp &&
                          out.sent_ns == in.sent_ns && out.seq == in.seq &&
                          out.next_seq == in.next_seq &&
                          out.ack_node == in.ack_node &&
                          out.ack_next == in.ack_next &&
                          out.ack_bits == in.ack_bits &&
                          out.flags == in.flags && out.payload == in.payload,
                      "fields round-trip");
    mpnet::Restamp(bytes.data(), 1234, 5, 6, 0x10);
    ok = ok && require(mpnet::Decode(bytes.data(), bytes.size(), &out) &&
                           out.sent_ns == 1234 && out.ack_node == 5 &&
                           out.ack_next == 6 && out.ack_bits == 0x10 &&
                           out.seq == 7 && out.flags == in.flags,
                       "restamp rewrites only per-send fields");
    // Truncated, wrong length, wrong magic, node 0.
    ok = ok && require(!mpnet::Decode(bytes.data(), bytes.size() - 1, &out),
                       "truncated rejected");
    std::vector<uint8_t> bad = bytes;
    bad[0] ^= 1;
    ok = ok && require(!mpnet::Decode(bad.data(), bad.size(), &out),
                       "bad magic rejected");
    bad = bytes;
    std::memset(bad.data() + 4, 0, 4);
    ok = ok && require(!mpnet::Decode(bad.data(), bad.size(), &out),
                       "node 0 rejected");

    // Bundled frames: appended behind the payload, decoded oldest first.
    {
        Frame first, second;
        first.node = second.node = in.node;
        first.type = 1;
        first.timestamp = 111;
        first.seq = 5;
        first.payload = {9, 8, 7};
        second.type = 2u | (1u << 16u);
        second.timestamp = 222;
        second.seq = 6;
        std::vector<uint8_t> b1, b2, dg;
        mpnet::Encode(first, &b1);
        mpnet::Encode(second, &b2);
        mpnet::Encode(in, &dg);
        ok = ok && require(mpnet::AppendBundled(&dg, b1.data()) &&
                               mpnet::AppendBundled(&dg, b2.data()),
                           "bundles appended");
        Frame got;
        ok = ok && require(mpnet::Decode(dg.data(), dg.size(), &got) &&
                               got.payload == in.payload &&
                               got.seq == in.seq && got.bundled.size() == 2,
                           "bundled datagram decodes") &&
             require(got.bundled[0].seq == 5 && got.bundled[0].type == 1 &&
                         got.bundled[0].timestamp == 111 &&
                         got.bundled[0].node == in.node &&
                         got.bundled[0].payload == first.payload &&
                         got.bundled[1].seq == 6 &&
                         got.bundled[1].type == second.type &&
                         got.bundled[1].timestamp == 222 &&
                         got.bundled[1].payload.empty(),
                     "bundled frames round-trip") &&
             require(!mpnet::Decode(dg.data(), dg.size() - 1, &got),
                     "truncated bundle rejected");
        std::vector<uint8_t> trailing = dg;
        trailing.push_back(0);
        ok = ok && require(!mpnet::Decode(trailing.data(), trailing.size(),
                                          &got),
                           "trailing bytes rejected");
        Frame fat;
        fat.node = in.node;
        fat.seq = 9;
        fat.payload.assign(mpnet::kBundleLimitBytes, 0);
        std::vector<uint8_t> bf;
        mpnet::Encode(fat, &bf);
        const size_t before = dg.size();
        ok = ok && require(!mpnet::AppendBundled(&dg, bf.data()) &&
                               dg.size() == before,
                           "a bundle past the size limit is refused");
    }

    uint8_t beacon[12 + 24] = {};
    beacon[12] = 0x80;
    uint8_t auth[12 + 24] = {};
    auth[12] = 0xB0;
    ok = ok && require(mpnet::IsBeacon(0, beacon, sizeof(beacon)),
                       "beacon detected") &&
         require(!mpnet::IsBeacon(1, beacon, sizeof(beacon)),
                 "MP cmd is not a beacon") &&
         require(!mpnet::IsBeacon(0, auth, sizeof(auth)),
                 "auth is not a beacon");
    return ok;
}

// A network of nodes, each sending a numbered reliable frame every
// `period` and an unreliable beacon every 100 ms. Checks that every node
// receives every other node's numbered frames exactly once and in order.
struct Sim {
    struct InFlight {
        Time at;
        uint32_t to;
        std::vector<uint8_t> bytes;
    };

    uint32_t loss_pct = 0;
    milliseconds delay{0};
    milliseconds jitter{0};
    bool reorder = false;
    // Burst loss: per mille chance that a datagram starts a run of
    // burst_len lost datagrams (all senders, as on a shared uplink).
    uint32_t burst_pct = 0;
    uint32_t burst_len = 0;
    uint32_t burst_left = 0;

    std::mt19937 rng{12345};
    std::map<uint32_t, Reliable> nodes;
    std::vector<InFlight> wire;
    std::map<std::pair<uint32_t, uint32_t>, Time> last_arrival;
    // received[to][from] = counters in arrival order
    std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> received;
    std::map<uint32_t, uint32_t> next_counter;
    uint64_t beacons_received = 0;
    uint64_t datagrams = 0;
    uint64_t bytes = 0;
    // How much later than the link's own delay each frame reached the
    // guest: what a lost datagram costs the emulation thread waiting on it.
    Time now_{};
    std::map<std::pair<uint32_t, uint32_t>, Time> sent_at;
    std::vector<int64_t> late_ms;

    Reliable::SendFn Sender(uint32_t from, Time now) {
        return [this, from, now](uint32_t to, const uint8_t* data,
                                 size_t len) {
            ++datagrams;
            bytes += len;
            if (burst_left) {
                --burst_left;
                return;
            }
            if (burst_pct && rng() % 1000u < burst_pct) {
                burst_left = burst_len - 1;
                return;
            }
            if (loss_pct && rng() % 100u < loss_pct) return;
            auto at = now + delay;
            if (jitter.count())
                at += milliseconds(rng() % (jitter.count() + 1));
            auto& last = last_arrival[{from, to}];
            if (!reorder && at < last) at = last;
            last = at;
            wire.push_back(InFlight{at, to, std::vector<uint8_t>(
                                                data, data + len)});
        };
    }

    void Collect(uint32_t to, std::vector<Frame>* ready) {
        for (const Frame& f : *ready) {
            if (f.type == 0) {
                ++beacons_received;
                continue;
            }
            uint32_t counter = 0;
            std::memcpy(&counter, f.payload.data(), 4);
            received[to][f.node].push_back(counter);
            const auto it = sent_at.find({f.node, counter});
            if (it != sent_at.end())
                late_ms.push_back(
                    std::chrono::duration_cast<milliseconds>(
                        now_ - it->second - delay).count());
        }
        ready->clear();
    }

    void Run(std::initializer_list<uint32_t> ids, milliseconds period,
             milliseconds send_for, milliseconds total) {
        for (uint32_t id : ids) nodes.emplace(id, Reliable(id));
        for (auto& [id, rel] : nodes)
            for (uint32_t other : ids) rel.AddPeer(other);
        const Time t0 = Time() + std::chrono::hours(1);
        std::vector<Frame> ready;
        for (milliseconds t{0}; t < total; t += milliseconds(1)) {
            const Time now = t0 + t;
            now_ = now;
            // Hellos for the first second, as the transport does.
            if (t < milliseconds(1000) && t.count() % 100 == 0) {
                for (auto& [id, rel] : nodes)
                    for (uint32_t other : rel.Peers())
                        rel.SendControl(other, mpnet::kFlagHello, now,
                                        Sender(id, now));
            }
            if (t >= milliseconds(1000) && t < send_for) {
                for (auto& [id, rel] : nodes) {
                    if (t.count() % 100 == 0) {
                        Frame beacon;
                        beacon.type = 0;
                        beacon.payload.assign(12 + 24, 0);
                        beacon.payload[12] = 0x80;
                        rel.Send(&beacon, false, now, Sender(id, now));
                    }
                    if (t.count() % period.count() != 0) continue;
                    Frame frame;
                    frame.type = 1;
                    const uint32_t counter = next_counter[id]++;
                    frame.payload.resize(64);
                    std::memcpy(frame.payload.data(), &counter, 4);
                    sent_at[{id, counter}] = now;
                    rel.Send(&frame, true, now, Sender(id, now));
                }
            }
            for (size_t i = 0; i < wire.size();) {
                if (wire[i].at > now) {
                    ++i;
                    continue;
                }
                InFlight d = std::move(wire[i]);
                wire.erase(wire.begin() + static_cast<long>(i));
                Frame f;
                if (!mpnet::Decode(d.bytes.data(), d.bytes.size(), &f))
                    continue;
                nodes[d.to].Receive(std::move(f), now, &ready);
                Collect(d.to, &ready);
            }
            for (auto& [id, rel] : nodes) {
                rel.Service(now, Sender(id, now), &ready);
                Collect(id, &ready);
            }
        }
    }

    bool CheckExactlyOnceInOrder(const char* label) {
        bool ok = true;
        for (auto& [to, rel] : nodes) {
            for (auto& [from, sent] : next_counter) {
                if (from == to) continue;
                const auto& got = received[to][from];
                bool same = got.size() == sent;
                for (size_t i = 0; same && i < got.size(); ++i)
                    same = got[i] == i;
                if (!same) {
                    std::fprintf(stderr,
                        "FAIL: %s: %08x got %zu of %u frames from %08x "
                        "(in order: %s)\n", label, to, got.size(), sent,
                        from, same ? "yes" : "no");
                    ok = false;
                }
            }
            const Reliable::Stats st = rel.stats();
            if (st.gap_skips || st.give_ups) {
                std::fprintf(stderr, "FAIL: %s: %08x gap_skips=%llu "
                             "give_ups=%llu\n", label, to,
                             static_cast<unsigned long long>(st.gap_skips),
                             static_cast<unsigned long long>(st.give_ups));
                ok = false;
            }
        }
        return ok;
    }

    // Frames delivered more than `ms` later than the link's delay, per
    // thousand.
    double LatePerMille(int64_t ms) const {
        if (late_ms.empty()) return 0.0;
        size_t n = 0;
        for (int64_t v : late_ms) n += v > ms;
        return 1000.0 * static_cast<double>(n) /
               static_cast<double>(late_ms.size());
    }

    int64_t LatePercentile(double pct) {
        if (late_ms.empty()) return 0;
        std::vector<int64_t> v = late_ms;
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(pct / 100.0 *
                                     static_cast<double>(v.size() - 1))];
    }

    void Report(const char* label) {
        uint64_t retx = 0, dup = 0, reord = 0, fast = 0, recovered = 0;
        uint32_t redundancy = 0;
        size_t unacked = 0;
        for (auto& [id, rel] : nodes) {
            const auto st = rel.stats();
            fast += st.fast_retransmits;
            recovered += st.recovered;
            redundancy = std::max(redundancy, st.redundancy);
            retx += st.retransmits;
            dup += st.duplicates;
            reord += st.reordered;
            unacked += st.unacked;
        }
        std::printf("%s: fast_retx=%llu recovered=%llu redundancy=%u\n  ",
                    label, static_cast<unsigned long long>(fast),
                    static_cast<unsigned long long>(recovered), redundancy);
        std::printf("%s: datagrams=%llu retx=%llu dup=%llu reordered=%llu "
                    "beacons=%llu unacked_left=%zu kB=%llu | late ms "
                    "p50=%lld p99=%lld p99.9=%lld max=%lld >40ms=%.1f/1000\n",
                    label,
                    static_cast<unsigned long long>(datagrams),
                    static_cast<unsigned long long>(retx),
                    static_cast<unsigned long long>(dup),
                    static_cast<unsigned long long>(reord),
                    static_cast<unsigned long long>(beacons_received),
                    unacked, static_cast<unsigned long long>(bytes / 1024),
                    static_cast<long long>(LatePercentile(50)),
                    static_cast<long long>(LatePercentile(99)),
                    static_cast<long long>(LatePercentile(99.9)),
                    static_cast<long long>(LatePercentile(100)),
                    LatePerMille(40));
    }
};

bool TestCleanLink() {
    Sim sim;
    sim.delay = milliseconds(12);
    sim.Run({0x1111, 0x2222}, milliseconds(16), milliseconds(6000),
            milliseconds(9000));
    sim.Report("clean");
    uint64_t retx = 0;
    for (auto& [id, rel] : sim.nodes) retx += rel.stats().retransmits;
    return sim.CheckExactlyOnceInOrder("clean") &&
           require(retx == 0, "no retransmits on a clean link") &&
           require(sim.beacons_received > 0, "beacons delivered");
}

bool TestLossyJitteryLink() {
    Sim sim;
    sim.loss_pct = 5;
    sim.delay = milliseconds(50);
    sim.jitter = milliseconds(10);
    sim.Run({0x1111, 0x2222, 0x3333}, milliseconds(16), milliseconds(8000),
            milliseconds(14000));
    sim.Report("loss 5% D=50 jitter 10");
    uint64_t retx = 0, dup = 0, recovered = 0;
    size_t unacked = 0;
    for (auto& [id, rel] : sim.nodes) {
        retx += rel.stats().retransmits;
        dup += rel.stats().duplicates;
        recovered += rel.stats().recovered;
        unacked += rel.stats().unacked;
    }
    // The next datagram carries a lost frame again, so loss costs the
    // guest about one send period (16 ms) plus jitter, not a timeout,
    // and hardly anything is left to retransmit.
    return sim.CheckExactlyOnceInOrder("lossy") &&
           require(recovered > 0, "lost frames recovered from bundles") &&
           require(dup <= retx, "no duplicates beyond retransmissions") &&
           require(sim.LatePercentile(100) <= 40,
                   "5 % loss never stalls delivery past 40 ms") &&
           require(unacked == 0, "everything acknowledged and retired");
}

bool TestReorderingLink() {
    Sim sim;
    sim.loss_pct = 2;
    sim.delay = milliseconds(20);
    sim.jitter = milliseconds(30);
    sim.reorder = true;
    sim.Run({0x1111, 0x2222}, milliseconds(5), milliseconds(6000),
            milliseconds(10000));
    sim.Report("reorder, loss 2%");
    uint64_t reordered = 0;
    for (auto& [id, rel] : sim.nodes) reordered += rel.stats().reordered;
    return sim.CheckExactlyOnceInOrder("reorder") &&
           require(reordered > 0, "out-of-order frames were held");
}

bool TestHeavyLoss() {
    Sim sim;
    sim.loss_pct = 20;
    sim.delay = milliseconds(30);
    sim.Run({0x1111, 0x2222}, milliseconds(16), milliseconds(5000),
            milliseconds(12000));
    sim.Report("loss 20%");
    // Before redundancy and fast retransmit: 852 of 1000 frames more than
    // 40 ms late, the worst 3 s.
    return sim.CheckExactlyOnceInOrder("heavy loss") &&
           require(sim.LatePerMille(40) < 100.0,
                   "20 % loss: under 10 % of frames over 40 ms late") &&
           require(sim.LatePercentile(100) < 500,
                   "20 % loss: no stall of half a second");
}

// Loss in runs of 4 datagrams, as on Wi-Fi or a congested uplink: more
// than the starting redundancy covers, so selective acks trigger fast
// retransmits and the redundancy rises.
bool TestBurstLoss() {
    Sim sim;
    sim.burst_pct = 15;
    sim.burst_len = 4;
    sim.delay = milliseconds(40);
    sim.jitter = milliseconds(5);
    sim.Run({0x1111, 0x2222}, milliseconds(16), milliseconds(9000),
            milliseconds(14000));
    sim.Report("bursts of 4, D=40");
    uint64_t fast = 0;
    for (auto& [id, rel] : sim.nodes) fast += rel.stats().fast_retransmits;
    return sim.CheckExactlyOnceInOrder("burst") &&
           require(fast > 0, "bursts trigger fast retransmits") &&
           require(sim.LatePercentile(100) < 400,
                   "bursts: no stall of 400 ms");
}

// A quarter second of round trip with heavy jitter and loss.
bool TestBadLink() {
    Sim sim;
    sim.loss_pct = 10;
    sim.delay = milliseconds(125);
    sim.jitter = milliseconds(40);
    // Long enough for the redundancy to settle: the first holes each cost
    // a round trip before it has risen.
    sim.Run({0x1111, 0x2222}, milliseconds(16), milliseconds(40000),
            milliseconds(46000));
    sim.Report("loss 10% D=125 jitter 40");
    return sim.CheckExactlyOnceInOrder("bad link") &&
           require(sim.LatePerMille(80) < 20.0,
                   "bad link: 98 % of frames within jitter + 40 ms");
}

// Redundancy is paid in bytes: on a clean link it stays at one earlier
// frame per datagram, and full-size frames are never bundled.
bool TestRedundancyCost() {
    Sim sim;
    sim.delay = milliseconds(12);
    sim.Run({0x1111, 0x2222}, milliseconds(16), milliseconds(6000),
            milliseconds(9000));
    bool ok = true;
    for (auto& [id, rel] : sim.nodes) {
        const auto st = rel.stats();
        ok = require(st.redundancy == Reliable::kMinRedundancy,
                     "clean link keeps the lowest redundancy") && ok;
        // 313 frames of 64 bytes, each bundled once on the next frame
        // and at most once more on an ack.
        ok = require(st.redundant_bytes <=
                         2u * 313u * (mpnet::kBundleHeaderBytes + 64u),
                     "clean link: each frame bundled at most twice") && ok;
    }

    Reliable a(0xAAAA), b(0xBBBB);
    a.AddPeer(0xBBBB);
    b.AddPeer(0xAAAA);
    const Time t0 = Time() + std::chrono::hours(1);
    std::vector<std::vector<uint8_t>> to_a, to_b;
    const auto send_a = [&](uint32_t, const uint8_t* d, size_t n) {
        to_b.emplace_back(d, d + n);
    };
    const auto send_b = [&](uint32_t, const uint8_t* d, size_t n) {
        to_a.emplace_back(d, d + n);
    };
    // Synchronise both ways, then send big frames.
    std::vector<Frame> ready;
    a.SendControl(0xBBBB, mpnet::kFlagHello, t0, send_a);
    b.SendControl(0xAAAA, mpnet::kFlagHello, t0, send_b);
    const auto pump = [&](Time now) {
        for (auto& d : to_b) {
            Frame f;
            if (mpnet::Decode(d.data(), d.size(), &f))
                b.Receive(std::move(f), now, &ready);
        }
        to_b.clear();
        for (auto& d : to_a) {
            Frame f;
            if (mpnet::Decode(d.data(), d.size(), &f))
                a.Receive(std::move(f), now, &ready);
        }
        to_a.clear();
    };
    pump(t0);
    b.SendControl(0xAAAA, mpnet::kFlagAckOnly, t0, send_b);
    pump(t0);
    size_t largest = 0;
    for (int i = 0; i < 4; ++i) {
        Frame big;
        big.type = 1;
        big.payload.assign(mpnet::kMaxPayloadBytes, 0x5A);
        a.Send(&big, true, t0 + milliseconds(i), send_a);
        Frame small;
        small.type = 1;
        small.payload.assign(40, 0x11);
        a.Send(&small, true, t0 + milliseconds(i), send_a);
        for (auto& d : to_b) largest = std::max(largest, d.size());
        to_b.clear();  // none arrive: all stay unacknowledged
    }
    return ok &&
           require(largest == mpnet::kMaxDatagramBytes,
                   "a full-size frame goes out alone, at its own size");
}

bool TestNoPeers() {
    Reliable rel(0x1111);
    Frame frame;
    frame.type = 1;
    frame.payload = {1, 2, 3};
    size_t sends = 0;
    const size_t peers = rel.Send(&frame, true, Time() + milliseconds(1),
                                  [&](uint32_t, const uint8_t*, size_t) {
                                      ++sends;
                                  });
    return require(peers == 0 && sends == 0, "no peer, nothing sent") &&
           require(rel.stats().unacked == 0, "no peer, nothing retained") &&
           require(frame.seq == 1 && frame.next_seq == 2,
                   "sequence still advances");
}

}  // namespace

int main() {
    bool ok = TestWireRoundTrip();
    ok = TestNoPeers() && ok;
    ok = TestCleanLink() && ok;
    ok = TestLossyJitteryLink() && ok;
    ok = TestReorderingLink() && ok;
    ok = TestHeavyLoss() && ok;
    ok = TestBurstLoss() && ok;
    ok = TestBadLink() && ok;
    ok = TestRedundancyCost() && ok;
    if (!ok) return 1;
    std::printf("mp_reliable_test: ok\n");
    return 0;
}
