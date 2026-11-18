// chat_overlay.cpp -- see chat_overlay.h.

#include "chat_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

// Printable ASCII, 0x20..0x7E: 8 rows of 5 pixels each. Rows 0-6 sit on
// the baseline, row 7 is the descender.
const char* const kFontArt[95][8] = {
    {".....", ".....", ".....", ".....", ".....", ".....", ".....", "....."},  // ' '
    {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#..", "....."},  // !
    {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", ".....", "....."},  // "
    {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#.", "....."},  // #
    {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", "....."},  // $
    {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##", "....."},  // %
    {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#", "....."},  // &
    {"..#..", "..#..", ".#...", ".....", ".....", ".....", ".....", "....."},  // '
    {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.", "....."},  // (
    {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...", "....."},  // )
    {".....", "..#..", "#.#.#", ".###.", "#.#.#", "..#..", ".....", "....."},  // *
    {".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....", "....."},  // +
    {".....", ".....", ".....", ".....", ".....", ".##..", "..#..", ".#..."},  // ,
    {".....", ".....", ".....", "#####", ".....", ".....", ".....", "....."},  // -
    {".....", ".....", ".....", ".....", ".....", ".##..", ".##..", "....."},  // .
    {".....", "....#", "...#.", "..#..", ".#...", "#....", ".....", "....."},  // /
    {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", "....."},  // 0
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."},  // 1
    {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", "....."},  // 2
    {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###.", "....."},  // 3
    {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", "....."},  // 4
    {"#####", "#....", "####.", "....#", "....#", "#...#", ".###.", "....."},  // 5
    {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", "....."},  // 6
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", "....."},  // 7
    {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", "....."},  // 8
    {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", "....."},  // 9
    {".....", ".##..", ".##..", ".....", ".##..", ".##..", ".....", "....."},  // :
    {".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#...", "....."},  // ;
    {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#.", "....."},  // <
    {".....", ".....", "#####", ".....", "#####", ".....", ".....", "....."},  // =
    {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#...", "....."},  // >
    {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..", "....."},  // ?
    {".###.", "#...#", "....#", ".##.#", "#.#.#", "#.#.#", ".###.", "....."},  // @
    {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "....."},  // A
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", "....."},  // B
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", "....."},  // C
    {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###..", "....."},  // D
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####", "....."},  // E
    {"#####", "#....", "#....", "####.", "#....", "#....", "#....", "....."},  // F
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####", "....."},  // G
    {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "....."},  // H
    {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."},  // I
    {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..", "....."},  // J
    {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", "....."},  // K
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####", "....."},  // L
    {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", "....."},  // M
    {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "....."},  // N
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "....."},  // O
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#....", "....."},  // P
    {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", "....."},  // Q
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", "....."},  // R
    {".####", "#....", "#....", ".###.", "....#", "....#", "####.", "....."},  // S
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "....."},  // T
    {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "....."},  // U
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."},  // V
    {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#.", "....."},  // W
    {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#", "....."},  // X
    {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", "....."},  // Y
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", "....."},  // Z
    {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.", "....."},  // [
    {".....", "#....", ".#...", "..#..", "...#.", "....#", ".....", "....."},  // backslash
    {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###.", "....."},  // ]
    {"..#..", ".#.#.", "#...#", ".....", ".....", ".....", ".....", "....."},  // ^
    {".....", ".....", ".....", ".....", ".....", ".....", ".....", "#####"},  // _
    {".#...", "..#..", ".....", ".....", ".....", ".....", ".....", "....."},  // `
    {".....", ".....", ".###.", "....#", ".####", "#...#", ".####", "....."},  // a
    {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "####.", "....."},  // b
    {".....", ".....", ".###.", "#....", "#....", "#...#", ".###.", "....."},  // c
    {"....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####", "....."},  // d
    {".....", ".....", ".###.", "#...#", "#####", "#....", ".###.", "....."},  // e
    {"..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#...", "....."},  // f
    {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", ".###."},  // g
    {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#", "....."},  // h
    {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###.", "....."},  // i
    {"...#.", ".....", "..##.", "...#.", "...#.", "...#.", "#..#.", ".##.."},  // j
    {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "....."},  // k
    {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", "....."},  // l
    {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#", "....."},  // m
    {".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#", "....."},  // n
    {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###.", "....."},  // o
    {".....", ".....", "####.", "#...#", "#...#", "####.", "#....", "#...."},  // p
    {".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#"},  // q
    {".....", ".....", "#.##.", "##..#", "#....", "#....", "#....", "....."},  // r
    {".....", ".....", ".###.", "#....", ".###.", "....#", "####.", "....."},  // s
    {".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##.", "....."},  // t
    {".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#", "....."},  // u
    {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#..", "....."},  // v
    {".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", "....."},  // w
    {".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "....."},  // x
    {".....", ".....", "#...#", "#...#", "#...#", ".####", "....#", ".###."},  // y
    {".....", ".....", "#####", "...#.", "..#..", ".#...", "#####", "....."},  // z
    {"...#.", "..#..", "..#..", ".#...", "..#..", "..#..", "...#.", "....."},  // {
    {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "....."},  // |
    {".#...", "..#..", "..#..", "...#.", "..#..", "..#..", ".#...", "....."},  // }
    {".....", ".....", ".#...", "#.#.#", "...#.", ".....", ".....", "....."},  // ~
};

const char* const kSharpS[8] = {".##..", "#..#.", "#..#.", "#.#..",
                                "#..#.", "#...#", "#.##.", "#...."};
const char* const kMissing[8] = {"#####", "#...#", "#...#", "#...#",
                                 "#...#", "#...#", "#####", "....."};

// Symbols, ligatures and arrows a game's own keyboard may type.
struct Extra {
    uint32_t cp;
    const char* art[8];
};
const Extra kExtra[] = {
    {0xA2, {"..#..", ".###.", "#.#..", "#.#..", "#.#.#", ".###.", "..#..", "....."}},  // ¢
    {0xA3, {"..##.", ".#..#", ".#...", "###..", ".#...", ".#..#", "#.##.", "....."}},  // £
    {0xAA, {".###.", "...#.", ".###.", "#..#.", ".###.", ".....", "####.", "....."}},  // ª
    {0xB0, {".##..", "#..#.", "#..#.", ".##..", ".....", ".....", ".....", "....."}},  // °
    {0xC6, {".####", "#.#..", "#.#..", "#####", "#.#..", "#.#..", "#.###", "....."}},  // Æ
    {0xE6, {".....", ".....", "##.#.", "..#.#", ".####", "#.#..", ".#.##", "....."}},  // æ
    {0x152, {".####", "#.#..", "#.#..", "#.###", "#.#..", "#.#..", ".####", "....."}},  // Œ
    {0x153, {".....", ".....", ".#.#.", "#.#.#", "#.###", "#.#..", ".#.##", "....."}},  // œ
    {0x201E, {".....", ".....", ".....", ".....", ".....", "##.##", ".#..#", "#..#."}},  // „
    {0x20AC, {"..###", ".#...", "####.", ".#...", "####.", ".#...", "..###", "....."}},  // €
    {0x2190, {".....", "..#..", ".#...", "#####", ".#...", "..#..", ".....", "....."}},  // ←
    {0x2191, {"..#..", ".###.", "#.#.#", "..#..", "..#..", "..#..", "..#..", "....."}},  // ↑
    {0x2192, {".....", "..#..", "...#.", "#####", "...#.", "..#..", ".....", "....."}},  // →
    {0x2193, {"..#..", "..#..", "..#..", "..#..", "#.#.#", ".###.", "..#..", "....."}},  // ↓
    {0x2605, {"..#..", "..#..", "#####", ".###.", ".###.", "##.##", "#...#", "....."}},  // ★
    {0x263A, {".....", ".#.#.", ".#.#.", ".....", "#...#", ".###.", ".....", "....."}},  // ☺
};

enum Accent { kGrave, kAcute, kCircumflex, kTilde, kDiaeresis, kRing };
const char* const kAccentArt[6][2] = {
    {".#...", "..#.."},  // grave
    {"...#.", "..#.."},  // acute
    {"..#..", ".#.#."},  // circumflex
    {".##.#", "#..#."},  // tilde
    {".#.#.", "....."},  // diaeresis
    {"..#..", ".#.#."},  // ring (drawn like the circumflex)
};

uint8_t ParseRow(const char* art) {
    uint8_t bits = 0;
    for (int x = 0; x < 5; ++x)
        if (art[x] == '#') bits |= static_cast<uint8_t>(0x10u >> x);
    return bits;
}

void Art(const char* const art[8], uint8_t rows[8]) {
    for (int y = 0; y < 8; ++y) rows[y] = ParseRow(art[y]);
}

void Ascii(char c, uint8_t rows[8]) {
    Art(kFontArt[static_cast<unsigned char>(c) - 0x20], rows);
}

// A lowercase letter with a mark above: the letter's x-height part (rows
// 2-7; an i loses its dot) under the mark.
// An uppercase letter fills rows 0-6, so it is squeezed into rows 2-6
// (rows 1 and 5 of it dropped) under the mark.
void Accented(char base, Accent accent, uint8_t rows[8]) {
    Ascii(base, rows);
    if (base >= 'A' && base <= 'Z') {
        const uint8_t full[7] = {rows[0], rows[1], rows[2], rows[3],
                                 rows[4], rows[5], rows[6]};
        rows[2] = full[0];
        rows[3] = full[2];
        rows[4] = full[3];
        rows[5] = full[4];
        rows[6] = full[6];
    }
    rows[0] = ParseRow(kAccentArt[accent][0]);
    rows[1] = ParseRow(kAccentArt[accent][1]);
}

// Latin-1 letters 0xC0..0xFF: base letter, and its mark.
struct Latin {
    char base;
    int accent;  // -1: none
};
const Latin kLatin[64] = {
    // 0xC0
    {'A', kGrave}, {'A', kAcute}, {'A', kCircumflex}, {'A', kTilde},
    {'A', kDiaeresis}, {'A', kRing}, {'A', -1}, {'C', -1},
    {'E', kGrave}, {'E', kAcute}, {'E', kCircumflex}, {'E', kDiaeresis},
    {'I', kGrave}, {'I', kAcute}, {'I', kCircumflex}, {'I', kDiaeresis},
    // 0xD0
    {'D', -1}, {'N', kTilde}, {'O', kGrave}, {'O', kAcute},
    {'O', kCircumflex}, {'O', kTilde}, {'O', kDiaeresis}, {'x', -1},
    {'O', -1}, {'U', kGrave}, {'U', kAcute}, {'U', kCircumflex},
    {'U', kDiaeresis}, {'Y', kAcute}, {'P', -1}, {'s', -1},
    // 0xE0
    {'a', kGrave}, {'a', kAcute}, {'a', kCircumflex}, {'a', kTilde},
    {'a', kDiaeresis}, {'a', kRing}, {'a', -1}, {'c', -1},
    {'e', kGrave}, {'e', kAcute}, {'e', kCircumflex}, {'e', kDiaeresis},
    {'i', kGrave}, {'i', kAcute}, {'i', kCircumflex}, {'i', kDiaeresis},
    // 0xF0
    {'d', -1}, {'n', kTilde}, {'o', kGrave}, {'o', kAcute},
    {'o', kCircumflex}, {'o', kTilde}, {'o', kDiaeresis}, {'/', -1},
    {'o', -1}, {'u', kGrave}, {'u', kAcute}, {'u', kCircumflex},
    {'u', kDiaeresis}, {'y', kAcute}, {'p', -1}, {'y', kDiaeresis},
};

// ---- layout ----------------------------------------------------------------

constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kOtherName = 0xFFFFD75Au;
constexpr uint32_t kSelfName = 0xFF8CE68Cu;
constexpr uint32_t kSystem = 0xFFB4B4B4u;
constexpr uint32_t kCursor = 0xFFFFFFFFu;
constexpr uint32_t kLineBack = 0xA0000000u;
constexpr uint32_t kPanelBack = 0xB4000000u;
constexpr uint32_t kInputBack = 0xDC101830u;
constexpr int kMargin = 2;
constexpr int kPad = 2;  // inside a background, left of the first glyph
constexpr int kIdleRows = 6;
constexpr int kOpenHistoryRows = 10;

struct Cell {
    uint32_t cp = ' ';
    uint32_t color = kWhite;
};
using Row = std::vector<Cell>;

void AppendUtf8(const std::string& text, uint32_t color, Row* out) {
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
        else { ++i; continue; }
        if (i + len > text.size()) break;
        for (size_t k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        out->push_back(Cell{cp, color});
        i += len;
    }
}

// Breaks `cells` into rows of at most `cols`, at the last space of a row
// when there is one in its second half, else wherever the row is full.
// A space a break lands on is dropped.
void Wrap(const Row& cells, int cols, std::vector<Row>* rows) {
    size_t start = 0;
    while (start < cells.size()) {
        size_t end = std::min(cells.size(), start + size_t(cols));
        size_t next = end;
        if (end < cells.size()) {
            for (size_t k = end; k > start + size_t(cols) / 2; --k) {
                if (cells[k].cp == ' ') {
                    end = k;
                    next = k + 1;
                    break;
                }
            }
        }
        rows->emplace_back(cells.begin() + start, cells.begin() + end);
        start = next;
    }
    if (cells.empty()) rows->emplace_back();
}

uint32_t Fade(uint32_t argb, double alpha) {
    const uint32_t a = static_cast<uint32_t>((argb >> 24) * alpha + 0.5);
    return (std::min<uint32_t>(a, 255u) << 24) | (argb & 0x00FFFFFFu);
}

void DrawRow(const Row& row, int x, int y, double alpha,
             std::vector<NdsChatOverlayRect>* out) {
    for (size_t i = 0; i < row.size(); ++i) {
        uint8_t bits[8];
        nds_chat_glyph(row[i].cp, bits);
        const int gx = x + int(i) * kNdsChatCellWidth;
        const uint32_t color = Fade(row[i].color, alpha);
        for (int gy = 0; gy < 8; ++gy) {
            // Runs of set pixels, one rect each.
            for (int px = 0; px < 5;) {
                if (!(bits[gy] & (0x10u >> px))) { ++px; continue; }
                int run = 1;
                while (px + run < 5 && (bits[gy] & (0x10u >> (px + run))))
                    ++run;
                out->push_back({gx + px, y + 1 + gy, run, 1, color});
                px += run;
            }
        }
    }
}

void LineCells(const NdsChatLine& line, Row* cells) {
    if (line.source == NdsChatSource::System) {
        AppendUtf8(line.text, kSystem, cells);
        return;
    }
    AppendUtf8(line.from.empty() ? "?" : line.from,
               line.self ? kSelfName : kOtherName, cells);
    AppendUtf8(": ", kWhite, cells);
    AppendUtf8(line.text, kWhite, cells);
}

std::mutex g_input_mutex;
NdsChatOverlayInput g_input;
std::atomic<NdsChatScaleSource> g_scale_source{nullptr};

}  // namespace

void nds_chat_overlay_set_scale_source(NdsChatScaleSource source) {
    g_scale_source.store(source, std::memory_order_release);
}

double nds_chat_overlay_scale() {
    const NdsChatScaleSource source =
        g_scale_source.load(std::memory_order_acquire);
    const double scale = source ? source() : 1.0;
    if (!(scale > 0.25)) return 0.25;  // also NaN
    return scale < 1.0 ? scale : 1.0;
}

void nds_chat_glyph(uint32_t cp, uint8_t rows[8]) {
    if (cp >= 0x20 && cp <= 0x7E) {
        Ascii(static_cast<char>(cp), rows);
        return;
    }
    for (const Extra& e : kExtra) {
        if (e.cp == cp) {
            Art(e.art, rows);
            return;
        }
    }
    if (cp == 0xDF) { Art(kSharpS, rows); return; }
    if (cp >= 0xC0 && cp <= 0xFF) {
        const Latin& l = kLatin[cp - 0xC0];
        if (l.accent >= 0) Accented(l.base, Accent(l.accent), rows);
        else Ascii(l.base, rows);
        if (cp == 0xC7 || cp == 0xE7) rows[7] = ParseRow("..#..");  // cedilla
        return;
    }
    if (cp == 0xA1 || cp == 0xBF) {  // inverted ! and ?: turned half round
        uint8_t up[8];
        Ascii(cp == 0xA1 ? '!' : '?', up);
        for (int y = 0; y < 7; ++y) {
            uint8_t mirrored = 0;
            for (int x = 0; x < 5; ++x)
                if (up[6 - y] & (0x10u >> x)) mirrored |= uint8_t(1u << x);
            rows[y] = mirrored;
        }
        rows[7] = 0;
        return;
    }
    switch (cp) {
        case 0xA0: Ascii(' ', rows); return;
        case 0xAB: Ascii('<', rows); return;
        case 0xBB: Ascii('>', rows); return;
        case 0xD7: Ascii('x', rows); return;
        case 0x2013: case 0x2014: Ascii('-', rows); return;
        case 0x2018: case 0x2019: Ascii('\'', rows); return;
        case 0x201C: case 0x201D: Ascii('"', rows); return;
        case 0x2026: Ascii('.', rows); return;
        default: Art(kMissing, rows); return;
    }
}

void nds_chat_overlay_set_input(const NdsChatOverlayInput& input) {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    g_input = input;
}

NdsChatOverlayInput nds_chat_overlay_input() {
    std::lock_guard<std::mutex> lock(g_input_mutex);
    return g_input;
}

namespace {

bool Continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}

// Bytes of the character starting with `lead`; 0 when no character starts
// with it.
size_t CharBytes(unsigned char lead) {
    if (lead < 0x80u) return 1;
    if (lead < 0xC2u) return 0;
    if (lead < 0xE0u) return 2;
    if (lead < 0xF0u) return 3;
    if (lead < 0xF5u) return 4;
    return 0;
}

size_t PrevBoundary(const std::string& s, size_t at) {
    if (at == 0) return 0;
    --at;
    while (at > 0 && Continuation(s[at])) --at;
    return at;
}

size_t NextBoundary(const std::string& s, size_t at) {
    if (at >= s.size()) return s.size();
    ++at;
    while (at < s.size() && Continuation(s[at])) ++at;
    return at;
}

}  // namespace

void nds_chat_edit_insert(NdsChatOverlayInput* input, const char* utf8) {
    if (!input || !utf8) return;
    std::string add;
    for (size_t i = 0; utf8[i] != '\0';) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const size_t n = CharBytes(lead);
        bool whole = n != 0;
        for (size_t k = 1; k < n && whole; ++k)
            whole = utf8[i + k] != '\0' && Continuation(utf8[i + k]);
        if (!whole) {  // malformed: skip the byte
            ++i;
            continue;
        }
        // C0 controls, DEL and C1 controls (U+0080..U+009F).
        const bool control = lead < 0x20u || lead == 0x7Fu ||
            (lead == 0xC2u &&
             static_cast<unsigned char>(utf8[i + 1]) < 0xA0u);
        if (!control) {
            if (input->text.size() + add.size() + n > kNdsChatTextMax) break;
            add.append(utf8 + i, n);
        }
        i += n;
    }
    input->cursor = std::min(input->cursor, input->text.size());
    input->text.insert(input->cursor, add);
    input->cursor += add.size();
}

void nds_chat_edit_key(NdsChatOverlayInput* input, NdsChatEditKey key) {
    if (!input) return;
    std::string& s = input->text;
    size_t& at = input->cursor;
    at = std::min(at, s.size());
    switch (key) {
        case NdsChatEditKey::Backspace: {
            const size_t from = PrevBoundary(s, at);
            s.erase(from, at - from);
            at = from;
            break;
        }
        case NdsChatEditKey::Delete:
            s.erase(at, NextBoundary(s, at) - at);
            break;
        case NdsChatEditKey::Left: at = PrevBoundary(s, at); break;
        case NdsChatEditKey::Right: at = NextBoundary(s, at); break;
        case NdsChatEditKey::Home: at = 0; break;
        case NdsChatEditKey::End: at = s.size(); break;
    }
}

void nds_chat_overlay_layout(int width, int height,
                             const std::vector<NdsChatLine>& lines,
                             const NdsChatOverlayInput& input, int64_t now_ms,
                             std::vector<NdsChatOverlayRect>* out,
                             int keep_clear) {
    // Open, the panel goes over whatever is at the right.
    const int reserve = input.open || keep_clear <= 0 ? 0 : keep_clear;
    const int cols =
        (width - 2 * kMargin - kPad - 1 - reserve) / kNdsChatCellWidth;
    const int max_rows = (height - 2 * kMargin) / kNdsChatCellHeight;
    if (cols < 8 || max_rows < 2) return;

    // History rows, newest last, each with its fade.
    struct Shown {
        Row row;
        double alpha;
    };
    std::vector<Shown> history;
    const int history_cap = input.open ? kOpenHistoryRows : kIdleRows;
    for (auto it = lines.rbegin();
         it != lines.rend() && int(history.size()) < history_cap; ++it) {
        double alpha = 1.0;
        if (!input.open) {
            const int64_t age = now_ms - it->at_ms;
            if (age >= kNdsChatShowMs) break;  // older ones are older still
            const int64_t left = kNdsChatShowMs - age;
            if (left < kNdsChatFadeMs) alpha = double(left) / kNdsChatFadeMs;
        }
        Row cells;
        LineCells(*it, &cells);
        std::vector<Row> rows;
        Wrap(cells, cols, &rows);
        for (auto r = rows.rbegin();
             r != rows.rend() && int(history.size()) < history_cap; ++r)
            history.push_back(Shown{*r, alpha});
    }
    std::reverse(history.begin(), history.end());

    std::vector<Row> input_rows;
    size_t cursor_cell = 0;
    if (input.open) {
        const size_t split = std::min(input.cursor, input.text.size());
        Row cells;
        AppendUtf8("> ", kSelfName, &cells);
        AppendUtf8(input.text.substr(0, split), kWhite, &cells);
        cursor_cell = cells.size();
        AppendUtf8(input.text.substr(split), kWhite, &cells);
        // Hard breaks, so the cursor's cell is simple to place; one more
        // cell for a cursor at the very end.
        for (size_t s = 0; s <= cells.size(); s += size_t(cols)) {
            const size_t e = std::min(cells.size(), s + size_t(cols));
            input_rows.emplace_back(cells.begin() + s, cells.begin() + e);
            if (e == cells.size() && (e - s) < size_t(cols)) break;
        }
    }

    // Keep everything on screen: input rows first (the last ones, if a
    // tiny screen cannot hold them all), then the newest history.
    const int input_count = std::min<int>(int(input_rows.size()), max_rows);
    const size_t input_skipped = input_rows.size() - size_t(input_count);
    const int history_room = max_rows - input_count;
    if (int(history.size()) > history_room)
        history.erase(history.begin(), history.end() - history_room);
    const int total = int(history.size()) + input_count;
    if (total == 0) return;

    const int x = kMargin;
    int y = height - kMargin - total * kNdsChatCellHeight;
    const int panel_w = kPad + cols * kNdsChatCellWidth + 1;
    if (input.open) {
        if (!history.empty())
            out->push_back({x, y, panel_w,
                            int(history.size()) * kNdsChatCellHeight,
                            kPanelBack});
        out->push_back({x, y + int(history.size()) * kNdsChatCellHeight,
                        panel_w, input_count * kNdsChatCellHeight,
                        kInputBack});
    }
    for (const Shown& s : history) {
        if (!input.open && !s.row.empty())
            out->push_back({x, y,
                            kPad + int(s.row.size()) * kNdsChatCellWidth + 1,
                            kNdsChatCellHeight, Fade(kLineBack, s.alpha)});
        DrawRow(s.row, x + kPad, y, s.alpha, out);
        y += kNdsChatCellHeight;
    }
    for (int r = 0; r < input_count; ++r) {
        const size_t row_index = size_t(r) + input_skipped;
        DrawRow(input_rows[row_index], x + kPad, y, 1.0, out);
        // Blinking bar left of the cursor's cell.
        if ((now_ms / 500) % 2 == 0 &&
            cursor_cell / size_t(cols) == row_index) {
            const int cx = x + kPad +
                int(cursor_cell % size_t(cols)) * kNdsChatCellWidth - 1;
            out->push_back({std::max(cx, x), y + 1, 1, 8, kCursor});
        }
        y += kNdsChatCellHeight;
    }
}

bool nds_chat_overlay_frame(int width, int height,
                            std::vector<NdsChatOverlayRect>* out,
                            int keep_clear) {
    out->clear();
    const NdsChatOverlayInput input = nds_chat_overlay_input();
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    // Most frames: no chat open and nothing recent. Checked on the newest
    // line alone, before copying the list.
    static uint64_t cached_id = ~0ull;
    static std::vector<NdsChatLine> cached;
    const uint64_t last = nds_netplay_chat_last_id();
    if (last != cached_id) {
        cached = nds_netplay_chat_since(last > kNdsChatKeep
                                            ? last - kNdsChatKeep : 0);
        cached_id = last;
    }
    if (!input.open &&
        (cached.empty() || now_ms - cached.back().at_ms >= kNdsChatShowMs))
        return false;
    nds_chat_overlay_layout(width, height, cached, input, now_ms, out,
                            keep_clear);
    return !out->empty();
}
