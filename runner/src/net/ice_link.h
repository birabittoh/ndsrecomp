// ice_link.h -- local wireless over the internet: ICE through a TURN relay.
//
// A lobby is a radio room (HANDOFF_ONLINE_LOBBY.md §2.1): while this runner
// sits in an online lobby, the transport's datagrams go to the other lobby
// members instead of the LAN. The lobby's host is the hub. It holds one ICE
// agent per guest and forwards what one guest sends to the others; a guest
// holds one agent, to the hub. Every member is addressed by its lobby seat,
// as a fake IPv4 address in TEST-NET-1 (SeatAddr), so the transport's peer
// table works unchanged.
//
// Relay only. No STUN server is configured, and every candidate that is not
// "typ relay" is dropped: from the local description, from trickle, and
// from what the remote side sends (a remote host candidate would make this
// agent send checks to it from a local socket, revealing this machine's
// address). A pair that still selects anything but relay on both ends is
// closed. So the other players only ever see the TURN server's address.
//
// Signalling (SDP, candidates, end of gathering) goes through the lobby as
// IceSignal values: TakeSignals() for what to send, PushSignal() for what
// arrived. `gen` names one negotiation. The guest offers and picks a new
// gen for every new agent; the hub answers, and starts a fresh agent when
// an offer carries a gen it has not seen. Signals of any other gen are
// dropped, so a retry is never mixed up with the attempt before it.
//
// Envelope, in front of every transport datagram: 0xD5, source seat,
// destination seat (0xFF = everyone). The hub accepts from a guest only
// datagrams whose source is that guest's seat.
//
// Threads: the lobby thread calls the control half, the emulation thread
// the MpLink half, and libjuice calls back on its own thread. Nothing calls
// into libjuice while holding the link's mutex or from inside a libjuice
// callback; the hub forwards on a thread of its own.

#pragma once

#include "net/mp_link.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mpnet {

// Lobby `signal` types for this link. Clear of recomp-net's ranges (1-6,
// 100-102, 110-126, 130-136).
constexpr int kIceSignalSdp = 150;
constexpr int kIceSignalCandidate = 151;
constexpr int kIceSignalDone = 152;

constexpr int kMaxSeats = 8;
constexpr uint8_t kSeatEveryone = 0xFF;
constexpr uint8_t kEnvelopeTag = 0xD5;
constexpr size_t kEnvelopeBytes = 3;

// Seat N as the transport sees it: 192.0.2.N:1 (RFC 5737, never routed).
PeerAddr SeatAddr(int seat);
// -1 when `addr` is not a seat address.
int SeatOf(const PeerAddr& addr);

struct TurnConfig {
    std::string host;
    uint16_t port = 3478;
    std::string user;
    std::string pass;
};

struct IceSignal {
    int seat = -1;  // to (TakeSignals) or from (PushSignal)
    int type = 0;   // kIceSignal*
    int gen = 0;    // 1..255
    std::string text;
};

// Candidate and SDP filtering, exposed for tests.
bool IsRelayCandidate(const std::string& line);
// Drops every a=candidate line that is not typ relay. Lines may be
// "candidate:..." (trickle) or "a=candidate:..." (inside an SDP).
std::string FilterRelayOnly(const std::string& sdp);

class IceLink : public MpLink {
public:
    struct PeerStatus {
        int seat = -1;
        int state = 0;        // juice_state_t
        bool relay = false;   // selected pair checked: relay on both ends
        int gen = 0;
        uint32_t retries = 0;
    };

    IceLink();
    ~IceLink() override;

    IceLink(const IceLink&) = delete;
    IceLink& operator=(const IceLink&) = delete;

    // ---- control half (lobby thread) -----------------------------------
    // Enters a room: this runner's seat and the hub's. Drops every agent.
    // self_seat == hub_seat makes this runner the hub.
    void SetRoom(int self_seat, int hub_seat, const TurnConfig& turn);
    // Leaves the room: drops every agent.
    void ClearRoom();
    // The seats this runner links to. Hub: every guest. Guest: the hub.
    // Agents of seats not listed are dropped; a guest starts its offer.
    void SetPeers(const std::vector<int>& seats);
    void PushSignal(const IceSignal& signal);
    std::vector<IceSignal> TakeSignals();
    // Timers: a guest retries an agent that failed or took too long; any
    // agent whose selected pair is not relay on both ends is dropped.
    void Poll(std::chrono::steady_clock::time_point now);
    std::vector<PeerStatus> Status() const;
    bool InRoom() const;

    // ---- MpLink (emulation thread) ---------------------------------------
    bool Open() override;
    void Close() override;
    bool IsOpen() const override;
    bool SendTo(const PeerAddr& to, const uint8_t* data, size_t len) override;
    void SendDiscovery(const uint8_t* data, size_t len) override;
    int Recv(uint8_t* data, size_t capacity, PeerAddr* from) override;
    bool WaitReadable(int64_t timeout_us) override;
    std::string Describe() const override;

    struct Peer;  // public for libjuice's C callbacks

private:
    struct Datagram {
        int seat = -1;               // hub: the agent it goes out on
        std::vector<uint8_t> bytes;  // envelope included
    };

    std::shared_ptr<Peer> CreateAgent(int seat, int gen, bool offerer);
    void StartOffer(int seat, int gen);
    void Answer(int seat, int gen, const std::string& sdp);
    void Send(int seat, const uint8_t* data, size_t len, uint8_t dst);
    void SendOnAgent(const std::shared_ptr<Peer>& peer,
                     const std::vector<uint8_t>& bytes);
    void ForwardLoop();

    // libjuice callbacks
    void OnState(Peer* peer, int state);
    void OnCandidate(Peer* peer, const char* sdp);
    void OnGatheringDone(Peer* peer);
    void OnRecv(Peer* peer, const char* data, size_t size);
    friend struct IceLinkCallbacks;

    mutable std::mutex mutex_;
    std::condition_variable rx_cv_;
    std::condition_variable fwd_cv_;

    bool in_room_ = false;
    int self_seat_ = -1;
    int hub_seat_ = -1;
    TurnConfig turn_;
    std::vector<int> wanted_;
    std::map<int, std::shared_ptr<Peer>> peers_;
    std::map<int, int> next_gen_;  // guest: the gen of its next offer
    std::vector<IceSignal> signals_out_;

    bool open_ = false;
    std::deque<std::pair<int, std::vector<uint8_t>>> rx_;  // seat, payload

    std::deque<Datagram> forward_;
    bool stop_ = false;
    std::thread forwarder_;
};

// An MpLink for the transport that shares `link` with the lobby, which
// keeps the agents: closing the view only stops delivery.
std::unique_ptr<MpLink> MakeLinkView(std::shared_ptr<IceLink> link);

}  // namespace mpnet
