// wfc_local_server.h -- the built-in Nintendo Wi-Fi Connection server
// ([network.wfc] provider = "built-in").
//
// Nintendo's servers are gone, so the runner answers the game itself,
// without the internet:
//
//   DNS        every *.nintendowifi.net name -> kServerIp (other names go on
//              to the normal resolver)
//   conntest   GET /                 -> "ok", X-Organization: Nintendo
//   nas        POST /ac              -> account, login and service location
//                                       accepted
//   dls1       POST /download        -> the title's files (title_dls.h)
//
// The game logs in over HTTPS with the DS's own SSL. Like the community's
// "NoSSL" patches, the https:// addresses of Nintendo's servers (and the
// download server's "https://%s/download" templates) in main RAM
// become http:// when the game looks up one of their names (before it
// connects), so this server speaks plain HTTP on port 80.
//
// TCP: the DS opens one short connection per request. Frames for kServerIp
// never reach libslirp; a small responder here takes the handshake, collects
// the request, sends the reply within the DS's window and closes. The link
// to the guest is a memory queue, so a lost segment is unusual; the DS's own
// retransmissions (and duplicate ACKs) make this side send again.
//
// Runs on the emulation thread (Net_SendPacket), so it may read guest
// memory and the title's query sees the guest as it is.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class NdsWfcLocalServer {
public:
    // TEST-NET-1 (RFC 5737): never a real host.
    static constexpr uint8_t kServerIp[4] = {192, 0, 2, 80};

    // A frame the guest sent. True when the server took it; its answers
    // (Ethernet frames for the guest) are appended to `out`.
    bool HandleTx(const uint8_t* frame, int len,
                  std::vector<std::vector<uint8_t>>* out);

private:
    struct Conn {
        bool used = false;
        uint8_t mac_ds[6] = {}, mac_gw[6] = {}, ip_ds[4] = {};
        uint16_t port = 0;
        uint32_t iss = 0, snd_una = 0, snd_nxt = 0, rcv_nxt = 0;
        uint32_t wnd = 0, mss = 536;
        std::string request;
        std::vector<uint8_t> reply;
        bool have_reply = false, fin_in = false, fin_sent = false;
        uint64_t last_use = 0;
    };

    bool HandleDns(const uint8_t* frame, int len,
                   std::vector<std::vector<uint8_t>>* out);
    bool HandleTcp(const uint8_t* frame, int len,
                   std::vector<std::vector<uint8_t>>* out);
    void Send(const Conn& c, uint32_t seq, uint8_t flags, const uint8_t* data,
              int n, std::vector<std::vector<uint8_t>>* out) const;
    void Push(Conn* c, std::vector<std::vector<uint8_t>>* out);
    std::vector<uint8_t> Http(const std::string& request, const uint8_t mac[6]);
    void PatchNoSsl();

    Conn conns_[8];
    uint64_t use_counter_ = 0;
    uint32_t rand_ = 0x2545F491u;
};
