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
    check(rect_is(nds_present_rect(M::Letterbox, S::Nearest, 1920, 1080, 256,
                                   192), 240, 0, 1440, 1080),
          "letterbox nearest is the exact fit");
    check(nds_auto_internal_scale(1080, 192, S::Nearest, 100, 16) == 6,
          "1080p nearest 100% rounds up like bilinear");
    check(nds_auto_internal_scale(0, 192, S::Bilinear, 100, 16) == 1,
          "degenerate rect -> 1x");

    {
        const auto off = nds_single_layout(M::Letterbox, S::Nearest, 1920,
                                           1080, 256, 192, 0);
        check(rect_is(off.main, 240, 0, 1440, 1080) && off.inset.w == 0,
              "no inset: the plain rect");
        // The inset takes the whole bar: the main screen keeps its full
        // height and nothing is left empty.
        const auto wide = nds_single_layout(M::Letterbox, S::Nearest, 1920,
                                            1080, 256, 192, 25);
        check(rect_is(wide.main, 0, 0, 1440, 1080),
              "16:9 inset leaves the main screen full height");
        check(rect_is(wide.inset, 1440, 720, 480, 360),
              "inset grows into the bar, bottom-aligned, keeps 4:3");
        // The percent is a floor: a bigger one still shrinks the main screen.
        const auto floor = nds_single_layout(M::Letterbox, S::Nearest, 1920,
                                             1080, 256, 192, 60);
        check(rect_is(floor.main, 0, 89, 1201, 901) &&
                  rect_is(floor.inset, 1201, 450, 719, 540),
              "inset never drops below the requested percent");
        // Never larger than the main screen, however wide the window.
        const auto ultra = nds_single_layout(M::Letterbox, S::Nearest, 4000,
                                             1080, 256, 192, 25);
        check(rect_is(ultra.main, 560, 0, 1440, 1080) &&
                  rect_is(ultra.inset, 2000, 0, 1440, 1080),
              "inset growth stops at the main screen's size");
        // 4:3 has no bars to spend, so the pair shrinks to fit.
        const auto square = nds_single_layout(M::Letterbox, S::Nearest, 640,
                                              480, 256, 192, 25);
        check(rect_is(square.main, 0, 48, 512, 384),
              "4:3 window fits the pair");
        check(rect_is(square.inset, 512, 336, 128, 96),
              "4:3 inset at a quarter size");
        // Integer keeps the main screen on whole multiples.
        const auto integer = nds_single_layout(M::Letterbox, S::Integer, 1920,
                                               1080, 256, 192, 25);
        check(integer.main.w == 256 * 5 && integer.main.h == 192 * 5,
              "integer main stays a whole multiple");
        const auto original = nds_single_layout(M::Original, S::Nearest, 1000,
                                                800, 256, 192, 50);
        check(original.main.w == 256 && original.main.h == 192 &&
                  original.inset.w == 128 && original.inset.h == 96,
              "original stays 1:1");
        // A portrait window moves the inset above the main screen, centred.
        const auto tall = nds_single_layout(M::Letterbox, S::Nearest, 480,
                                            640, 256, 192, 25);
        check(rect_is(tall.main, 0, 280, 480, 360),
              "portrait main keeps the full width, at the bottom");
        check(rect_is(tall.inset, 53, 0, 373, 280),
              "portrait inset fills the rest, centred, keeps 4:3");
        const auto tower = nds_single_layout(M::Letterbox, S::Nearest, 480,
                                             1000, 256, 192, 25);
        check(rect_is(tower.inset, 0, 140, 480, 360) &&
                  rect_is(tower.main, 0, 500, 480, 360),
              "portrait inset growth stops at the main screen's size");
        const auto tall_off = nds_single_layout(M::Letterbox, S::Nearest, 480,
                                                640, 256, 192, 0);
        check(rect_is(tall_off.main, 0, 140, 480, 360) &&
                  tall_off.inset.w == 0,
              "portrait with no inset: the plain rect");
        const auto tall_integer = nds_single_layout(
            M::Letterbox, S::Integer, 1080, 1920, 256, 192, 25);
        check(tall_integer.main.w == 256 * 4 &&
                  tall_integer.main.h == 192 * 4,
              "portrait integer main stays a whole multiple");
        const auto tall_original = nds_single_layout(
            M::Original, S::Nearest, 800, 1000, 256, 192, 50);
        check(tall_original.main.w == 256 && tall_original.main.h == 192 &&
                  tall_original.inset.w == 128 &&
                  tall_original.inset.h == 96 &&
                  tall_original.main.y ==
                      tall_original.inset.y + tall_original.inset.h,
              "portrait original stays 1:1");
        const auto tall_stretch = nds_single_layout(
            M::Stretch, S::Nearest, 480, 640, 256, 192, 25);
        check(tall_stretch.inset.w == 120,
              "stretch keeps the requested percent");
        // A square window is not portrait.
        const auto even = nds_single_layout(M::Letterbox, S::Nearest, 640,
                                            640, 256, 192, 25);
        check(even.inset.x == even.main.x + even.main.w,
              "square window keeps the inset beside main");
        // 100: both screens the same size, side by side or stacked.
        const auto full = nds_single_layout(M::Letterbox, S::Nearest, 1920,
                                            1080, 256, 192, 100);
        check(rect_is(full.main, 0, 180, 960, 720) &&
                  rect_is(full.inset, 960, 180, 960, 720),
              "100% inset matches the main screen, side by side");
        const auto full_tall = nds_single_layout(
            M::Letterbox, S::Nearest, 1080, 1920, 256, 192, 100);
        check(rect_is(full_tall.inset, 0, 150, 1080, 810) &&
                  rect_is(full_tall.main, 0, 960, 1080, 810),
              "100% portrait inset matches the main screen, above it");
        // A forced stack keeps the DS order: the inset below the main one.
        const auto stacked_tall = nds_single_layout(
            M::Letterbox, S::Nearest, 1080, 1920, 256, 192, 100, true);
        check(rect_is(stacked_tall.main, 0, 150, 1080, 810) &&
                  rect_is(stacked_tall.inset, 0, 960, 1080, 810),
              "stacked portrait keeps the inset below the main screen");
        const auto stacked_wide = nds_single_layout(
            M::Letterbox, S::Nearest, 1920, 1080, 256, 192, 100, true);
        check(stacked_wide.inset.y == stacked_wide.main.y + stacked_wide.main.h,
              "stacked landscape keeps the inset below the main screen");
    }

    // The initial single window counts the inset beside the main screen.
    const NdsInitialWindowSize single_full =
        nds_initial_window_size(256, 256, false, 8, 1.0f, 1920, 1080, true,
                                100);
    check(single_full.top_width == 912 && single_full.height == 684,
          "single window with a 100% inset fits the display width");

    if (failures) return 1;
    std::puts("frontend window size checks passed");
    return 0;
}
