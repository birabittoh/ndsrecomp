// chat_overlay.h -- the netplay chat drawn over the game (net/netplay_chat.h).
//
// The frontend draws it in DS pixels of one screen (the top screen, or the
// shown one in the single-screen layout), through the same overlay path as
// the savestate notice, so it keeps its size relative to the image.
//
// Closed, the newest lines show at the bottom left for a few seconds after
// they arrive, then fade. Open (a line being typed), a panel shows the
// recent lines and the input line below them.
//
// The layout is a list of filled rectangles, so it is the same on every
// presenter: an SDL renderer, or the GPU presenter's ARGB overlay buffer.

#pragma once

#include "net/netplay_chat.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct NdsChatOverlayRect {
    int x = 0, y = 0, w = 0, h = 0;  // DS pixels
    uint32_t argb = 0;
};

// The line being typed. Safe to set from any thread.
struct NdsChatOverlayInput {
    bool open = false;
    std::string text;   // UTF-8
    size_t cursor = 0;  // byte offset into text
};
void nds_chat_overlay_set_input(const NdsChatOverlayInput& input);
NdsChatOverlayInput nds_chat_overlay_input();

// Glyph cell: 5x8 pixels (7 above the baseline, 1 below) in a 6x9 cell.
constexpr int kNdsChatCellWidth = 6;
constexpr int kNdsChatCellHeight = 9;
// A line shows this long once it has arrived, fading over the last second.
constexpr int64_t kNdsChatShowMs = 10000;
constexpr int64_t kNdsChatFadeMs = 1000;

// Rows of the 8 glyph rows of `cp`, bit 4 = leftmost pixel.
void nds_chat_glyph(uint32_t cp, uint8_t rows[8]);

// Lays out `lines` (oldest first) and `input` over a `width` x `height`
// screen at time `now_ms` (steady_clock ms). Appends to `out`.
void nds_chat_overlay_layout(int width, int height,
                             const std::vector<NdsChatLine>& lines,
                             const NdsChatOverlayInput& input, int64_t now_ms,
                             std::vector<NdsChatOverlayRect>* out);

// Size of the chat against the screen: 1 = a font pixel is a DS pixel,
// 0.5 = half that, and so on. A title sets a source to tie it to its own UI
// size; without one it is 1. The frontend lays the chat out over a screen of
// (width / scale) x (192 / scale) chat pixels. Clamped to [1/4, 1].
using NdsChatScaleSource = double (*)();
void nds_chat_overlay_set_scale_source(NdsChatScaleSource source);
double nds_chat_overlay_scale();

// For the frontend, once per presented frame: the current chat over a
// `width` x `height` screen. False when there is nothing to draw.
bool nds_chat_overlay_frame(int width, int height,
                            std::vector<NdsChatOverlayRect>* out);
