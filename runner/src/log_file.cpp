#include "log_file.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
// Not <io.h>: runner/src/io.h shadows it.
#include <corecrt_io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// rexglue's log_max_file_size_mb and log_max_files defaults.
constexpr size_t kMaxFileSize = 5u * 1024u * 1024u;
constexpr int kMaxFiles = 20;
// Cap on what is held before the file opens.
constexpr size_t kMaxPending = 4u * 1024u * 1024u;

#if defined(_WIN32)
// spdlog's SPDLOG_EOL, which rexglue's files use.
constexpr std::string_view kEol = "\r\n";

bool make_pipe(int fds[2]) {
    // Not inheritable: a child holding the read end would keep it alive.
    return _pipe(fds, 64 * 1024, _O_BINARY | _O_NOINHERIT) == 0;
}
int dup_fd(int fd) { return _dup(fd); }
bool dup_fd_to(int fd, int target) { return _dup2(fd, target) == 0; }
void close_fd(int fd) { _close(fd); }
int read_fd(int fd, char* data, unsigned size) {
    return _read(fd, data, size);
}
int write_fd(int fd, const char* data, unsigned size) {
    return _write(fd, data, size);
}
#else
constexpr std::string_view kEol = "\n";

bool make_pipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    return true;
}
int dup_fd(int fd) { return dup(fd); }
bool dup_fd_to(int fd, int target) { return dup2(fd, target) == target; }
void close_fd(int fd) { close(fd); }
int read_fd(int fd, char* data, unsigned size) {
    return static_cast<int>(read(fd, data, size));
}
int write_fd(int fd, const char* data, unsigned size) {
    return static_cast<int>(write(fd, data, size));
}
#endif

// Heap-allocated and never freed: the reader may outlive static destruction
// when a child process still holds the pipe at exit.
struct LogState {
    std::mutex mutex;
    std::FILE* file = nullptr;
    fs::path path;
    size_t file_size = 0;
    std::string pending;
    bool at_line_start = true;
    std::string debugger_line;
    int console_fd = -1;  // the original stderr, -1 when there was none
    std::thread reader;
    bool reader_done = false;
    bool abandoned = false;  // exit stopped waiting for the reader
    uint64_t chunks_read = 0;
    std::condition_variable reader_done_cv;
#if defined(_WIN32)
    HANDLE saved_stderr_handle = nullptr;
#endif
};

LogState* g_state = nullptr;

std::FILE* open_append(const fs::path& path) {
#if defined(_WIN32)
    return _wfopen(path.c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

// rexglue's NextSequentialLogPath.
fs::path next_sequential_path(const fs::path& dir, const std::string& app) {
    std::error_code error;
    fs::create_directories(dir, error);
    int max_seq = 0;
    const std::string prefix = app + "_";
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (!entry.is_regular_file(error)) continue;
        const std::string stem = entry.path().stem().string();
        if (stem.rfind(prefix, 0) != 0) continue;
        const std::string number = stem.substr(prefix.size());
        int value = 0;
        const char* end = number.data() + number.size();
        const auto [ptr, parse_error] =
            std::from_chars(number.data(), end, value);
        if (parse_error == std::errc() && ptr == end && value > max_seq)
            max_seq = value;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "_%03d.log", max_seq + 1);
    return dir / (app + name);
}

// spdlog's rotating_file_sink names: base.log, base.1.log, base.2.log, ...
fs::path rotated_path(const fs::path& base, int index) {
    if (index == 0) return base;
    fs::path path = base;
    path.replace_filename(base.stem().string() + "." + std::to_string(index) +
                          base.extension().string());
    return path;
}

void rotate(LogState& state) {
    std::fclose(state.file);
    state.file = nullptr;
    std::error_code error;
    for (int i = kMaxFiles; i > 0; --i) {
        const fs::path source = rotated_path(state.path, i - 1);
        if (!fs::exists(source, error)) continue;
        const fs::path target = rotated_path(state.path, i);
        fs::remove(target, error);
        fs::rename(source, target, error);
    }
    state.file = open_append(state.path);
    state.file_size = 0;
}

// spdlog's "[%Y-%m-%d %H:%M:%S.%e] ", in local time.
std::string timestamp_prefix() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[40];
    std::snprintf(text, sizeof(text), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, ms);
    return text;
}

void emit(LogState& state, const std::string& text, bool starts_line) {
    if (!state.file) {
        if (state.pending.size() + text.size() <= kMaxPending)
            state.pending += text;
        return;
    }
    // Rotate between lines only, as spdlog does between messages.
    if (starts_line && state.file_size > 0 &&
        state.file_size + text.size() > kMaxFileSize) {
        rotate(state);
        if (!state.file) return;
    }
    std::fwrite(text.data(), 1, text.size(), state.file);
    state.file_size += text.size();
}

void to_debugger(LogState& state, std::string_view segment, bool line_end) {
#if defined(_WIN32)
    // msvc_sink's default: only while a debugger is attached.
    if (!IsDebuggerPresent()) {
        state.debugger_line.clear();
        return;
    }
    state.debugger_line.append(segment);
    if (!line_end) return;
    state.debugger_line += '\n';
    OutputDebugStringA(state.debugger_line.c_str());
    state.debugger_line.clear();
#else
    (void)state;
    (void)segment;
    (void)line_end;
#endif
}

