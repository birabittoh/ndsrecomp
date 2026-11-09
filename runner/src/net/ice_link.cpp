// ice_link.cpp -- see ice_link.h.

#include "net/ice_link.h"

#include <juice/juice.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

namespace mpnet {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kSeatNet = 0xC0000200u;  // 192.0.2.0
constexpr uint16_t kSeatPort = 1;
// A guest gives an agent this long to connect, then offers again.
constexpr auto kConnectTimeout = std::chrono::seconds(30);
// ... and waits this long after a failure before the next offer.
constexpr auto kRetryDelay = std::chrono::seconds(3);
// The transport drains far faster than this; it only bounds a stall.
constexpr size_t kMaxRxQueue = 4096;

bool Connected(int state) {
    return state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
}

std::string CandidateType(const char* line) {
    const char* typ = std::strstr(line, " typ ");
    if (!typ) return "unknown";
    typ += 5;
    const char* end = typ;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n') ++end;
    return std::string(typ, end);
}

void JuiceLog(juice_log_level_t level, const char* message) {
    if (level >= JUICE_LOG_LEVEL_WARN)
        std::fprintf(stderr, "[online] juice: %s\n", message);
}

}  // namespace

PeerAddr SeatAddr(int seat) {
    PeerAddr addr;
    addr.ip = kSeatNet | static_cast<uint32_t>(seat & 0xFF);
    addr.port = kSeatPort;
    return addr;
}

int SeatOf(const PeerAddr& addr) {
    if ((addr.ip & 0xFFFFFF00u) != kSeatNet || addr.port != kSeatPort)
        return -1;
    const int seat = static_cast<int>(addr.ip & 0xFFu);
    return seat < kMaxSeats ? seat : -1;
}

bool IsRelayCandidate(const std::string& line) {
    return CandidateType(line.c_str()) == "relay";
}

std::string FilterRelayOnly(const std::string& sdp) {
    std::string out;
    std::istringstream in(sdp);
    std::string line;
    while (std::getline(in, line)) {
        std::string bare = line;
        if (!bare.empty() && bare.back() == '\r') bare.pop_back();
        const bool candidate = bare.rfind("a=candidate:", 0) == 0 ||
                               bare.rfind("candidate:", 0) == 0;
        if (candidate && !IsRelayCandidate(bare)) continue;
        out += bare;
        out += "\r\n";
    }
    return out;
}

struct IceLink::Peer {
    IceLink* link = nullptr;
    int seat = -1;
    int gen = 0;
    bool offerer = false;
    juice_agent_t* agent = nullptr;
    // Set before the agent is destroyed: late callbacks do nothing.
    std::atomic<bool> dead{false};
    // libjuice may keep pointers into its config.
    std::string turn_host, turn_user, turn_pass;
    juice_turn_server_t turn{};
    // Guarded by the link's mutex.
    int state = JUICE_STATE_DISCONNECTED;
    bool relay = false;
    bool relay_checked = false;
    Clock::time_point started{};
    Clock::time_point failed_at{};

    ~Peer() {
        dead = true;
        if (agent) juice_destroy(agent);
    }
};

struct IceLinkCallbacks {
    static void State(juice_agent_t*, juice_state_t state, void* user) {
        auto* peer = static_cast<IceLink::Peer*>(user);
        if (!peer->dead) peer->link->OnState(peer, state);
    }
    static void Candidate(juice_agent_t*, const char* sdp, void* user) {
        auto* peer = static_cast<IceLink::Peer*>(user);
        if (!peer->dead) peer->link->OnCandidate(peer, sdp);
    }
    static void GatheringDone(juice_agent_t*, void* user) {
        auto* peer = static_cast<IceLink::Peer*>(user);
        if (!peer->dead) peer->link->OnGatheringDone(peer);
    }
    static void Recv(juice_agent_t*, const char* data, size_t size,
                     void* user) {
        auto* peer = static_cast<IceLink::Peer*>(user);
        if (!peer->dead) peer->link->OnRecv(peer, data, size);
    }
};

IceLink::IceLink() {
    static std::once_flag log_once;
    std::call_once(log_once, [] {
        juice_set_log_handler(JuiceLog);
        juice_set_log_level(JUICE_LOG_LEVEL_WARN);
    });
    forwarder_ = std::thread([this] { ForwardLoop(); });
}

