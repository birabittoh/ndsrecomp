// lobby_ws.cpp -- see lobby_ws.h.

#include "net/lobby_ws.h"

#include "recomp_net/rnet_ws.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mpnet {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
int SocketError() { return WSAGetLastError(); }
bool WouldBlock(int err) {
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
void SocketClose(Socket s) { closesocket(s); }
void SetNonBlocking(Socket s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
int SocketError() { return errno; }
bool WouldBlock(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == EINPROGRESS ||
           err == EINTR;
}
void SocketClose(Socket s) { ::close(s); }
void SetNonBlocking(Socket s) {
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}
#endif

Socket AsSocket(intptr_t s) { return static_cast<Socket>(s); }

// select() on one socket. 1 ready, 0 timeout, -1 error.
int WaitSocket(Socket s, bool want_write, int timeout_ms) {
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_SET(s, &rd);
    if (want_write) FD_SET(s, &wr);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int n = select(static_cast<int>(s) + 1, &rd,
                         want_write ? &wr : nullptr, nullptr, &tv);
    return n < 0 ? -1 : (n > 0 ? 1 : 0);
}

constexpr size_t kMaxMessageBytes = 1u << 20;
constexpr size_t kMaxBacklogBytes = 256u * 1024u;

}  // namespace

WsClient::~WsClient() { Close(); }

bool WsClient::ParseUrl(const std::string& url, std::string* host,
                        uint16_t* port, std::string* path,
                        std::string* error) {
    std::string rest;
    if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
    } else if (url.rfind("wss://", 0) == 0) {
        *error = "wss:// is not supported (no TLS in this build)";
        return false;
    } else {
        *error = "the lobby URL must start with ws://";
        return false;
    }
    const size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    *path = slash == std::string::npos ? "/" : rest.substr(slash);
    *port = 80;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        const int p = std::atoi(hostport.c_str() + colon + 1);
        if (p <= 0 || p > 65535) {
            *error = "bad port in the lobby URL";
            return false;
        }
        *port = static_cast<uint16_t>(p);
        hostport.resize(colon);
    }
    if (hostport.empty()) {
        *error = "no host in the lobby URL";
        return false;
    }
    *host = hostport;
    return true;
}

bool WsClient::Connect(const std::string& url, int timeout_ms,
                       std::string* error) {
    Close();
    std::string host, path;
    uint16_t port = 0;
    if (!ParseUrl(url, &host, &port, &path, error)) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    auto left_ms = [&] {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        return left > 0 ? static_cast<int>(left) : 0;
    };

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res) != 0 ||
        !res) {
        *error = "cannot resolve " + host;
        return false;
    }
    Socket s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == kInvalidSocket) {
        freeaddrinfo(res);
        *error = "socket() failed";
        return false;
    }
    SetNonBlocking(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));
    const int rc = connect(s, res->ai_addr,
                           static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (rc != 0 && !WouldBlock(SocketError())) {
        SocketClose(s);
        *error = "cannot connect to " + host;
        return false;
    }
    {
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        timeval tv;
        const int ms = left_ms();
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        if (select(static_cast<int>(s) + 1, nullptr, &wr, &ex, &tv) <= 0 ||
            FD_ISSET(s, &ex)) {
            SocketClose(s);
            *error = "cannot connect to " + host;
            return false;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(s, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&so_error), &len);
        if (so_error != 0) {
            SocketClose(s);
            *error = "cannot connect to " + host;
            return false;
        }
    }
    socket_ = static_cast<intptr_t>(s);

    // Upgrade.
    std::random_device rd;
    uint8_t raw_key[16];
    for (uint8_t& b : raw_key) b = static_cast<uint8_t>(rd());
    static const char* kB64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string key;
    for (int i = 0; i < 16; i += 3) {
        const uint32_t v = (raw_key[i] << 16) |
                           (i + 1 < 16 ? raw_key[i + 1] << 8 : 0) |
                           (i + 2 < 16 ? raw_key[i + 2] : 0);
        key.push_back(kB64[(v >> 18) & 63]);
        key.push_back(kB64[(v >> 12) & 63]);
        key.push_back(i + 1 < 16 ? kB64[(v >> 6) & 63] : '=');
        key.push_back(i + 2 < 16 ? kB64[v & 63] : '=');
    }
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + port_text +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key +
        "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    tx_.assign(request.begin(), request.end());
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (!Flush()) {
            *error = "connection lost during the upgrade";
            Close();
            return false;
        }
        const int ms = left_ms();
        if (ms == 0 || WaitSocket(s, !tx_.empty(), ms) < 0) {
            *error = "no answer to the upgrade";
            Close();
            return false;
        }
        char buf[2048];
        const int got = recv(s, buf, sizeof(buf), 0);
        if (got == 0 || (got < 0 && !WouldBlock(SocketError()))) {
            *error = "connection closed during the upgrade";
            Close();
            return false;
        }
        if (got > 0) head.append(buf, static_cast<size_t>(got));
        if (head.size() > 16384) {
            *error = "oversized upgrade answer";
            Close();
            return false;
        }
    }
    const size_t end = head.find("\r\n\r\n") + 4;
    rx_.assign(head.begin() + static_cast<std::ptrdiff_t>(end), head.end());
    head.resize(end);
    if (head.find(" 101 ") == std::string::npos) {
        *error = "upgrade refused: " + head.substr(0, head.find("\r\n"));
        Close();
        return false;
    }
    char accept[32];
    if (rnet_ws_accept_key(key.c_str(), accept) != 0 ||
        head.find(accept) == std::string::npos) {
        *error = "bad Sec-WebSocket-Accept";
        Close();
        return false;
    }
    return true;
}