void log_bytes(LogState& state, const char* data, size_t size) {
    size_t at = 0;
    while (at < size) {
        const void* newline = std::memchr(data + at, '\n', size - at);
        const size_t end = newline
            ? static_cast<size_t>(static_cast<const char*>(newline) - data)
            : size;
        std::string_view segment(data + at, end - at);
        if (newline && !segment.empty() && segment.back() == '\r')
            segment.remove_suffix(1);
        const bool starts_line = state.at_line_start;
        std::string text;
        if (starts_line) text = timestamp_prefix();
        text.append(segment);
        if (newline) text.append(kEol);
        emit(state, text, starts_line);
        to_debugger(state, segment, newline != nullptr);
        state.at_line_start = newline != nullptr;
        at = newline ? end + 1 : size;
    }
}

void reader_main(LogState* state, int fd) {
    char buffer[4096];
    for (;;) {
        const int got = read_fd(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (state->console_fd >= 0) {
            int written = 0;
            while (written < got) {
                const int n = write_fd(state->console_fd, buffer + written,
                                       static_cast<unsigned>(got - written));
                if (n <= 0) break;
                written += n;
            }
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->abandoned) return;
        log_bytes(*state, buffer, static_cast<size_t>(got));
        if (state->file) std::fflush(state->file);
        ++state->chunks_read;
        state->reader_done_cv.notify_all();
    }
    close_fd(fd);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->reader_done = true;
    state->reader_done_cv.notify_all();
}

void capture_end() {
    LogState* state = g_state;
    if (!state) return;
    std::fflush(stderr);
#if defined(_WIN32)
    SetStdHandle(STD_ERROR_HANDLE, state->saved_stderr_handle);
#endif
    // Point fd 2 back at the original stderr, which closes this process's
    // write end of the pipe, and let the reader drain it.
    if (state->console_fd >= 0)
        dup_fd_to(state->console_fd, 2);
    else
        close_fd(2);
    std::unique_lock<std::mutex> lock(state->mutex);
    // Wait as long as the reader makes progress. Idle for a second means a
    // child process that inherited stderr still holds the pipe open.
    bool drained = state->reader_done;
    while (!drained) {
        const uint64_t chunks = state->chunks_read;
        state->reader_done_cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return state->reader_done || state->chunks_read != chunks;
        });
        drained = state->reader_done;
        if (!drained && state->chunks_read == chunks) break;
    }
    if (!drained) state->abandoned = true;
    lock.unlock();
    if (drained)
        state->reader.join();
    else
        state->reader.detach();
    lock.lock();
    if (state->file) {
        std::fclose(state->file);
        state->file = nullptr;
    }
}

}  // namespace

void nds_log_capture_begin() {
    if (g_state) return;
    std::fflush(stderr);
    int fds[2];
    if (!make_pipe(fds)) return;
    auto* state = new LogState();
    state->console_fd = dup_fd(2);
    if (!dup_fd_to(fds[1], 2)) {
        close_fd(fds[0]);
        close_fd(fds[1]);
        if (state->console_fd >= 0) close_fd(state->console_fd);
        delete state;
        return;
    }
    close_fd(fds[1]);
#if defined(_WIN32)
    // Native writers (SDL, drivers) use the Win32 handle, not fd 2.
    state->saved_stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_ERROR_HANDLE,
                 reinterpret_cast<HANDLE>(_get_osfhandle(2)));
#endif
    state->reader = std::thread(reader_main, state, fds[0]);
    g_state = state;
    // Registered first, so it runs after main's other atexit handlers. Not
    // any later: the reader must be done before the CRT shuts down its I/O
    // (it crashed in _read when ended from a .CRT$XT terminator). What
    // global destructors print still reaches stderr, just not the file.
    std::atexit(capture_end);
}

bool nds_log_file_open(const std::string& explicit_path, const fs::path& dir,
                       const std::string& app_name) {
    LogState* state = g_state;
    if (!state) return false;
    std::error_code error;
    const fs::path path = explicit_path.empty()
        ? next_sequential_path(dir, app_name)
        : fs::path(explicit_path);
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), error);
    std::FILE* file = open_append(path);
    if (!file) {
        std::fprintf(stderr, "[log] cannot open %s\n", path.string().c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->file) std::fclose(state->file);
        state->file = file;
        state->path = path;
        const auto size = fs::file_size(path, error);
        state->file_size = error ? 0 : static_cast<size_t>(size);
        if (!state->pending.empty()) {
            std::fwrite(state->pending.data(), 1, state->pending.size(), file);
            std::fflush(file);
            state->file_size += state->pending.size();
            std::string().swap(state->pending);
        }
    }
    std::fprintf(stderr, "[log] writing %s\n", path.string().c_str());
    return true;
}

std::string nds_log_file_path() {
    LogState* state = g_state;
    if (!state) return {};
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->path.string();
}