IceLink::~IceLink() {
    std::map<int, std::shared_ptr<Peer>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        doomed.swap(peers_);
    }
    fwd_cv_.notify_all();
    if (forwarder_.joinable()) forwarder_.join();
    doomed.clear();
}

// ---- control half ---------------------------------------------------------

void IceLink::SetRoom(int self_seat, int hub_seat, const TurnConfig& turn) {
    std::map<int, std::shared_ptr<Peer>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doomed.swap(peers_);
        in_room_ = true;
        self_seat_ = self_seat;
        hub_seat_ = hub_seat;
        turn_ = turn;
        wanted_.clear();
        signals_out_.clear();
        forward_.clear();
    }
    std::fprintf(stderr, "[online] link: seat %d, hub seat %d%s\n", self_seat,
                 hub_seat, self_seat == hub_seat ? " (this runner)" : "");
}

void IceLink::ClearRoom() {
    std::map<int, std::shared_ptr<Peer>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        doomed.swap(peers_);
        in_room_ = false;
        self_seat_ = hub_seat_ = -1;
        wanted_.clear();
        signals_out_.clear();
        forward_.clear();
        rx_.clear();
    }
    std::fprintf(stderr, "[online] link: left the room\n");
}

bool IceLink::InRoom() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_room_;
}

void IceLink::SetPeers(const std::vector<int>& seats) {
    std::vector<std::shared_ptr<Peer>> doomed;
    bool offer = false;
    int hub = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        wanted_ = seats;
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (std::find(seats.begin(), seats.end(), it->first) ==
                seats.end()) {
                std::fprintf(stderr, "[online] link: seat %d dropped\n",
                             it->first);
                doomed.push_back(std::move(it->second));
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
        hub = hub_seat_;
        offer = self_seat_ != hub_seat_ &&
                std::find(seats.begin(), seats.end(), hub) != seats.end() &&
                peers_.find(hub) == peers_.end();
    }
    doomed.clear();
    if (offer) {
        int gen;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = next_gen_.find(hub);
            if (it == next_gen_.end()) {
                // Random start: a hub that still remembers this seat's
                // last negotiation must not take the new one for it.
                std::random_device rd;
                it = next_gen_.emplace(hub, 1 + rd() % 255).first;
            }
            gen = it->second;
            it->second = it->second % 255 + 1;
        }
        StartOffer(hub, gen);
    }
}

std::shared_ptr<IceLink::Peer> IceLink::CreateAgent(int seat, int gen,
                                                    bool offerer) {
    auto peer = std::make_shared<Peer>();
    peer->link = this;
    peer->seat = seat;
    peer->gen = gen;
    peer->offerer = offerer;
    peer->started = Clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer->turn_host = turn_.host;
        peer->turn_user = turn_.user;
        peer->turn_pass = turn_.pass;
        peer->turn.port = turn_.port;
    }
    if (peer->turn_host.empty()) {
        std::fprintf(stderr,
                     "[online] link: no TURN server, seat %d not linked\n",
                     seat);
        return nullptr;
    }
    peer->turn.host = peer->turn_host.c_str();
    peer->turn.username = peer->turn_user.c_str();
    peer->turn.password = peer->turn_pass.c_str();

    juice_config_t config;
    std::memset(&config, 0, sizeof(config));
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    config.stun_server_host = nullptr;  // no server-reflexive candidates
    config.turn_servers = &peer->turn;
    config.turn_servers_count = 1;
    config.cb_state_changed = IceLinkCallbacks::State;
    config.cb_candidate = IceLinkCallbacks::Candidate;
    config.cb_gathering_done = IceLinkCallbacks::GatheringDone;
    config.cb_recv = IceLinkCallbacks::Recv;
    config.user_ptr = peer.get();
    peer->agent = juice_create(&config);
    if (!peer->agent) {
        std::fprintf(stderr, "[online] link: juice_create failed (seat %d)\n",
                     seat);
        return nullptr;
    }
    return peer;
}

