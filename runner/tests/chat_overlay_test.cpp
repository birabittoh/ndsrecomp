// chat_overlay_test.cpp -- glyphs and layout of the netplay chat overlay
// (chat_overlay.h). `chat_overlay_test --print` also prints a sample
// layout as text, to look at.

#include "chat_overlay.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool require(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", what);
    return condition;
}

NdsChatLine Line(const char* from, const char* text, int64_t at_ms,
                 bool self = false) {
    NdsChatLine line;
    line.source = NdsChatSource::Online;
    line.from = from;
    line.text = text;
    line.at_ms = at_ms;
    line.self = self;
    return line;
}

bool InScreen(const std::vector<NdsChatOverlayRect>& rects, int w, int h) {
    for (const auto& r : rects)
        if (r.x < 0 || r.y < 0 || r.x + r.w > w || r.y + r.h > h) return false;
    return true;
}

bool TestGlyphs() {
    bool ok = true;
    uint8_t rows[8];
    for (uint32_t cp = 0x21; cp <= 0x7E; ++cp) {
        nds_chat_glyph(cp, rows);
        uint8_t any = 0;
        for (uint8_t r : rows) any |= r;
        if (!any || (any & ~0x1Fu)) {
            std::fprintf(stderr, "glyph %02x\n", cp);
            ok = require(false, "printable glyph has pixels, 5 wide") && ok;
        }
    }
    nds_chat_glyph('A', rows);
    ok = require(rows[0] == 0x0E && rows[3] == 0x1F && rows[7] == 0,
                 "A") && ok;
    nds_chat_glyph(0xE9, rows);  // e acute: mark over the e's body
    uint8_t e[8];
    nds_chat_glyph('e', e);
    ok = require(rows[0] == 0x02 && rows[1] == 0x04 &&
                     std::memcmp(rows + 2, e + 2, 6) == 0, "e acute") && ok;
    nds_chat_glyph(0x3042, rows);  // no kana: the box
    ok = require(rows[0] == 0x1F && rows[6] == 0x1F, "missing glyph") && ok;
    return ok;
}

bool TestLayout() {
    bool ok = true;
    std::vector<NdsChatOverlayRect> out;
    NdsChatOverlayInput closed;
    const std::vector<NdsChatLine> lines = {Line("Ally", "hello there", 1000)};

    nds_chat_overlay_layout(256, 192, lines, closed, 1000, &out);
    ok = require(!out.empty() && InScreen(out, 256, 192), "fresh line") && ok;
    ok = require(out[0].argb == 0xA0000000u, "opaque background") && ok;

    out.clear();
    nds_chat_overlay_layout(256, 192, lines, closed, 1000 + 9500, &out);
    ok = require(!out.empty() && (out[0].argb >> 24) < 0xA0, "fading") && ok;

    out.clear();
    nds_chat_overlay_layout(256, 192, lines, closed, 1000 + 10000, &out);
    ok = require(out.empty(), "gone after the show time") && ok;

    // A long line wraps and stays on screen, on a narrow screen too.
    const std::string long_text(240, 'w');
    out.clear();
    nds_chat_overlay_layout(256, 192, {Line("Ally", long_text.c_str(), 0)},
                            closed, 0, &out);
    ok = require(InScreen(out, 256, 192), "long line on screen") && ok;
    out.clear();
    nds_chat_overlay_layout(96, 192, {Line("Ally", long_text.c_str(), 0)},
                            closed, 0, &out);
    ok = require(!out.empty() && InScreen(out, 96, 192), "narrow") && ok;

    // Open: old lines show too, with the input row and its cursor.
    NdsChatOverlayInput open;
    open.open = true;
    open.text = "hi";
    open.cursor = 2;
    out.clear();
    nds_chat_overlay_layout(256, 192, lines, open, 999999000, &out);
    bool cursor = false;
    for (const auto& r : out) cursor |= r.w == 1 && r.h == 8;
    ok = require(!out.empty() && InScreen(out, 256, 192) && cursor,
                 "open panel with cursor") && ok;
    return ok;
}

// Prints `rects` as text: '#' bright, '+' background.
bool TestEditing() {
    bool ok = true;
    NdsChatOverlayInput in;
    nds_chat_edit_insert(&in, "caf\xC3\xA9");  // café
    ok = require(in.text == "caf\xC3\xA9" && in.cursor == 5, "insert") && ok;
    nds_chat_edit_key(&in, NdsChatEditKey::Left);
    ok = require(in.cursor == 3, "left over a 2-byte char") && ok;
    nds_chat_edit_insert(&in, "a\tb\x7F\xC2\x85\x80\xFF");
    ok = require(in.text == "cafab\xC3\xA9" && in.cursor == 5,
                 "controls and stray bytes dropped") && ok;
    nds_chat_edit_key(&in, NdsChatEditKey::Delete);
    ok = require(in.text == "cafab" && in.cursor == 5, "delete") && ok;
    nds_chat_edit_key(&in, NdsChatEditKey::Home);
    nds_chat_edit_key(&in, NdsChatEditKey::Backspace);
    ok = require(in.text == "cafab" && in.cursor == 0,
                 "backspace at start") && ok;
    nds_chat_edit_key(&in, NdsChatEditKey::End);
    nds_chat_edit_key(&in, NdsChatEditKey::Right);
    nds_chat_edit_key(&in, NdsChatEditKey::Backspace);
    ok = require(in.text == "cafa" && in.cursor == 4, "backspace") && ok;

    NdsChatOverlayInput full;
    full.text.assign(kNdsChatTextMax - 1, 'x');
    full.cursor = full.text.size();
    nds_chat_edit_insert(&full, "\xE2\x82\xAC");  // € does not fit
    ok = require(full.text.size() == kNdsChatTextMax - 1,
                 "a character never split at the limit") && ok;
    nds_chat_edit_insert(&full, "yz");
    ok = require(full.text.size() == kNdsChatTextMax &&
                 full.text.back() == 'y' && full.cursor == kNdsChatTextMax,
                 "text kept within the limit") && ok;
    return ok;
}

void Print(int w, int h, const std::vector<NdsChatOverlayRect>& rects) {
    std::vector<std::string> screen(size_t(h), std::string(size_t(w), ' '));
    for (const auto& r : rects) {
        const bool dark = ((r.argb >> 16) & 255) < 64;
        for (int y = r.y; y < r.y + r.h && y < h; ++y)
            for (int x = r.x; x < r.x + r.w && x < w; ++x)
                screen[size_t(y)][size_t(x)] = dark ? '+' : '#';
    }
    for (const std::string& row : screen)
        if (row.find_first_not_of(' ') != std::string::npos)
            std::printf("%s\n", row.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
        std::vector<NdsChatOverlayRect> out;
        NdsChatOverlayInput input;
        input.open = true;
        input.text = "On my way, ¿qué tal?";
        input.cursor = 6;
        nds_chat_overlay_layout(
            256, 192,
            {Line("Ally", "The quick brown fox jumps over the lazy dog!", 0),
             Line("Me", "0123456789 @#$%&*()[]{}<>=+-_/\\|~^`'\",.;:", 0,
                  true),
             Line("Élodie", "café crème, naïve über Señor ça va? ß", 0)},
            input, 0, &out);
        Print(256, 192, out);
        return 0;
    }
    bool ok = TestGlyphs();
    ok = TestLayout() && ok;
    ok = TestEditing() && ok;
    if (!ok) return 1;
    std::printf("chat_overlay_test: ok\n");
    return 0;
}
