// lobby_ws.h -- a WebSocket client for the online lobby: plain ws:// (the
// lobby server offers no TLS), text messages only, one thread.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mpnet {

class WsClient {
public:
    WsClient() = default;
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    // Connects and upgrades, blocking for at most timeout_ms. False with a
    // reason in *error.
    bool Connect(const std::string& url, int timeout_ms, std::string* error);
    void Close();
    bool IsOpen() const;

    // Queues one text message and sends what the socket takes. False when
    // the connection is gone or the backlog is too large.
    bool SendText(const std::string& text);

    // Waits up to timeout_ms for traffic, sends what is queued, answers
    // pings, and appends every complete text message. False when the
    // connection closed or failed (*error says why).
    bool Poll(int timeout_ms, std::vector<std::string>* messages,
              std::string* error);

    // ws://host[:port][/path]. Port defaults to 80.
    static bool ParseUrl(const std::string& url, std::string* host,
                         uint16_t* port, std::string* path,
                         std::string* error);

private:
    bool Flush();
    bool SendFrame(uint8_t opcode, const uint8_t* data, size_t len);
    // Parses rx_ into messages. False on a protocol error or close.
    bool ParseFrames(std::vector<std::string>* messages, std::string* error);

    intptr_t socket_ = -1;
    std::vector<uint8_t> rx_;
    std::vector<uint8_t> tx_;
    std::string fragment_;
    bool in_fragment_ = false;
};

}  // namespace mpnet