void WsClient::Close() {
    if (socket_ != -1) SocketClose(AsSocket(socket_));
    socket_ = -1;
    rx_.clear();
    tx_.clear();
    fragment_.clear();
    in_fragment_ = false;
}

bool WsClient::IsOpen() const { return socket_ != -1; }

bool WsClient::Flush() {
    while (!tx_.empty()) {
        const int sent = send(AsSocket(socket_),
                              reinterpret_cast<const char*>(tx_.data()),
                              static_cast<int>(tx_.size()), 0);
        if (sent < 0) return WouldBlock(SocketError());
        tx_.erase(tx_.begin(), tx_.begin() + sent);
    }
    return true;
}

bool WsClient::SendFrame(uint8_t opcode, const uint8_t* data, size_t len) {
    if (!IsOpen()) return false;
    if (tx_.size() + len + 16 > kMaxBacklogBytes) return false;
    static thread_local std::mt19937 rng{std::random_device{}()};
    tx_.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (len < 126) {
        tx_.push_back(static_cast<uint8_t>(0x80 | len));
    } else if (len < 65536) {
        tx_.push_back(0x80 | 126);
        tx_.push_back(static_cast<uint8_t>(len >> 8));
        tx_.push_back(static_cast<uint8_t>(len));
    } else {
        tx_.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i)
            tx_.push_back(static_cast<uint8_t>(uint64_t(len) >> (8 * i)));
    }
    const uint32_t mask = rng();
    uint8_t m[4];
    for (int i = 0; i < 4; ++i) {
        m[i] = static_cast<uint8_t>(mask >> (8 * i));
        tx_.push_back(m[i]);
    }
    for (size_t i = 0; i < len; ++i) tx_.push_back(data[i] ^ m[i & 3]);
    return Flush();
}

bool WsClient::SendText(const std::string& text) {
    return SendFrame(0x1, reinterpret_cast<const uint8_t*>(text.data()),
                     text.size());
}

bool WsClient::ParseFrames(std::vector<std::string>* messages,
                           std::string* error) {
    for (;;) {
        if (rx_.size() < 2) return true;
        const uint8_t b0 = rx_[0], b1 = rx_[1];
        const bool fin = (b0 & 0x80) != 0;
        const uint8_t opcode = b0 & 0x0F;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7F;
        size_t off = 2;
        if (len == 126) {
            if (rx_.size() < 4) return true;
            len = (uint64_t(rx_[2]) << 8) | rx_[3];
            off = 4;
        } else if (len == 127) {
            if (rx_.size() < 10) return true;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | rx_[2 + i];
            off = 10;
        }
        if (len > kMaxMessageBytes) {
            *error = "oversized message from the lobby";
            return false;
        }
        uint8_t m[4] = {0, 0, 0, 0};
        if (masked) {
            if (rx_.size() < off + 4) return true;
            std::memcpy(m, rx_.data() + off, 4);
            off += 4;
        }
        if (rx_.size() < off + len) return true;
        std::string payload(rx_.begin() + static_cast<std::ptrdiff_t>(off),
                            rx_.begin() + static_cast<std::ptrdiff_t>(off + len));
        if (masked)
            for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= m[i & 3];
        rx_.erase(rx_.begin(),
                  rx_.begin() + static_cast<std::ptrdiff_t>(off + len));

        switch (opcode) {
        case 0x0:  // continuation
            if (!in_fragment_) break;
            fragment_ += payload;
            if (fragment_.size() > kMaxMessageBytes) {
                *error = "oversized message from the lobby";
                return false;
            }
            if (fin) {
                messages->push_back(std::move(fragment_));
                fragment_.clear();
                in_fragment_ = false;
            }
            break;
        case 0x1:  // text
            if (fin) {
                messages->push_back(std::move(payload));
            } else {
                fragment_ = std::move(payload);
                in_fragment_ = true;
            }
            break;
        case 0x8:  // close
            SendFrame(0x8, nullptr, 0);
            *error = "the lobby server closed the connection";
            return false;
        case 0x9:  // ping
            SendFrame(0xA, reinterpret_cast<const uint8_t*>(payload.data()),
                      payload.size());
            break;
        default:  // pong, binary: nothing to do
            break;
        }
    }
}

bool WsClient::Poll(int timeout_ms, std::vector<std::string>* messages,
                    std::string* error) {
    if (!IsOpen()) {
        *error = "not connected";
        return false;
    }
    if (!Flush()) {
        *error = "connection lost";
        Close();
        return false;
    }
    const Socket s = AsSocket(socket_);
    const int ready = WaitSocket(s, !tx_.empty(), timeout_ms);
    if (ready < 0) {
        *error = "connection lost";
        Close();
        return false;
    }
    for (;;) {
        char buf[16384];
        const int got = recv(s, buf, sizeof(buf), 0);
        if (got > 0) {
            rx_.insert(rx_.end(), buf, buf + got);
            continue;
        }
        if (got == 0) {
            *error = "the lobby server closed the connection";
            Close();
            return false;
        }
        if (!WouldBlock(SocketError())) {
            *error = "connection lost";
            Close();
            return false;
        }
        break;
    }
    if (!ParseFrames(messages, error) || !Flush()) {
        if (error->empty()) *error = "connection lost";
        Close();
        return false;
    }
    return true;
}

}  // namespace mpnet