void IceLink::StartOffer(int seat, int gen) {
    std::shared_ptr<Peer> peer = CreateAgent(seat, gen, true);
    if (!peer) return;
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(peer->agent, sdp, sizeof(sdp)) != 0)
        return;
    std::shared_ptr<Peer> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_ || std::find(wanted_.begin(), wanted_.end(), seat) ==
                             wanted_.end())
            return;
        auto& slot = peers_[seat];
        old = std::move(slot);
        slot = peer;
        signals_out_.push_back({seat, kIceSignalSdp, gen, FilterRelayOnly(sdp)});
    }
    old.reset();
    std::fprintf(stderr, "[online] link: offer to seat %d (gen %d)\n", seat,
                 gen);
    juice_gather_candidates(peer->agent);
}

void IceLink::Answer(int seat, int gen, const std::string& remote_sdp) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_ || self_seat_ != hub_seat_) return;
        if (std::find(wanted_.begin(), wanted_.end(), seat) == wanted_.end())
            return;
        auto it = peers_.find(seat);
        if (it != peers_.end() && it->second->gen == gen) return;  // repeat
    }
    std::shared_ptr<Peer> peer = CreateAgent(seat, gen, false);
    if (!peer) return;
    if (juice_set_remote_description(peer->agent,
                                     FilterRelayOnly(remote_sdp).c_str()) !=
        0) {
        std::fprintf(stderr, "[online] link: bad offer from seat %d\n", seat);
        return;
    }
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(peer->agent, sdp, sizeof(sdp)) != 0)
        return;
    std::shared_ptr<Peer> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        auto& slot = peers_[seat];
        old = std::move(slot);
        slot = peer;
        signals_out_.push_back({seat, kIceSignalSdp, gen, FilterRelayOnly(sdp)});
    }
    old.reset();
    std::fprintf(stderr, "[online] link: answer to seat %d (gen %d)\n", seat,
                 gen);
    juice_gather_candidates(peer->agent);
}

void IceLink::PushSignal(const IceSignal& signal) {
    if (signal.type == kIceSignalSdp) {
        bool hub;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hub = in_room_ && self_seat_ == hub_seat_;
        }
        if (hub) {
            Answer(signal.seat, signal.gen, signal.text);
            return;
        }
    }
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        auto it = peers_.find(signal.seat);
        if (it == peers_.end() || it->second->gen != signal.gen) return;
        peer = it->second;
    }
    switch (signal.type) {
    case kIceSignalSdp:  // the hub's answer to our offer
        if (juice_set_remote_description(
                peer->agent, FilterRelayOnly(signal.text).c_str()) != 0)
            std::fprintf(stderr, "[online] link: bad answer from seat %d\n",
                         signal.seat);
        break;
    case kIceSignalCandidate:
        if (IsRelayCandidate(signal.text))
            juice_add_remote_candidate(peer->agent, signal.text.c_str());
        break;
    case kIceSignalDone:
        juice_set_remote_gathering_done(peer->agent);
        break;
    default:
        break;
    }
}

std::vector<IceSignal> IceLink::TakeSignals() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<IceSignal> out;
    out.swap(signals_out_);
    return out;
}

