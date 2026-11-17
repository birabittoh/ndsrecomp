#include "discord_rpc.h"

#if !defined(__ANDROID__) && (defined(_WIN32) || defined(__linux__) || \
                              defined(__APPLE__))

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace nds::discord {
namespace {

// Frame opcodes of Discord's IPC protocol: an 8-byte header (opcode and
// payload length, little-endian u32s) and a JSON payload.
enum : uint32_t { kOpHandshake = 0, kOpFrame = 1, kOpClose = 2, kOpPing = 3,
                  kOpPong = 4 };

// Discord allows 5 activity updates per 20 seconds: one per 4 s at most,
// the latest wins.
constexpr auto kSendInterval = std::chrono::seconds(4);
constexpr auto kRetryInterval = std::chrono::seconds(15);
constexpr auto kPollInterval = std::chrono::milliseconds(250);

class Connection {
public:
    ~Connection() { Close(); }

#if defined(_WIN32)
    bool Open() {
        for (int i = 0; i < 10; ++i) {
            char name[64];
            std::snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                pipe_ = pipe;
                return true;
            }
        }
        return false;
    }
    bool IsOpen() const { return pipe_ != INVALID_HANDLE_VALUE; }
    void Close() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    bool Write(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            DWORD written = 0;
            if (!WriteFile(pipe_, p, static_cast<DWORD>(size), &written,
                           nullptr) || written == 0)
                return false;
            p += written;
            size -= written;
        }
        return true;
    }
    bool Read(void* data, size_t size) {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            DWORD read = 0;
            if (!ReadFile(pipe_, p, static_cast<DWORD>(size), &read, nullptr) ||
                read == 0)
                return false;
            p += read;
            size -= read;
        }
        return true;
    }
    // -1 on a broken pipe.
    int Available() {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr))
            return -1;
        return avail >= 8 ? 1 : 0;
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
#else
    bool Open() {
        // Most specific first. macOS uses the per-user $TMPDIR; Flatpak and
        // Snap clients put the socket in app/ subdirectories.
        std::vector<std::string> dirs;
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
            if (const char* v = std::getenv(var); v && *v) dirs.push_back(v);
        dirs.push_back("/run/user/" + std::to_string(getuid()));
        dirs.push_back("/tmp");
        const size_t base = dirs.size();
        for (size_t i = 0; i < base; ++i) {
            dirs.push_back(dirs[i] + "/app/com.discordapp.Discord");
            dirs.push_back(dirs[i] + "/snap.discord");
        }
        for (const std::string& dir : dirs) {
            for (int i = 0; i < 10; ++i) {
                const std::string path =
                    dir + "/discord-ipc-" + std::to_string(i);
                sockaddr_un addr{};
                if (path.size() >= sizeof(addr.sun_path)) continue;
                addr.sun_family = AF_UNIX;
                std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
                const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) continue;
                if (connect(fd, reinterpret_cast<sockaddr*>(&addr),
                            sizeof(addr)) == 0) {
                    fd_ = fd;
                    return true;
                }
                close(fd);
            }
        }
        return false;
    }
    bool IsOpen() const { return fd_ >= 0; }
    void Close() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
    bool Write(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
#if defined(MSG_NOSIGNAL)
            const ssize_t n = send(fd_, p, size, MSG_NOSIGNAL);
#else
            const ssize_t n = write(fd_, p, size);
#endif
            if (n <= 0) return false;
            p += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    }
    bool Read(void* data, size_t size) {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            const ssize_t n = recv(fd_, p, size, 0);
            if (n <= 0) return false;
            p += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    }
    int Available() {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        timeval tv{0, 0};
        const int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    }

private:
    int fd_ = -1;
#endif

public:
    bool WriteFrame(uint32_t op, const std::string& payload) {
        const uint32_t header[2] = {op, static_cast<uint32_t>(payload.size())};
        return Write(header, sizeof(header)) &&
               (payload.empty() || Write(payload.data(), payload.size()));
    }
};

