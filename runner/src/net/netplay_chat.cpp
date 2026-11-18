// netplay_chat.cpp -- see netplay_chat.h.

#include "net/netplay_chat.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

#if NDS_ONLINE
#include "recomp_net/auth.h"
#include "recomp_net/chat_filter.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;

// A line waits this long for its transport to pick it up, then goes.
constexpr auto kOutboxLife = std::chrono::seconds(5);
constexpr size_t kOutboxMax = 16;

struct Outgoing {
    Clock::time_point at;
    std::string from;
    std::string text;
};

struct ChatState {
    std::mutex mu;
    std::deque<NdsChatLine> lines;
    uint64_t next_id = 1;
    std::string name;
    std::deque<Outgoing> lan_out;
    std::deque<Outgoing> online_out;
};

ChatState& State() {
    static ChatState state;
    return state;
}

std::atomic<int> g_lan_peers{0};
std::atomic<bool> g_online_room{false};

// Length of the UTF-8 sequence starting at s[i], 0 when malformed.
size_t SequenceLength(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) len = 3;
    else if (c >= 0xF0 && c <= 0xF4) len = 4;
    else return 0;
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
    return len;
}

void Push(ChatState& st, NdsChatLine&& line) {
    line.id = st.next_id++;
    line.at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     Clock::now().time_since_epoch()).count();
    st.lines.push_back(std::move(line));
    while (st.lines.size() > kNdsChatKeep) st.lines.pop_front();
}

void Mask(std::string* text) {
#if NDS_ONLINE
    if (text->empty()) return;
    std::vector<char> buf(text->begin(), text->end());
    buf.push_back('\0');
    rnet_chat_filter_apply(buf.data(), buf.size());
    text->assign(buf.data());
#else
    (void)text;
#endif
}

std::string DefaultName() {
#if NDS_ONLINE
    const char* handle = rnet_account_handle();
    if (rnet_account_state() == RNET_ACCOUNT_SIGNED_IN && handle && handle[0])
        return nds_netplay_chat_clean(handle, kNdsChatNameMax);
#endif
    return "Player";
}

bool TakeFresh(std::deque<Outgoing>* queue, Outgoing* out) {
    const auto now = Clock::now();
    while (!queue->empty()) {
        Outgoing o = std::move(queue->front());
        queue->pop_front();
        if (now - o.at <= kOutboxLife) {
            *out = std::move(o);
            return true;
        }
    }
    return false;
}

void Enqueue(std::deque<Outgoing>* queue, Outgoing o) {
    queue->push_back(std::move(o));
    while (queue->size() > kOutboxMax) queue->pop_front();
}

}  // namespace

std::string nds_netplay_chat_clean(const std::string& text, size_t max_bytes) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const size_t len = SequenceLength(text, i);
        if (len == 0) {  // malformed byte: dropped
            ++i;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (len == 1 && (c < 0x20 || c == 0x7F)) {
            // A line break or tab becomes a space, other controls go.
            if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        } else if (len == 2 && c == 0xC2 &&
                   static_cast<unsigned char>(text[i + 1]) < 0xA0) {
            // C1 controls (U+0080..U+009F): dropped.
        } else {
            if (out.size() + len > max_bytes) break;
            out.append(text, i, len);
        }
        i += len;
    }
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    const size_t last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

NdsChatSendResult nds_netplay_chat_send(const std::string& text) {
    const std::string clean = nds_netplay_chat_clean(text, kNdsChatTextMax);
    if (clean.empty()) return NdsChatSendResult::Empty;
    const bool online = g_online_room.load(std::memory_order_acquire);
    const bool lan = g_lan_peers.load(std::memory_order_acquire) > 0;
    if (!online && !lan) return NdsChatSendResult::NoOne;

    const std::string name = nds_netplay_chat_name();
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    const auto now = Clock::now();
    if (online) Enqueue(&st.online_out, Outgoing{now, name, clean});
    if (lan) Enqueue(&st.lan_out, Outgoing{now, name, clean});
    // The server sends an online line back to its sender; a LAN-only line
    // is listed here.
    if (!online) {
        NdsChatLine line;
        line.source = NdsChatSource::Lan;
        line.self = true;
        line.from = name;
        line.text = clean;
        Mask(&line.text);
        Push(st, std::move(line));
    }
    return NdsChatSendResult::Sent;
}

std::vector<NdsChatLine> nds_netplay_chat_since(uint64_t after) {
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    std::vector<NdsChatLine> out;
    for (const NdsChatLine& line : st.lines)
        if (line.id > after) out.push_back(line);
    return out;
}

uint64_t nds_netplay_chat_last_id() {
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    return st.next_id - 1;
}

void nds_netplay_chat_clear() {
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    st.lines.clear();
}

void nds_netplay_chat_set_name(const std::string& name) {
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    st.name = nds_netplay_chat_clean(name, kNdsChatNameMax);
}

std::string nds_netplay_chat_name() {
    {
        ChatState& st = State();
        std::lock_guard<std::mutex> lock(st.mu);
        if (!st.name.empty()) return st.name;
    }
    return DefaultName();
}

bool nds_netplay_chat_reachable() {
    return g_online_room.load(std::memory_order_acquire) ||
           g_lan_peers.load(std::memory_order_acquire) > 0;
}

void nds_netplay_chat_push(NdsChatLine line) {
    line.from = nds_netplay_chat_clean(line.from, kNdsChatNameMax);
    line.text = nds_netplay_chat_clean(line.text, kNdsChatTextMax);
    if (line.text.empty()) return;
    // Server lines were masked by the server already; masking again covers
    // an older server, and LAN lines have had nothing else.
    Mask(&line.text);
    Mask(&line.from);
    std::fprintf(stderr, "[chat] %s%s%s: %zu bytes\n",
                 line.source == NdsChatSource::Online ? "online "
                 : line.source == NdsChatSource::Lan  ? "lan "
                                                      : "system",
                 line.self ? "(self) " : "",
                 line.source == NdsChatSource::System ? "" : line.from.c_str(),
                 line.text.size());
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    Push(st, std::move(line));
}

void nds_netplay_chat_set_lan_peers(int count) {
    const int before = g_lan_peers.exchange(count, std::memory_order_acq_rel);
    if (before > 0 && count == 0) {
        ChatState& st = State();
        std::lock_guard<std::mutex> lock(st.mu);
        st.lan_out.clear();
    }
}

bool nds_netplay_chat_take_lan(std::string* from, std::string* text) {
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    Outgoing o;
    if (!TakeFresh(&st.lan_out, &o)) return false;
    *from = std::move(o.from);
    *text = std::move(o.text);
    return true;
}

void nds_netplay_chat_set_online_room(bool in_room) {
    const bool before = g_online_room.exchange(in_room,
                                               std::memory_order_acq_rel);
    if (before && !in_room) {
        ChatState& st = State();
        std::lock_guard<std::mutex> lock(st.mu);
        st.online_out.clear();
    }
}

bool nds_netplay_chat_take_online(std::string* text) {
    ChatState& st = State();
    std::lock_guard<std::mutex> lock(st.mu);
    Outgoing o;
    if (!TakeFresh(&st.online_out, &o)) return false;
    *text = std::move(o.text);
    return true;
}