void IceLink::Poll(Clock::time_point now) {
    std::vector<std::shared_ptr<Peer>> unchecked;
    std::vector<std::shared_ptr<Peer>> doomed;
    bool reoffer = false;
    int hub = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        hub = hub_seat_;
        const bool is_hub = self_seat_ == hub_seat_;
        for (auto it = peers_.begin(); it != peers_.end();) {
            Peer& p = *it->second;
            if (Connected(p.state) && !p.relay_checked)
                unchecked.push_back(it->second);
            const bool failed = p.state == JUICE_STATE_FAILED;
            const bool stale = !is_hub && !Connected(p.state) &&
                               now - p.started > kConnectTimeout;
            // The hub drops a failed agent and waits for the next offer;
            // a guest makes that offer once the retry delay has passed.
            if ((failed && (is_hub || now - p.failed_at > kRetryDelay)) ||
                stale) {
                std::fprintf(stderr, "[online] link: seat %d %s (gen %d)\n",
                             it->first, failed ? "failed" : "timed out",
                             p.gen);
                doomed.push_back(std::move(it->second));
                it = peers_.erase(it);
                continue;
            }
            ++it;
        }
        reoffer = !is_hub &&
                  std::find(wanted_.begin(), wanted_.end(), hub) !=
                      wanted_.end() &&
                  peers_.find(hub) == peers_.end();
    }
    doomed.clear();

    for (const auto& peer : unchecked) {
        char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
        char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
        if (juice_get_selected_candidates(peer->agent, local, sizeof(local),
                                          remote, sizeof(remote)) != 0)
            continue;
        const std::string lt = CandidateType(local);
        const std::string rt = CandidateType(remote);
        const bool relay = lt == "relay" && rt == "relay";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            peer->relay_checked = true;
            peer->relay = relay;
            if (!relay) {
                // Never carry a byte over a pair that could reveal an
                // address. The guest's next offer starts over.
                auto it = peers_.find(peer->seat);
                if (it != peers_.end() && it->second == peer) {
                    doomed.push_back(std::move(it->second));
                    peers_.erase(it);
                }
            }
        }
        std::fprintf(stderr,
                     "[online] link: seat %d connected, local %s, remote %s%s\n",
                     peer->seat, lt.c_str(), rt.c_str(),
                     relay ? "" : ": not relay, closed");
    }
    doomed.clear();

    if (reoffer) {
        // SetPeers with the same list starts the next offer.
        std::vector<int> seats;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seats = wanted_;
        }
        SetPeers(seats);
    }
}

std::vector<IceLink::PeerStatus> IceLink::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerStatus> out;
    for (const auto& [seat, peer] : peers_) {
        PeerStatus s;
        s.seat = seat;
        s.state = peer->state;
        s.relay = peer->relay;
        s.gen = peer->gen;
        out.push_back(s);
    }
    return out;
}

// ---- libjuice callbacks ---------------------------------------------------

void IceLink::OnState(Peer* peer, int state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer->state = state;
        if (state == JUICE_STATE_FAILED) peer->failed_at = Clock::now();
    }
    std::fprintf(stderr, "[online] link: seat %d %s\n", peer->seat,
                 juice_state_to_string(static_cast<juice_state_t>(state)));
}

void IceLink::OnCandidate(Peer* peer, const char* sdp) {
    if (!sdp || !IsRelayCandidate(sdp)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    signals_out_.push_back({peer->seat, kIceSignalCandidate, peer->gen, sdp});
}

void IceLink::OnGatheringDone(Peer* peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    signals_out_.push_back({peer->seat, kIceSignalDone, peer->gen, ""});
}

void IceLink::OnRecv(Peer* peer, const char* data, size_t size) {
    if (size < kEnvelopeBytes ||
        static_cast<uint8_t>(data[0]) != kEnvelopeTag)
        return;
    const int src = static_cast<uint8_t>(data[1]);
    const uint8_t dst = static_cast<uint8_t>(data[2]);
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    bool wake_rx = false;
    bool wake_fwd = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        const bool is_hub = self_seat_ == hub_seat_;
        const bool for_us = dst == kSeatEveryone ||
                            dst == static_cast<uint8_t>(self_seat_);
        if (is_hub) {
            if (src != peer->seat) return;  // a guest speaks for itself only
            if (dst == kSeatEveryone) {
                for (const auto& [seat, other] : peers_) {
                    if (seat == src) continue;
                    forward_.push_back(
                        {seat, std::vector<uint8_t>(bytes, bytes + size)});
                    wake_fwd = true;
                }
            } else if (!for_us && peers_.count(dst)) {
                forward_.push_back(
                    {dst, std::vector<uint8_t>(bytes, bytes + size)});
                wake_fwd = true;
            }
        } else if (peer->seat != hub_seat_) {
            return;
        }
        if (for_us && open_ && rx_.size() < kMaxRxQueue) {
            rx_.emplace_back(src, std::vector<uint8_t>(
                                      bytes + kEnvelopeBytes, bytes + size));
            wake_rx = true;
        }
    }
    if (wake_rx) rx_cv_.notify_all();
    if (wake_fwd) fwd_cv_.notify_all();
}

void IceLink::ForwardLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        fwd_cv_.wait(lock, [this] { return stop_ || !forward_.empty(); });
        if (stop_) return;
        Datagram d = std::move(forward_.front());
        forward_.pop_front();
        auto it = peers_.find(d.seat);
        if (it == peers_.end() || !it->second->relay) continue;
        std::shared_ptr<Peer> peer = it->second;
        lock.unlock();
        SendOnAgent(peer, d.bytes);
        peer.reset();
        lock.lock();
    }
}

