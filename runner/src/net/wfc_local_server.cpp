#include "net/wfc_local_server.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "runtime_arm.h"
#include "title_dls.h"

namespace {

constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08,
                  kAck = 0x10;
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kOurMss = 1400;

uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t Get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}
void Put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void Put32(uint8_t* p, uint32_t v) {
    Put16(p, v >> 16);
    Put16(p + 2, v);
}

uint32_t SumWords(const uint8_t* p, size_t n, uint32_t sum) {
    for (size_t i = 0; i + 1 < n; i += 2) sum += static_cast<uint32_t>(p[i] << 8 | p[i + 1]);
    if (n & 1u) sum += static_cast<uint32_t>(p[n - 1] << 8);
    return sum;
}
uint16_t Fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

// An IPv4 frame for the guest: Ethernet + IP header around `l4` (whose
// checksum, at `csum_at` within it, is filled in here).
std::vector<uint8_t> Ipv4Frame(const uint8_t dst_mac[6], const uint8_t src_mac[6],
                               const uint8_t src_ip[4], const uint8_t dst_ip[4],
                               uint8_t proto, std::vector<uint8_t> l4,
                               size_t csum_at) {
    static uint16_t ip_id = 1;
    const size_t l4n = l4.size();
    uint32_t pseudo = SumWords(src_ip, 4, 0);
    pseudo = SumWords(dst_ip, 4, pseudo);
    pseudo += proto + static_cast<uint32_t>(l4n);
    l4[csum_at] = l4[csum_at + 1] = 0;
    uint16_t csum = Fold(SumWords(l4.data(), l4n, pseudo));
    if (proto == 17 && csum == 0) csum = 0xFFFF;
    Put16(&l4[csum_at], csum);

    std::vector<uint8_t> f(14 + 20 + l4n);
    std::memcpy(&f[0], dst_mac, 6);
    std::memcpy(&f[6], src_mac, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    uint8_t* ip = &f[14];
    ip[0] = 0x45;
    Put16(ip + 2, static_cast<uint32_t>(20 + l4n));
    Put16(ip + 4, ip_id++);
    ip[6] = 0x40;  // don't fragment
    ip[8] = 64;
    ip[9] = proto;
    std::memcpy(ip + 12, src_ip, 4);
    std::memcpy(ip + 16, dst_ip, 4);
    Put16(ip + 10, Fold(SumWords(ip, 20, 0)));
    std::memcpy(&f[34], l4.data(), l4n);
    return f;
}

bool EndsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
            return false;
    }
    return true;
}

// ---- Nintendo's form encoding: base64 with '.', '-' for '+', '/' and '*'
// for '=' padding (the plain alphabet is accepted too).
int B64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '.' || c == '+') return 62;
    if (c == '-' || c == '/') return 63;
    return -1;
}
std::string B64Decode(const std::string& in) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = B64Value(c);
        if (v < 0) continue;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFFu));
        }
    }
    return out;
}
std::string B64Encode(const std::string& in) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const size_t n = std::min<size_t>(3, in.size() - i);
        uint32_t v = static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << 16;
        if (n > 1) v |= static_cast<uint32_t>(static_cast<uint8_t>(in[i + 1])) << 8;
        if (n > 2) v |= static_cast<uint8_t>(in[i + 2]);
        out.push_back(kAlphabet[v >> 18 & 63]);
        out.push_back(kAlphabet[v >> 12 & 63]);
        out.push_back(n > 1 ? kAlphabet[v >> 6 & 63] : '*');
        out.push_back(n > 2 ? kAlphabet[v & 63] : '*');
    }
    return out;
}

// A field of an x-www-form-urlencoded body, base64-decoded ("" if missing).
std::string FormField(const std::string& body, const char* key) {
    const size_t kl = std::strlen(key);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('&', pos);
        if (end == std::string::npos) end = body.size();
        if (end - pos > kl && body.compare(pos, kl, key) == 0 && body[pos + kl] == '=') {
            std::string raw;
            for (size_t i = pos + kl + 1; i < end; ++i) {
                const char c = body[i];
                if (c == '%' && i + 2 < end) {
                    raw.push_back(static_cast<char>(
                        std::strtol(body.substr(i + 1, 2).c_str(), nullptr, 16)));
                    i += 2;
                } else if (c != '\r' && c != '\n') {
                    raw.push_back(c);
                }
            }
            return B64Decode(raw);
        }
        pos = end + 1;
    }
    return {};
}

