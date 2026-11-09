// mp_link.cpp -- see mp_link.h.

#include "net/mp_link.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
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
// Nothing left to read, or a peer port that is not bound yet.
bool SocketDrained(int err) {
    return err == WSAEWOULDBLOCK || err == WSAECONNRESET;
}
bool SocketPeerGone(int err) { return err == WSAECONNRESET; }
// Sending to an address nobody listens on; not worth a log line.
bool SocketUnreachable(int err) {
    return err == WSAEWOULDBLOCK || err == WSAECONNRESET ||
           err == WSAEHOSTUNREACH || err == WSAENETUNREACH ||
           err == WSAEADDRNOTAVAIL;
}
void SocketClose(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
int SocketError() { return errno; }
bool SocketDrained(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == ECONNREFUSED ||
           err == EINTR;
}
bool SocketPeerGone(int err) { return err == ECONNREFUSED || err == EINTR; }
bool SocketUnreachable(int err) {
    return err == EAGAIN || err == EWOULDBLOCK || err == ECONNREFUSED ||
           err == EHOSTUNREACH || err == ENETUNREACH ||
           err == EADDRNOTAVAIL || err == EACCES;
}
void SocketClose(Socket s) { ::close(s); }
#endif

sockaddr_in ToSockaddr(const PeerAddr& addr) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(addr.ip);
    out.sin_port = htons(addr.port);
    return out;
}

class UdpMpLink final : public MpLink {
public:
    explicit UdpMpLink(const UdpLinkConfig& config) : config_(config) {}
    ~UdpMpLink() override { Close(); }

    bool Open() override {
        if (socket_ != kInvalidSocket) return true;
        Socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kInvalidSocket) {
            std::fprintf(stderr, "[local_mp] socket failed: %d\n",
                         SocketError());
            return false;
        }
#ifdef _WIN32
        // Sending to a port nobody has bound yet must not make the next
        // recvfrom fail with WSAECONNRESET.
        BOOL reset_enabled = FALSE;
        DWORD bytes_returned = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &reset_enabled, sizeof(reset_enabled),
                 nullptr, 0, &bytes_returned, nullptr, nullptr);
#endif
        if (config_.lan) {
            int on = 1;
            if (::setsockopt(s, SOL_SOCKET, SO_BROADCAST,
                             reinterpret_cast<const char*>(&on),
                             sizeof(on)) != 0) {
                std::fprintf(stderr,
                    "[local_mp] SO_BROADCAST failed: %d (LAN discovery "
                    "limited to --local-wireless-peer)\n", SocketError());
            }
        }
        const PeerAddr bind_addr{
            config_.lan ? INADDR_ANY : INADDR_LOOPBACK, OwnPort()};
        sockaddr_in addr = ToSockaddr(bind_addr);
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
            0) {
            std::fprintf(stderr, "[local_mp] bind %s:%u failed: %d\n",
                         config_.lan ? "0.0.0.0" : "127.0.0.1",
                         static_cast<unsigned>(OwnPort()), SocketError());
            SocketClose(s);
            return false;
        }
