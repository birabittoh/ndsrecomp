#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// Largest internal-resolution (3D sample) multiplier the accelerated renderer
// is allowed to build. gpu3d enforces it; the frontend clamps auto scale to it.
constexpr uint8_t kNdsMaxInternalScale = 16u;

struct NdsInitialWindowSize {
    int top_width, bottom_width, height;
};

// `single` is the one-screen layout: one window, one 192-line screen as wide
// as the wider of the two. It takes precedence over `separate`. The caller
// widens that window by `inset_percent` for the second screen beside the main
// one, so the fit to the display counts it too; the returned widths stay one
// screen's.
inline NdsInitialWindowSize nds_initial_window_size(
    int top_width, int bottom_width, bool separate, int scale,
    float coordinate_scale, int usable_width, int usable_height,
    bool single = false, int inset_percent = 0) {
    double factor = std::clamp(scale, 1, 8) *
        (std::isfinite(coordinate_scale) && coordinate_scale > 0
            ? coordinate_scale : 1.0);
    const int height = separate || single ? 192 : 384;
    const int width = separate && !single
                          ? top_width + bottom_width
                          : std::max(top_width, bottom_width) *
                                (100 + (single ? std::max(inset_percent, 0)
                                               : 0)) / 100;
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
    Nearest,   // nearest filtering to the exact rect: pixelated, uneven
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

// The single-screen layout's two rects. With inset_percent above zero the
// other LCD is shown at that percent of its size: to the right of the main one
// and bottom-aligned, or, in a portrait drawable (taller than wide), just below
// it and centred. The pair is laid out as one image through nds_present_rect,
// so window_mode and scaling apply to it unchanged, the main screen is never
// covered, and the window spends its bars on the inset before shrinking the
// main screen. inset_percent is therefore a minimum: in Letterbox, space left
// over along that axis goes to the inset too, up to the main screen's size,
// and the pair is re-centred. inset is empty at zero.
struct NdsSingleLayout {
    NdsPresentRect main, inset;
};

// Set while a game mode forces the stacked arrangement (see
// title_settings.h, nds_title_set_stacked_layout_query): the presenters pass
// it as nds_single_layout's stack_vertically.
inline std::atomic<bool>& nds_stack_vertically_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

//
// stack_vertically puts the inset below the main screen whatever the
// drawable's shape (what a title forces on some game modes, with
// inset_percent 100: both screens the same size, one above the other).
inline NdsSingleLayout nds_single_layout(NdsWindowMode mode,
                                         NdsScalingFilter scaling,
                                         int drawable_w, int drawable_h,
                                         int content_w, int content_h,
                                         int inset_percent,
                                         bool stack_vertically = false) {
    NdsSingleLayout layout{};
    const int inset_content_w = content_w * inset_percent / 100;
    if (inset_percent <= 0 || inset_content_w <= 0) {
        layout.main = nds_present_rect(mode, scaling, drawable_w, drawable_h,
                                       content_w, content_h);
        return layout;
    }
    if (stack_vertically || drawable_h > drawable_w) {
        const int total_h = content_h + content_h * inset_percent / 100;
        const NdsPresentRect all = nds_present_rect(
            mode, scaling, drawable_w, drawable_h, content_w, total_h);
        layout.main = all;
        layout.main.h = static_cast<int>(
            static_cast<long long>(all.h) * content_h / total_h);
        layout.inset.h = all.h - layout.main.h;
        layout.inset.w = all.w * inset_percent / 100;
        layout.inset.x = all.x + (all.w - layout.inset.w) / 2;
        layout.inset.y = all.y + layout.main.h;
        const int room = drawable_h - layout.main.h;
        if (mode == NdsWindowMode::Letterbox && room > layout.inset.h) {
            layout.inset.h = std::min(room, layout.main.h);
            layout.inset.w = static_cast<int>(
                static_cast<long long>(layout.inset.h) * content_w /
                content_h);
            layout.main.y =
                (drawable_h - layout.main.h - layout.inset.h) / 2;
            layout.inset.x =
                layout.main.x + (layout.main.w - layout.inset.w) / 2;
            layout.inset.y = layout.main.y + layout.main.h;
        }
        return layout;
    }
    const int total_w = content_w + inset_content_w;
    const NdsPresentRect all = nds_present_rect(
        mode, scaling, drawable_w, drawable_h, total_w, content_h);
    layout.main = all;
    layout.main.w = static_cast<int>(
        static_cast<long long>(all.w) * content_w / total_w);
    layout.inset.w = all.w - layout.main.w;
    layout.inset.h = all.h * inset_percent / 100;
    layout.inset.x = all.x + layout.main.w;
    layout.inset.y = all.y + all.h - layout.inset.h;
    const int room = drawable_w - layout.main.w;
    if (mode == NdsWindowMode::Letterbox && room > layout.inset.w) {
        layout.inset.w = std::min(room, layout.main.w);
        layout.inset.h = static_cast<int>(
            static_cast<long long>(layout.inset.w) * content_h / content_w);
        layout.main.x = (drawable_w - layout.main.w - layout.inset.w) / 2;
        layout.inset.x = layout.main.x + layout.main.w;
        layout.inset.y = layout.main.y + layout.main.h - layout.inset.h;
    }
    return layout;
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

// The 3D internal-resolution multiplier that renders at `percent` of the
// presented rect's pixel density (resolution_scale). `rect_h` is the rect's
// height in drawable pixels and `content_h` the content's (192), so the
// letterbox bars never count. The compute renderer only takes whole
// multipliers:
// - bilinear and nearest round up (100% is never softer than the screen) and
//   let the presenter resample the remainder;
// - integer keeps 3D texels on whole screen pixels: the rect is k*content_h
//   tall, so at or below 100% the result is the largest divisor of k not
//   above the target, and above 100% the smallest multiple of k at or above
//   it (falling back to the largest multiple, then divisor, of k that fits
//   max_scale).
// The result is always in [1, max_scale].
inline uint8_t nds_auto_internal_scale(int rect_h, int content_h,
                                       NdsScalingFilter scaling, int percent,
                                       int max_scale) {
    max_scale = std::max(max_scale, 1);
    if (rect_h <= 0 || content_h <= 0 || percent <= 0) return 1u;
    int n = 1;
    if (scaling == NdsScalingFilter::Integer) {
        const int k = std::max(1, rect_h / content_h);
        if (percent <= 100) {
            for (int d = k; d >= 1; --d) {
                if (k % d == 0 && d * 100 <= k * percent && d <= max_scale) {
                    n = d;
                    break;
                }
            }
        } else {
            const int want = (percent + 99) / 100;  // multiples of k
            int m = std::min(want, max_scale / k);
            if (m >= 1) {
                n = m * k;
            } else {
                for (int d = std::min(k, max_scale); d >= 1; --d) {
                    if (k % d == 0) {
                        n = d;
                        break;
                    }
                }
            }
        }
    } else {
        const long long num = static_cast<long long>(rect_h) * percent;
        const long long den = static_cast<long long>(content_h) * 100;
        n = static_cast<int>((num + den - 1) / den);
    }
    return static_cast<uint8_t>(std::clamp(n, 1, max_scale));
}
