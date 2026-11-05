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

    const NdsInitialWindowSize single =
        nds_initial_window_size(256, 256, false, 3, 1.0f, 3840, 2080, true);
    check(single.top_width == 768 && single.height == 576,
          "single window is one screen at the requested scale");

    // Auto internal scale (resolution_scale), bilinear: round up.
    check(nds_auto_internal_scale(1080, 192, S::Bilinear, 100, 16) == 6,
          "1080p bilinear 100% -> 6x");
    check(nds_auto_internal_scale(384, 192, S::Bilinear, 100, 16) == 2,
          "exact 2x bilinear 100% -> 2x");
    check(nds_auto_internal_scale(2160, 192, S::Bilinear, 100, 16) == 12,
          "4K bilinear 100% -> 12x");
    check(nds_auto_internal_scale(1080, 192, S::Bilinear, 50, 16) == 3,
          "1080p bilinear 50% -> 3x");
    check(nds_auto_internal_scale(1080, 192, S::Bilinear, 10, 16) == 1,
          "low percent floors at 1x");
    check(nds_auto_internal_scale(1080, 192, S::Bilinear, 200, 16) == 12,
          "1080p bilinear 200% supersamples");
    check(nds_auto_internal_scale(2160, 192, S::Bilinear, 200, 16) == 16,
          "clamped to the maximum");
    check(nds_auto_internal_scale(192, 192, S::Bilinear, 100, 16) == 1,
          "original 1:1 at 100% -> 1x");
    check(nds_auto_internal_scale(192, 192, S::Bilinear, 200, 16) == 2,
          "original 1:1 at 200% supersamples");
    // Integer: divisors of k at or below 100%, multiples above.
    check(nds_auto_internal_scale(960, 192, S::Integer, 100, 16) == 5,
          "integer 5x at 100% -> 5x");
    check(nds_auto_internal_scale(1152, 192, S::Integer, 50, 16) == 3,
          "integer 6x at 50% -> 3x");
    check(nds_auto_internal_scale(1152, 192, S::Integer, 75, 16) == 3,
          "integer 6x at 75% -> largest divisor under 4.5 -> 3x");
    check(nds_auto_internal_scale(960, 192, S::Integer, 50, 16) == 1,
          "integer 5x at 50% -> 1x (5 is prime)");
    check(nds_auto_internal_scale(768, 192, S::Integer, 150, 16) == 8,
          "integer 4x at 150% -> 8x");
    check(nds_auto_internal_scale(1920, 192, S::Integer, 200, 16) == 10,
          "integer 10x at 200% falls back to 10x");
    check(nds_auto_internal_scale(192, 192, S::Integer, 200, 16) == 2,
          "integer 1x at 200% -> 2x");
    check(nds_auto_internal_scale(0, 192, S::Bilinear, 100, 16) == 1,
          "degenerate rect -> 1x");

    if (failures) return 1;
    std::puts("frontend window size checks passed");
    return 0;
}