// ---- MpLink ---------------------------------------------------------------

bool IceLink::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    return true;
}

void IceLink::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    rx_.clear();
}

bool IceLink::IsOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

void IceLink::SendOnAgent(const std::shared_ptr<Peer>& peer,
                          const std::vector<uint8_t>& bytes) {
    juice_send(peer->agent, reinterpret_cast<const char*>(bytes.data()),
               bytes.size());
}

void IceLink::Send(int seat, const uint8_t* data, size_t len, uint8_t dst) {
    std::shared_ptr<Peer> peer;
    std::vector<uint8_t> bytes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_ || !open_) return;
        // A guest reaches everyone through the hub.
        const int via = self_seat_ == hub_seat_ ? seat : hub_seat_;
        auto it = peers_.find(via);
        if (it == peers_.end() || !it->second->relay) return;
        peer = it->second;
        bytes.reserve(kEnvelopeBytes + len);
        bytes.push_back(kEnvelopeTag);
        bytes.push_back(static_cast<uint8_t>(self_seat_));
        bytes.push_back(dst);
    }
    bytes.insert(bytes.end(), data, data + len);
    SendOnAgent(peer, bytes);
}

bool IceLink::SendTo(const PeerAddr& to, const uint8_t* data, size_t len) {
    const int seat = SeatOf(to);
    if (seat < 0) return false;
    Send(seat, data, len, static_cast<uint8_t>(seat));
    return true;
}

void IceLink::SendDiscovery(const uint8_t* data, size_t len) {
    std::vector<int> seats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!in_room_) return;
        if (self_seat_ == hub_seat_) {
            for (const auto& [seat, peer] : peers_) seats.push_back(seat);
        } else {
            seats.push_back(hub_seat_);
        }
    }
    for (int seat : seats) Send(seat, data, len, kSeatEveryone);
}

int IceLink::Recv(uint8_t* data, size_t capacity, PeerAddr* from) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!rx_.empty()) {
        auto [seat, payload] = std::move(rx_.front());
        rx_.pop_front();
        if (payload.size() > capacity) continue;
        std::memcpy(data, payload.data(), payload.size());
        if (from) *from = SeatAddr(seat);
        return static_cast<int>(payload.size());
    }
    return 0;
}

bool IceLink::WaitReadable(int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!rx_.empty()) return true;
    if (timeout_us <= 0) return false;
    rx_cv_.wait_for(lock, std::chrono::microseconds(timeout_us),
                    [this] { return !rx_.empty(); });
    return !rx_.empty();
}

namespace {

class IceLinkView : public MpLink {
public:
    explicit IceLinkView(std::shared_ptr<IceLink> link)
        : link_(std::move(link)) {}
    bool Open() override { return link_->Open(); }
    void Close() override { link_->Close(); }
    bool IsOpen() const override { return link_->IsOpen(); }
    bool SendTo(const PeerAddr& to, const uint8_t* data,
                size_t len) override {
        return link_->SendTo(to, data, len);
    }
    void SendDiscovery(const uint8_t* data, size_t len) override {
        link_->SendDiscovery(data, len);
    }
    int Recv(uint8_t* data, size_t capacity, PeerAddr* from) override {
        return link_->Recv(data, capacity, from);
    }
    bool WaitReadable(int64_t timeout_us) override {
        return link_->WaitReadable(timeout_us);
    }
    std::string Describe() const override { return link_->Describe(); }

private:
    std::shared_ptr<IceLink> link_;
};

}  // namespace

std::unique_ptr<MpLink> MakeLinkView(std::shared_ptr<IceLink> link) {
    return std::make_unique<IceLinkView>(std::move(link));
}

std::string IceLink::Describe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!in_room_) return "online (not in a lobby)";
    char text[96];
    std::snprintf(text, sizeof(text), "online seat=%d hub=%d%s", self_seat_,
                  hub_seat_, self_seat_ == hub_seat_ ? " (hub)" : "");
    return text;
}

}  // namespace mpnet
