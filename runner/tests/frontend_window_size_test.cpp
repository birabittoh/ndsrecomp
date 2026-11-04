#include "frontend_window_size.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

bool rect_is(const NdsPresentRect& r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

}  // namespace

int main() {
    using M = NdsWindowMode;
    using S = NdsScalingFilter;

    // Letterbox + integer: today's SDL look, largest whole multiple.
    check(rect_is(nds_present_rect(M::Letterbox, S::Integer, 512, 384, 256,
                                   192), 0, 0, 512, 384),
          "letterbox integer exact 2x");
    check(rect_is(nds_present_rect(M::Letterbox, S::Integer, 1920, 1080, 256,
                                   192), 320, 60, 1280, 960),
          "letterbox integer 1080p picks 5x");
    check(rect_is(nds_present_rect(M::Letterbox, S::Integer, 200, 100, 256,
                                   192), -28, -46, 256, 192),
          "letterbox integer never below 1x");

    // Letterbox + bilinear: exact aspect fit.
    check(rect_is(nds_present_rect(M::Letterbox, S::Bilinear, 1920, 1080,
                                   256, 192), 240, 0, 1440, 1080),
          "letterbox bilinear 1080p pillarbox");
    check(rect_is(nds_present_rect(M::Letterbox, S::Bilinear, 800, 1000, 256,
                                   192), 0, 200, 800, 600),
          "letterbox bilinear tall window");

    // Stretch.
    check(rect_is(nds_present_rect(M::Stretch, S::Bilinear, 1920, 1080, 256,
                                   192), 0, 0, 1920, 1080),
          "stretch bilinear fills");
    check(rect_is(nds_present_rect(M::Stretch, S::Integer, 1920, 1080, 256,
                                   192), 64, 60, 1792, 960),
          "stretch integer independent factors 7x5");

    // Original: strict 1:1 whatever the filter.
    check(rect_is(nds_present_rect(M::Original, S::Bilinear, 1920, 1080, 256,
                                   192), 832, 444, 256, 192),
          "original centred 1:1");
    check(rect_is(nds_present_rect(M::Original, S::Integer, 1920, 1080, 256,
                                   192), 832, 444, 256, 192),
          "original ignores scaling");

    // Stacked content.
    check(rect_is(nds_present_rect(M::Letterbox, S::Integer, 1920, 1080, 256,
                                   384), 704, 156, 512, 768),
          "stacked letterbox integer");

    // Degenerate input.
    check(rect_is(nds_present_rect(M::Letterbox, S::Bilinear, 0, 100, 256,
                                   192), 0, 0, 0, 0),
          "degenerate drawable");

    // Inversion maps rect corners to content corners, bars outside.
    const NdsPresentRect r = nds_present_rect(M::Letterbox, S::Bilinear, 1920,
                                              1080, 256, 192);
    float cx = 0, cy = 0;
    nds_present_rect_to_content(r, 256, 192, 240.0f, 0.0f, &cx, &cy);
    check(cx == 0.0f && cy == 0.0f, "inverse top-left");
    nds_present_rect_to_content(r, 256, 192, 240.0f + 1440.0f * 0.5f, 540.0f,
                                &cx, &cy);
    check(cx == 128.0f && cy == 96.0f, "inverse centre");
    nds_present_rect_to_content(r, 256, 192, 100.0f, 540.0f, &cx, &cy);
    check(cx < 0.0f, "inverse left bar is outside");

    if (failures) return 1;
    std::puts("frontend window size checks passed");
    return 0;
}