uint32_t ProcessId() {
#if defined(_WIN32)
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

std::string JsonString(const std::string& in) {
    std::string out = "\"";
    for (const char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                out += hex;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

// Discord rejects fields shorter than 2 or longer than 128 characters.
std::string Field(std::string text) {
    if (text.size() > 128) text.resize(128);
    if (text.size() == 1) text += ' ';
    return text;
}

// The SET_ACTIVITY command; a null activity clears it.
std::string ActivityPayload(const Activity* a, int64_t start, uint64_t nonce) {
    std::string activity = "null";
    if (a) {
        std::string fields;
        const auto add = [&](const char* key, const std::string& value) {
            if (value.empty()) return;
            if (!fields.empty()) fields += ',';
            fields += std::string("\"") + key + "\":" + JsonString(Field(value));
        };
        add("details", a->details);
        add("state", a->state);
        std::string assets;
        std::swap(fields, assets);
        add("large_image", a->large_image);
        add("large_text", a->large_text);
        std::swap(fields, assets);
        if (!fields.empty()) fields += ',';
        fields += "\"timestamps\":{\"start\":" + std::to_string(start) + "}";
        if (!assets.empty()) fields += ",\"assets\":{" + assets + "}";
        fields += ",\"instance\":false";
        activity = "{" + fields + "}";
    }
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
           std::to_string(ProcessId()) + ",\"activity\":" + activity +
           "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
}

// The value of a JSON string field in `json` ("" when absent). Enough for
// Discord's flat error messages.
std::string JsonField(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return "";
    const size_t begin = at + needle.size();
    const size_t end = json.find('"', begin);
    return json.substr(begin, end == std::string::npos ? end : end - begin);
}

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    bool running = false;
    std::string application_id;
    Activity activity;
    bool enabled = true;
};

State& state() {
    static State s;
    return s;
}

void Worker() {
    State& s = state();
    using Clock = std::chrono::steady_clock;
    const int64_t start = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string application_id;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        application_id = s.application_id;
    }

    Connection conn;
    bool ready = false;
    bool reported_missing = false;
    bool reported_error = false;
    // What Discord shows: nothing until the first send after READY.
    bool shown_valid = false;
    bool shown_enabled = false;
    Activity shown;
    Clock::time_point next_connect = Clock::now();
    Clock::time_point next_send = Clock::now();
    uint64_t nonce = 0;

    const auto drop = [&](const char* why) {
        if (ready) std::fprintf(stderr, "[discord] %s\n", why);
        conn.Close();
        ready = false;
        shown_valid = false;
        next_connect = Clock::now() + kRetryInterval;
    };

    std::unique_lock<std::mutex> lock(s.mutex);
    while (s.running) {
        const bool enabled = s.enabled;
        const Activity wanted = s.activity;
        lock.unlock();

        const auto now = Clock::now();
        bool give_up = false;
        if (!conn.IsOpen() && now >= next_connect) {
            if (!conn.Open()) {
                if (!reported_missing)
                    std::fprintf(stderr,
                                 "[discord] Discord is not running; looking "
                                 "for it every %llds\n",
                                 static_cast<long long>(kRetryInterval.count()));
                reported_missing = true;
                next_connect = now + kRetryInterval;
            } else if (!conn.WriteFrame(
                           kOpHandshake, "{\"v\":1,\"client_id\":" +
                                             JsonString(application_id) + "}")) {
                drop("connection lost");
            }
        }

        // Discord's messages: READY after the handshake, pings, replies to
        // SET_ACTIVITY (errors logged once) and a close with the reason.
        while (conn.IsOpen()) {
            const int avail = conn.Available();
            if (avail < 0) {
                drop("connection lost");
                break;
            }
            if (avail == 0) break;
            uint32_t header[2] = {0, 0};
            if (!conn.Read(header, sizeof(header)) || header[1] > (1u << 20)) {
                drop("connection lost");
                break;
            }
            std::string body(header[1], '\0');
            if (!body.empty() && !conn.Read(body.data(), body.size())) {
                drop("connection lost");
                break;
            }
            if (header[0] == kOpPing) {
                conn.WriteFrame(kOpPong, body);
            } else if (header[0] == kOpClose) {
                if (!ready) {
                    // Refused at the handshake: a bad application id, which
                    // retrying will not fix.
                    std::fprintf(stderr, "[discord] refused: %s\n",
                                 JsonField(body, "message").c_str());
                    give_up = true;
                }
                drop("Discord closed the connection");
                break;
            } else if (header[0] == kOpFrame) {
                if (body.find("\"evt\":\"READY\"") != std::string::npos) {
                    ready = true;
                    reported_missing = false;
                    reported_error = false;
                    std::fprintf(stderr, "[discord] connected\n");
                } else if (body.find("\"evt\":\"ERROR\"") != std::string::npos &&
                           !reported_error) {
                    reported_error = true;
                    std::fprintf(stderr, "[discord] activity refused: %s\n",
                                 JsonField(body, "message").c_str());
                }
            }
        }
        if (give_up) break;

        if (ready && Clock::now() >= next_send &&
            (!shown_valid || enabled != shown_enabled ||
             (enabled && !(wanted == shown)))) {
            if (conn.WriteFrame(kOpFrame,
                                ActivityPayload(enabled ? &wanted : nullptr,
                                                start, ++nonce))) {
                shown_valid = true;
                shown_enabled = enabled;
                shown = wanted;
                next_send = Clock::now() + kSendInterval;
            } else {
                drop("connection lost");
            }
        }

        lock.lock();
        s.wake.wait_for(lock, kPollInterval, [&] { return !s.running; });
    }
    if (lock.owns_lock()) lock.unlock();

    // Clear the activity rather than leave it to Discord's timeout.
    if (conn.IsOpen() && ready) {
        conn.WriteFrame(kOpFrame, ActivityPayload(nullptr, start, ++nonce));
        conn.WriteFrame(kOpClose, "{}");
    }
    conn.Close();
}

}  // namespace

void Start(const std::string& application_id, const Activity& initial) {
    if (application_id.empty()) return;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.activity = initial;
    if (s.running || s.thread.joinable()) return;
    s.application_id = application_id;
    s.running = true;
    s.thread = std::thread(Worker);
}

void SetActivity(const Activity& activity) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.activity = activity;
}

void SetEnabled(bool enabled) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.enabled = enabled;
}

void Stop() {
    State& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.running = false;
    }
    s.wake.notify_all();
    if (s.thread.joinable()) s.thread.join();
}

}  // namespace nds::discord

#else

namespace nds::discord {
void Start(const std::string&, const Activity&) {}
void SetActivity(const Activity&) {}
void SetEnabled(bool) {}
void Stop() {}
}  // namespace nds::discord

#endif