void AddField(std::string* out, const char* key, const std::string& value) {
    if (!out->empty()) out->push_back('&');
    *out += key;
    out->push_back('=');
    *out += B64Encode(value);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::tm UtcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    return tm;
}

void Log(const char* fmt, const char* a = "", const char* b = "",
         const char* c = "") {
    std::fprintf(stderr, "[wfc] ");
    std::fprintf(stderr, fmt, a, b, c);
    std::fprintf(stderr, "\n");
}

}  // namespace

bool NdsWfcLocalServer::HandleTx(const uint8_t* frame, int len,
                                 std::vector<std::vector<uint8_t>>* out) {
    if (len < 34 || frame[12] != 0x08 || frame[13] != 0x00) return false;
    const uint8_t* ip = frame + 14;
    if ((ip[0] >> 4) != 4) return false;
    const int ihl = (ip[0] & 15) * 4;
    if (len < 14 + ihl + 8) return false;
    if (ip[9] == 17 && Get16(ip + ihl + 2) == 53) return HandleDns(frame, len, out);
    if (ip[9] == 6 && std::memcmp(ip + 16, kServerIp, 4) == 0)
        return HandleTcp(frame, len, out);
    return false;
}

// ---------------------------------------------------------------- DNS
bool NdsWfcLocalServer::HandleDns(const uint8_t* frame, int len,
                                  std::vector<std::vector<uint8_t>>* out) {
    const uint8_t* ip = frame + 14;
    const int ihl = (ip[0] & 15) * 4;
    const uint8_t* udp = ip + ihl;
    const uint8_t* d = udp + 8;
    const int dn = std::min(len - 14 - ihl - 8, static_cast<int>(Get16(udp + 4)) - 8);
    if (dn < 17 || Get16(d + 4) != 1) return false;  // one question

    std::string name;
    int o = 12;
    while (o < dn && d[o] != 0) {
        const int label = d[o++];
        if (label > 63 || o + label > dn) return false;
        if (!name.empty()) name.push_back('.');
        name.append(reinterpret_cast<const char*>(d + o), static_cast<size_t>(label));
        o += label;
    }
    const int qend = o + 5;  // the zero byte, type and class
    if (qend > dn) return false;
    if (!EndsWithNoCase(name, "nintendowifi.net")) return false;  // the normal resolver's
    const bool type_a = Get16(d + o + 1) == 1;

    // Before the game connects: its https:// addresses become http://.
    PatchNoSsl();

    std::vector<uint8_t> l4(8 + static_cast<size_t>(qend) + (type_a ? 16 : 0));
    Put16(&l4[0], 53);
    Put16(&l4[2], Get16(udp));
    Put16(&l4[4], static_cast<uint32_t>(l4.size()));
    uint8_t* r = &l4[8];
    std::memcpy(r, d, static_cast<size_t>(qend));
    r[2] = 0x81;  // response, recursion desired
    r[3] = 0x80;  // recursion available, no error
    Put16(r + 6, type_a ? 1 : 0);
    Put16(r + 8, 0);
    Put16(r + 10, 0);
    if (type_a) {
        static const uint8_t kAnswer[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
        std::memcpy(r + qend, kAnswer, 12);
        std::memcpy(r + qend + 12, kServerIp, 4);
    }
    out->push_back(Ipv4Frame(frame + 6, frame, ip + 16, ip + 12, 17, std::move(l4), 6));
    Log("%s -> built-in server", name.c_str());
    return true;
}

// ---------------------------------------------------------------- NoSSL
void NdsWfcLocalServer::PatchNoSsl() {
    constexpr uint32_t kBase = 0x02000000u, kSize = 0x00400000u;
    std::vector<uint8_t> ram(kSize);
    for (uint32_t a = 0; a < kSize; a += 4) {
        const uint32_t v = bus_read_u32(kBase + a);
        std::memcpy(&ram[a], &v, 4);  // little-endian host, as the DS
    }
    static const char kHttps[] = "https://";
    auto it = ram.begin();
    while ((it = std::search(it, ram.end(), kHttps, kHttps + 8)) != ram.end()) {
        const size_t at = static_cast<size_t>(it - ram.begin());
        size_t n = 0;
        while (at + n < kSize && n < 128 && ram[at + n] != 0) ++n;
        it += 8;
        if (n >= 128) continue;
        const std::string url(reinterpret_cast<const char*>(&ram[at]), n);
        // Nintendo's servers, and the download server's address templates
        // ("https://%s/download"), which the address is built from after the
        // login says where the download server is.
        const bool download = url.size() >= 9 && url.compare(url.size() - 9, 9, "/download") == 0;
        if (url.find("nintendowifi.net") == std::string::npos && !download) continue;
        // "https://x" -> "http://x": drop the 's', move the rest down by one.
        for (size_t i = 4; i + 1 < n; ++i)
            bus_write_u8(kBase + static_cast<uint32_t>(at + i), ram[at + i + 1]);
        bus_write_u8(kBase + static_cast<uint32_t>(at + n - 1), 0);
        std::fprintf(stderr, "[wfc] %08X: %s -> http:// (no SSL)\n",
                     kBase + static_cast<uint32_t>(at), url.c_str());
    }
}

// ---------------------------------------------------------------- TCP
void NdsWfcLocalServer::Send(const Conn& c, uint32_t seq, uint8_t flags,
                             const uint8_t* data, int n,
                             std::vector<std::vector<uint8_t>>* out) const {
    const size_t opt = (flags & kSyn) ? 4 : 0;
    std::vector<uint8_t> t(20 + opt + static_cast<size_t>(n));
    Put16(&t[0], kHttpPort);
    Put16(&t[2], c.port);
    Put32(&t[4], seq);
    Put32(&t[8], (flags & kAck) ? c.rcv_nxt : 0);
    t[12] = static_cast<uint8_t>((20 + opt) / 4 << 4);
    t[13] = flags;
    Put16(&t[14], 16384);
    if (opt) {
        t[20] = 2;  // MSS
        t[21] = 4;
        Put16(&t[22], kOurMss);
    }
    if (n) std::memcpy(&t[20 + opt], data, static_cast<size_t>(n));
    out->push_back(Ipv4Frame(c.mac_ds, c.mac_gw, kServerIp, c.ip_ds, 6, std::move(t), 16));
}

// Send what the window allows, then the FIN.
void NdsWfcLocalServer::Push(Conn* c, std::vector<std::vector<uint8_t>>* out) {
    if (!c->have_reply) return;
    const uint32_t base = c->iss + 1;
    for (;;) {
        const uint32_t off = c->snd_nxt - base;
        if (off < c->reply.size()) {
            const uint32_t n = std::min<uint32_t>(
                {static_cast<uint32_t>(c->reply.size()) - off, c->mss, kOurMss});
            if ((c->snd_nxt - c->snd_una) + n > c->wnd) break;
            Send(*c, c->snd_nxt, kAck | kPsh, &c->reply[off], static_cast<int>(n), out);
            c->snd_nxt += n;
        } else {
            if (!c->fin_sent) {
                Send(*c, c->snd_nxt, kAck | kFin, nullptr, 0, out);
                c->snd_nxt++;
                c->fin_sent = true;
            }
            break;
        }
    }
}

bool NdsWfcLocalServer::HandleTcp(const uint8_t* frame, int len,
                                  std::vector<std::vector<uint8_t>>* out) {
    const uint8_t* ip = frame + 14;
    const int ihl = (ip[0] & 15) * 4;
    const int ip_len = std::min(static_cast<int>(Get16(ip + 2)), len - 14);
    const uint8_t* t = ip + ihl;
    const int tl = ip_len - ihl;
    if (tl < 20) return true;
    const int doff = (t[12] >> 4) * 4;
    if (doff < 20 || doff > tl) return true;
    const uint16_t sport = Get16(t), dport = Get16(t + 2);
    const uint32_t seq = Get32(t + 4), ack = Get32(t + 8);
    const uint8_t flags = t[13];
    const uint32_t wnd = Get16(t + 14);
    const uint8_t* data = t + doff;
    const int dl = tl - doff;

    Conn* c = nullptr;
    for (Conn& k : conns_) {
        if (k.used && k.port == sport && std::memcmp(k.ip_ds, ip + 12, 4) == 0) c = &k;
    }
    auto fill = [&](Conn* k) {
        std::memcpy(k->mac_ds, frame + 6, 6);
        std::memcpy(k->mac_gw, frame, 6);
        std::memcpy(k->ip_ds, ip + 12, 4);
        k->port = sport;
    };
    auto reset = [&](uint32_t rst_seq, uint32_t rst_ack) {
        Conn tmp;
        fill(&tmp);
        tmp.rcv_nxt = rst_ack;
        Send(tmp, rst_seq, kRst | kAck, nullptr, 0, out);
    };

    if (flags & kRst) {
        if (c) *c = Conn{};
        return true;
    }
    if ((flags & (kSyn | kAck)) == kSyn) {
        if (dport != kHttpPort) {
            reset(0, seq + 1);
            std::fprintf(stderr, "[wfc] refused a connection to port %u\n", dport);
            return true;
        }
        if (c && c->rcv_nxt == seq + 1) {  // the same SYN again
            Send(*c, c->iss, kSyn | kAck, nullptr, 0, out);
            return true;
        }
        if (!c) {
            for (Conn& k : conns_) if (!k.used) { c = &k; break; }
        }
        if (!c) {  // all busy: the one unused longest
            c = &conns_[0];
            for (Conn& k : conns_) if (k.last_use < c->last_use) c = &k;
        }
        *c = Conn{};
        c->used = true;
        fill(c);
        rand_ = rand_ * 1103515245u + 12345u;
        c->iss = rand_ & 0x7FFFFF00u;
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->rcv_nxt = seq + 1;
        c->wnd = wnd;
        for (int o = 20; o + 1 < doff;) {  // the DS's MSS
            const uint8_t kind = t[o];
            if (kind == 0) break;
            if (kind == 1) { ++o; continue; }
            const uint8_t ol = t[o + 1];
            if (ol < 2 || o + ol > doff) break;
            if (kind == 2 && ol == 4) c->mss = std::max<uint32_t>(64, Get16(t + o + 2));
            o += ol;
        }
        c->last_use = ++use_counter_;
        Send(*c, c->iss, kSyn | kAck, nullptr, 0, out);
        return true;
    }
    if (!c) {
        reset((flags & kAck) ? ack : 0, seq + static_cast<uint32_t>(dl) + ((flags & kFin) ? 1 : 0));
        return true;
    }
    c->last_use = ++use_counter_;
    c->wnd = wnd;
    if (flags & kAck) {
        if (static_cast<int32_t>(ack - c->snd_una) > 0 &&
            static_cast<int32_t>(ack - c->snd_nxt) <= 0) {
            c->snd_una = ack;
        } else if (ack == c->snd_una && dl == 0 && !(flags & kFin) &&
                   c->snd_una != c->snd_nxt) {
            // The DS is still missing what starts at snd_una: send again.
            c->snd_nxt = c->snd_una;
            if (c->fin_sent && c->snd_una - (c->iss + 1) <= c->reply.size())
                c->fin_sent = false;
        }
    }
    if (dl > 0) {
        if (seq == c->rcv_nxt && c->request.size() + static_cast<size_t>(dl) < 16384) {
            c->request.append(reinterpret_cast<const char*>(data), static_cast<size_t>(dl));
            c->rcv_nxt += static_cast<uint32_t>(dl);
        }
        Send(*c, c->snd_nxt, kAck, nullptr, 0, out);
        const size_t head_end = c->request.find("\r\n\r\n");
        if (head_end != std::string::npos && !c->have_reply) {
            size_t need = 0;
            const std::string head = Lower(c->request.substr(0, head_end));
            const size_t cl = head.find("content-length:");
            if (cl != std::string::npos)
                need = static_cast<size_t>(std::strtoul(head.c_str() + cl + 15, nullptr, 10));
            if (c->request.size() - (head_end + 4) >= need) {
                c->reply = Http(c->request, c->mac_ds);
                c->have_reply = true;
            }
        }
    }
    if ((flags & kFin) && seq + static_cast<uint32_t>(dl) == c->rcv_nxt && !c->fin_in) {
        c->rcv_nxt++;
        c->fin_in = true;
        Send(*c, c->snd_nxt, kAck, nullptr, 0, out);
    }
    Push(c, out);
    if (c->fin_sent && c->fin_in && c->snd_una == c->snd_nxt) *c = Conn{};
    return true;
}

// ---------------------------------------------------------------- HTTP
std::vector<uint8_t> NdsWfcLocalServer::Http(const std::string& request,
                                             const uint8_t mac[6]) {
    char method[8] = "", path[128] = "";
    std::sscanf(request.c_str(), "%7s %127s", method, path);
    const size_t head_end = request.find("\r\n\r\n");
    const std::string body = head_end == std::string::npos ? std::string() : request.substr(head_end + 4);

    int status = 200;
    std::string ctype = "text/plain", extra, text;
    std::vector<uint8_t> bin;
    bool binary = false;
    const std::tm now = UtcNow();

    if (std::strcmp(method, "GET") == 0) {  // conntest
        text = "ok";
        ctype = "text/html";
        extra = "X-Organization: Nintendo\r\n";
        Log("connection test answered");
    } else if (std::strcmp(path, "/ac") == 0) {  // nas
        const std::string action = Lower(FormField(body, "action"));
        const std::string svc = FormField(body, "svc");
        const std::string userid = FormField(body, "userid");
        char datetime[16];
        std::strftime(datetime, sizeof datetime, "%Y%m%d%H%M%S", &now);
        std::string token = "NDS";
        static const char kChars[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
        while (token.size() < 99) {
            rand_ = rand_ * 1103515245u + 12345u;
            token.push_back(kChars[(rand_ >> 8) % 62]);
        }
        if (action == "acctcreate") {
            AddField(&text, "retry", "0");
            AddField(&text, "returncd", "002");
            AddField(&text, "userid", userid.empty() ? "1000000000001" : userid);
        } else if (action == "login") {
            AddField(&text, "retry", "0");
            AddField(&text, "returncd", "001");
            AddField(&text, "locator", "gamespy.com");
            AddField(&text, "challenge", "NDSRECMP");
            AddField(&text, "token", token);
        } else {  // svcloc and anything else
            AddField(&text, "retry", "0");
            AddField(&text, "returncd", "007");
            AddField(&text, "statusdata", "Y");
            AddField(&text, "svchost", "dls1.nintendowifi.net");
            AddField(&text, svc == "9000" ? "token" : "servicetoken", token);
        }
        AddField(&text, "datetime", datetime);
        text += "\r\n";
        extra = "NODE: wifiappe1\r\n";
        Log("login (%s, game %s) accepted", action.c_str(), FormField(body, "gamecd").c_str());
    } else if (std::strcmp(path, "/download") == 0) {  // dls1
        const std::string action = Lower(FormField(body, "action"));
        NdsDlsRequest req;
        req.gamecd = FormField(body, "gamecd");
        std::memcpy(req.mac, mac, 6);
        const std::string attr[3] = {FormField(body, "attr1"), FormField(body, "attr2"),
                                     FormField(body, "attr3")};
        const std::string name = FormField(body, "contents");
        std::vector<NdsDlsFile> files = nds_title_dls_files(req);
        std::vector<const NdsDlsFile*> hits;
        for (const NdsDlsFile& f : files) {
            const std::string* fa[3] = {&f.attr1, &f.attr2, &f.attr3};
            bool match = true;
            for (int i = 0; i < 3; ++i) match &= attr[i].empty() || attr[i] == *fa[i];
            if (match) hits.push_back(&f);
        }
        extra = "X-DLS-Host: http://127.0.0.1/\r\n";
        if (action == "count") {
            text = std::to_string(hits.size());
        } else if (action == "list") {
            const std::string off = FormField(body, "offset"), num = FormField(body, "num");
            size_t first = off.empty() ? 0 : std::strtoul(off.c_str(), nullptr, 10);
            size_t count = num.empty() ? hits.size() : std::strtoul(num.c_str(), nullptr, 10);
            for (size_t i = first; i < hits.size() && i - first < count; ++i) {
                const NdsDlsFile& f = *hits[i];
                text += f.name + "\t\t" + f.attr1 + "\t" + f.attr2 + "\t" + f.attr3 + "\t" +
                        std::to_string(f.data.size()) + "\r\n";
            }
            if (text.empty()) text = "\r\n";
        } else if (action == "contents") {
            status = 404;
            for (const NdsDlsFile& f : files) {
                if (f.name != name) continue;
                bin = f.data;
                binary = true;
                status = 200;
                ctype = "application/x-dsdl";
                extra = "Content-Disposition: attachment; filename=\"" + name + "\"\r\n";
                break;
            }
        }
        if (action == "contents")
            Log("download %s %s: %s", req.gamecd.c_str(), name.c_str(), status == 200 ? "sent" : "not found");
        else
            std::fprintf(stderr, "[wfc] download %s %s (attr %s/%s/%s): %zu file(s)\n",
                         req.gamecd.c_str(), action.c_str(), attr[0].c_str(), attr[1].c_str(),
                         attr[2].c_str(), hits.size());
    } else {
        status = 404;
        Log("%s %s: not handled", method, path);
    }

    if (!binary) bin.assign(text.begin(), text.end());
    if (status != 200) bin.clear();
    char date[64];
    std::strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &now);
    char head[1024];
    const int hn = std::snprintf(
        head, sizeof head,
        "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: Nintendo Wii (http)\r\n"
        "Content-Type: %s\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, status == 200 ? "OK" : "Not Found", date, ctype.c_str(), extra.c_str(),
        bin.size());
    std::vector<uint8_t> reply(head, head + std::max(0, std::min(hn, static_cast<int>(sizeof head) - 1)));
    reply.insert(reply.end(), bin.begin(), bin.end());
    return reply;
}