#ifdef _WIN32
        u_long nonblocking = 1;
        const bool nonblocking_ok = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
        const int flags = ::fcntl(s, F_GETFL, 0);
        const bool nonblocking_ok =
            flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
        if (!nonblocking_ok) {
            std::fprintf(stderr, "[local_mp] nonblocking setup failed: %d\n",
                         SocketError());
            SocketClose(s);
            return false;
        }
        socket_ = s;
        return true;
    }

    void Close() override {
        if (socket_ == kInvalidSocket) return;
        SocketClose(socket_);
        socket_ = kInvalidSocket;
    }

    bool IsOpen() const override { return socket_ != kInvalidSocket; }

    bool SendTo(const PeerAddr& to, const uint8_t* data,
                size_t len) override {
        if (socket_ == kInvalidSocket) return false;
        sockaddr_in target = ToSockaddr(to);
        if (::sendto(socket_, reinterpret_cast<const char*>(data),
                     static_cast<int>(len), 0,
                     reinterpret_cast<sockaddr*>(&target),
                     sizeof(target)) >= 0)
            return true;
        const int err = SocketError();
        if (!SocketUnreachable(err) && send_error_log_counter_ < 8u) {
            ++send_error_log_counter_;
            std::fprintf(stderr, "[local_mp] sendto %s failed: %d\n",
                         FormatPeerAddress(to).c_str(), err);
        }
        return false;
    }

    void SendDiscovery(const uint8_t* data, size_t len) override {
        for (uint32_t i = 0; i < kMaxInstances; ++i) {
            const uint16_t port = static_cast<uint16_t>(config_.base_port + i);
            if (i != config_.instance)
                SendTo(PeerAddr{INADDR_LOOPBACK, port}, data, len);
            // Another machine may run any instance index, this one too.
            if (config_.lan)
                SendTo(PeerAddr{INADDR_BROADCAST, port}, data, len);
        }
        for (const PeerAddr& peer : config_.peers) SendTo(peer, data, len);
    }

    int Recv(uint8_t* data, size_t capacity, PeerAddr* from) override {
        if (socket_ == kInvalidSocket) return 0;
        for (;;) {
            sockaddr_in addr{};
#ifdef _WIN32
            int addr_len = sizeof(addr);
#else
            socklen_t addr_len = sizeof(addr);
#endif
            const int got = static_cast<int>(::recvfrom(
                socket_, reinterpret_cast<char*>(data),
                static_cast<int>(capacity), 0,
                reinterpret_cast<sockaddr*>(&addr), &addr_len));
            if (got >= 0) {
                if (from) {
                    from->ip = ntohl(addr.sin_addr.s_addr);
                    from->port = ntohs(addr.sin_port);
                }
                // A zero-length datagram is not ours; keep reading.
                if (got == 0) continue;
                return got;
            }
            const int err = SocketError();
            // A stale ICMP port-unreachable error is consumed by this
            // call; datagrams queued behind it are still there.
            if (SocketPeerGone(err)) continue;
            if (SocketDrained(err)) return 0;
#ifdef _WIN32
            // Larger than our largest datagram: not ours, discarded.
            if (err == WSAEMSGSIZE) continue;
#endif
            std::fprintf(stderr, "[local_mp] recvfrom failed: %d\n", err);
            return -1;
        }
    }

    bool WaitReadable(int64_t timeout_us) override {
        if (socket_ == kInvalidSocket) return false;
        if (timeout_us < 0) timeout_us = 0;
#ifdef _WIN32
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout_us / 1000000);
        tv.tv_usec = static_cast<long>(timeout_us % 1000000);
        return ::select(0, &readable, nullptr, nullptr, &tv) > 0;
#else
        // poll() has millisecond resolution; round up so a short remaining
        // wait still waits instead of spinning.
        pollfd fd{};
        fd.fd = socket_;
        fd.events = POLLIN;
        const int timeout_ms = static_cast<int>((timeout_us + 999) / 1000);
        return ::poll(&fd, 1, timeout_ms) > 0 && (fd.revents & POLLIN) != 0;
#endif
    }

    std::string Describe() const override {
        char text[96];
        std::snprintf(text, sizeof(text), "%s %s:%u",
                      config_.lan ? "lan" : "loopback",
                      config_.lan ? "0.0.0.0" : "127.0.0.1",
                      static_cast<unsigned>(OwnPort()));
        std::string out = text;
        for (const PeerAddr& peer : config_.peers)
            out += " peer=" + FormatPeerAddress(peer);
        return out;
    }

private:
    uint16_t OwnPort() const {
        return static_cast<uint16_t>(config_.base_port + config_.instance);
    }

    UdpLinkConfig config_;
    Socket socket_ = kInvalidSocket;
    uint32_t send_error_log_counter_ = 0;
};

}  // namespace

std::unique_ptr<MpLink> MakeUdpLink(const UdpLinkConfig& config) {
    return std::make_unique<UdpMpLink>(config);
}

bool ParsePeerAddress(const std::string& text, uint16_t default_port,
                      PeerAddr* out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = "invalid peer \"" + text + "\": " + why;
        return false;
    };
    std::string host = text;
    uint16_t port = default_port;
    const size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        const std::string port_text = text.substr(colon + 1);
        char* end = nullptr;
        const long parsed = std::strtol(port_text.c_str(), &end, 10);
        if (port_text.empty() || !end || *end != '\0' || parsed < 1 ||
            parsed > 65535)
            return fail("port must be 1..65535");
        port = static_cast<uint16_t>(parsed);
    }
    if (host.empty()) return fail("missing host");

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 ||
        !result)
        return fail("cannot resolve host");
    const sockaddr_in* addr =
        reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    out->ip = ntohl(addr->sin_addr.s_addr);
    out->port = port;
    ::freeaddrinfo(result);
    return true;
}

std::string FormatPeerAddress(const PeerAddr& addr) {
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u",
                  static_cast<unsigned>(addr.ip >> 24),
                  static_cast<unsigned>((addr.ip >> 16) & 0xFF),
                  static_cast<unsigned>((addr.ip >> 8) & 0xFF),
                  static_cast<unsigned>(addr.ip & 0xFF),
                  static_cast<unsigned>(addr.port));
    return text;
}

}  // namespace mpnet
