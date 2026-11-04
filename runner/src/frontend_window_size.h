#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct NdsInitialWindowSize {
    int top_width, bottom_width, height;
};

// `single` is the one-screen layout: one window, one 192-line screen as wide
// as the wider of the two. It takes precedence over `separate`.
inline NdsInitialWindowSize nds_initial_window_size(
    int top_width, int bottom_width, bool separate, int scale,
    float coordinate_scale, int usable_width, int usable_height,
    bool single = false) {
    double factor = std::clamp(scale, 1, 8) *
        (std::isfinite(coordinate_scale) && coordinate_scale > 0
            ? coordinate_scale : 1.0);
    const int height = separate || single ? 192 : 384;
    const int width = separate && !single
                          ? top_width + bottom_width
                          : std::max(top_width, bottom_width);
    // Fit both windows together, reserving space for borders/taskbar/gap.
    if (usable_width > 96)
        factor = std::min(factor, (usable_width - 96.0) / width);
    if (usable_height > 80)
        factor = std::min(factor, (usable_height - 80.0) / height);
    return {std::max(1, static_cast<int>(std::lround(top_width * factor))),
            std::max(1, static_cast<int>(std::lround(bottom_width * factor))),
            std::max(1, static_cast<int>(std::lround(height * factor)))};
}

// Where the presented image goes inside a window's drawable.
enum class NdsWindowMode : uint8_t {
    Letterbox,  // largest aspect-correct fit, centred
    Stretch,    // fill the drawable, ignoring the aspect ratio
    Original,   // strict 1:1 in physical pixels, centred, never scaled
};

// How the image is resampled into that rect.
enum class NdsScalingFilter : uint8_t {
    Integer,   // pixel-perfect: nearest, whole-multiple factors only
    Bilinear,  // linear filtering to the exact rect
};

struct NdsPresentRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// The one presentation rect, shared by both back ends and the mouse-to-touch
// inversion. Everything is in drawable (physical) pixels, so "1:1" and
// "integer" mean device pixels on a HiDPI display too. The rect is centred and
// may extend past the drawable (Original or a forced 1x in a tiny window):
// the excess is clipped, never squeezed. A degenerate input yields {0,0,0,0}.
inline NdsPresentRect nds_present_rect(NdsWindowMode mode,
                                       NdsScalingFilter scaling,
                                       int drawable_w, int drawable_h,
                                       int content_w, int content_h) {
    NdsPresentRect rect{};
    if (drawable_w <= 0 || drawable_h <= 0 || content_w <= 0 ||
        content_h <= 0)
        return rect;
    const bool integer = scaling == NdsScalingFilter::Integer;
    switch (mode) {
        case NdsWindowMode::Original:
            rect.w = content_w;
            rect.h = content_h;
            break;
        case NdsWindowMode::Stretch:
            if (integer) {
                rect.w = std::max(1, drawable_w / content_w) * content_w;
                rect.h = std::max(1, drawable_h / content_h) * content_h;
            } else {
                rect.w = drawable_w;
                rect.h = drawable_h;
            }
            break;
        case NdsWindowMode::Letterbox:
        default:
            if (integer) {
                const int k = std::max(
                    1, std::min(drawable_w / content_w,
                                drawable_h / content_h));
                rect.w = k * content_w;
                rect.h = k * content_h;
            } else {
                rect.w = drawable_w;
                rect.h = static_cast<int>(
                    static_cast<long long>(drawable_w) * content_h /
                    content_w);
                if (rect.h > drawable_h) {
                    rect.h = drawable_h;
                    rect.w = static_cast<int>(
                        static_cast<long long>(drawable_h) * content_w /
                        content_h);
                }
                rect.w = std::max(rect.w, 1);
                rect.h = std::max(rect.h, 1);
            }
            break;
    }
    rect.x = (drawable_w - rect.w) / 2;
    rect.y = (drawable_h - rect.h) / 2;
    return rect;
}

// Inverse of the rect: a drawable-pixel point to content coordinates. Points
// outside the rect map outside [0, content) rather than being clamped, so a
// caller can tell a click on the bars from a click on the image.
inline void nds_present_rect_to_content(const NdsPresentRect& rect,
                                        int content_w, int content_h,
                                        float px, float py,
                                        float* cx, float* cy) {
    if (rect.w <= 0 || rect.h <= 0) {
        if (cx) *cx = -1.0f;
        if (cy) *cy = -1.0f;
        return;
    }
    if (cx) *cx = (px - rect.x) * static_cast<float>(content_w) / rect.w;
    if (cy) *cy = (py - rect.y) * static_cast<float>(content_h) / rect.h;
}
