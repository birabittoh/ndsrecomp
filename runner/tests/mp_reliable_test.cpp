// mp_reliable_test.cpp -- the local wireless datagram format and reliable
// delivery (net/mp_wire.h, net/mp_reliable.h) over a simulated network with
// loss, delay, jitter and reordering, in virtual time.

#include "net/mp_reliable.h"
#include "net/mp_wire.h"

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

    std::mt19937 rng{12345};
    std::map<uint32_t, Reliable> nodes;
    std::vector<InFlight> wire;
    std::map<std::pair<uint32_t, uint32_t>, Time> last_arrival;
    // received[to][from] = counters in arrival order
    std::map<uint32_t, std::map<uint32_t, std::vector<uint32_t>>> received;
    std::map<uint32_t, uint32_t> next_counter;
    uint64_t beacons_received = 0;
    uint64_t datagrams = 0;

    Reliable::SendFn Sender(uint32_t from, Time now) {
        return [this, from, now](uint32_t to, const uint8_t* data,
                                 size_t len) {
            ++datagrams;
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

    void Report(const char* label) {
        uint64_t retx = 0, dup = 0, reord = 0;
        size_t unacked = 0;
        for (auto& [id, rel] : nodes) {
            const auto st = rel.stats();
            retx += st.retransmits;
            dup += st.duplicates;
            reord += st.reordered;
            unacked += st.unacked;
        }
        std::printf("%s: datagrams=%llu retx=%llu dup=%llu reordered=%llu "
                    "beacons=%llu unacked_left=%zu\n", label,
                    static_cast<unsigned long long>(datagrams),
                    static_cast<unsigned long long>(retx),
                    static_cast<unsigned long long>(dup),
                    static_cast<unsigned long long>(reord),
                    static_cast<unsigned long long>(beacons_received),
                    unacked);
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
    uint64_t retx = 0, dup = 0;
    size_t unacked = 0;
    for (auto& [id, rel] : sim.nodes) {
        retx += rel.stats().retransmits;
        dup += rel.stats().duplicates;
        unacked += rel.stats().unacked;
    }
    // Selective acks: what arrived behind a gap is not sent again, so
    // duplicates stay well below retransmissions.
    return sim.CheckExactlyOnceInOrder("lossy") &&
           require(retx > 0, "lossy link retransmits") &&
           require(dup * 2 < retx, "few duplicate deliveries") &&
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
    return sim.CheckExactlyOnceInOrder("heavy loss");
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
    if (!ok) return 1;
    std::printf("mp_reliable_test: ok\n");
    return 0;
}
