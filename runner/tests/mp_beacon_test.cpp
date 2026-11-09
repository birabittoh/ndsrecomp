// mp_beacon_test.cpp -- WMGameInfo parsing from beacons and the hosting
// start/stop tracking (net/mp_beacon.h).

#include "net/mp_beacon.h"

#include <cstdio>
#include <vector>

namespace {

using mpnet::BeaconWatch;
using mpnet::GameInfo;
using std::chrono::milliseconds;

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

void Put16(std::vector<uint8_t>* v, unsigned x) {
    v->push_back(static_cast<uint8_t>(x));
    v->push_back(static_cast<uint8_t>(x >> 8));
}

// A beacon the way melonDS hands it to the transport: TX header, 802.11
// header, fixed fields, SSID, rates, then the Nintendo element.
std::vector<uint8_t> MakeBeacon(uint8_t attribute,
                                const std::vector<uint8_t>& user) {
    std::vector<uint8_t> f(12 + 24, 0);
    f[12] = 0x80;  // beacon
    for (int i = 0; i < 8; ++i) f.push_back(0);  // timestamp
    Put16(&f, 200);                               // interval
    Put16(&f, 0x0021);                            // capability
    f.insert(f.end(), {0x00, 0x00});              // empty SSID
    f.insert(f.end(), {0x01, 0x02, 0x82, 0x84});  // rates
    std::vector<uint8_t> e = {0x00, 0x09, 0xBF, 0x00};
    Put16(&e, 0x000A);  // stepping offset
    Put16(&e, 0x0000);  // LCD video sync
    Put16(&e, 0x0001);  // magic
    e.push_back(0x00);  // ver
    e.push_back(0x40);  // platform
    Put16(&e, 0x5678);  // GGID lo
    Put16(&e, 0x1234);  // GGID hi
    Put16(&e, 0x0042);  // TGID
    e.push_back(static_cast<uint8_t>(user.size()));
    e.push_back(attribute);
    Put16(&e, 0x00C0);  // parent max size
    Put16(&e, 0x0040);  // child max size
    e.insert(e.end(), user.begin(), user.end());
    f.push_back(0xDD);
    f.push_back(static_cast<uint8_t>(e.size()));
    f.insert(f.end(), e.begin(), e.end());
    f.insert(f.end(), {0, 0, 0, 0});  // FCS
    const unsigned frame_len = static_cast<unsigned>(f.size() - 12);
    f[10] = static_cast<uint8_t>(frame_len);
    f[11] = static_cast<uint8_t>(frame_len >> 8);
    return f;
}

bool TestParse() {
    const std::vector<uint8_t> user = {0x11, 0x22, 0x33, 0x44};
    const auto b = MakeBeacon(mpnet::kAttrEntry, user);
    GameInfo g;
    bool ok = require(mpnet::ParseBeaconGameInfo(b.data(), b.size(), &g),
                      "beacon parses");
    ok = require(g.ggid == 0x12345678u && g.tgid == 0x42 &&
                     g.attribute == mpnet::kAttrEntry &&
                     g.parent_max_size == 0xC0 && g.child_max_size == 0x40 &&
                     g.beacon_interval == 200,
                 "fixed fields") && ok;
    ok = require(g.user_len == 4 && g.user[0] == 0x11 && g.user[3] == 0x44,
                 "user game info") && ok;

    auto not_beacon = b;
    not_beacon[12] = 0x50;  // probe response
    ok = require(!mpnet::ParseBeaconGameInfo(not_beacon.data(),
                                             not_beacon.size(), &g),
                 "probe response ignored") && ok;
    auto truncated = b;
    truncated.resize(truncated.size() - 10);
    truncated[10] = static_cast<uint8_t>(truncated.size() - 12);
    ok = require(!mpnet::ParseBeaconGameInfo(truncated.data(),
                                             truncated.size(), &g),
                 "truncated element rejected") && ok;
    return ok;
}

bool TestWatch() {
    BeaconWatch w;
    BeaconWatch::Time t{};
    const auto b = MakeBeacon(mpnet::kAttrEntry, {1, 2});
    GameInfo g;
    mpnet::ParseBeaconGameInfo(b.data(), b.size(), &g);
    bool ok = require(w.OnBeacon(g, t) == BeaconWatch::Event::kStarted,
                      "first beacon starts");
    t += milliseconds(200);
    ok = require(w.OnBeacon(g, t) == BeaconWatch::Event::kNone,
                 "same info") && ok;
    ok = require(w.Poll(t + milliseconds(1000)) == BeaconWatch::Event::kNone,
                 "short gap") && ok;
    GameInfo closed = g;
    closed.attribute = 0;
    t += milliseconds(200);
    ok = require(w.OnBeacon(closed, t) == BeaconWatch::Event::kChanged,
                 "attribute change") && ok;
    ok = require(w.count() == 3, "count") && ok;
    ok = require(w.Poll(t + milliseconds(1600)) ==
                     BeaconWatch::Event::kStopped && !w.active(),
                 "silence stops") && ok;
    ok = require(w.Poll(t + milliseconds(3000)) == BeaconWatch::Event::kNone,
                 "stopped once") && ok;
    ok = require(w.OnBeacon(g, t + milliseconds(4000)) ==
                     BeaconWatch::Event::kStarted && w.count() == 1,
                 "restart") && ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = TestParse();
    ok = TestWatch() && ok;
    if (!ok) return 1;
    std::printf("mp_beacon_test: ok\n");
    return 0;
}
