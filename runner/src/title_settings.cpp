#include "title_settings.h"

#include "io.h"

#include <atomic>
#include <cstring>
#include <deque>

namespace {

struct Entry {
    const NdsTitleSetting* setting;
    std::atomic<int> value;
};

// A deque keeps the atomics in place as the registry grows. Built from
// static initializers, so it is a function-local static.
std::deque<Entry>& registry() {
    static std::deque<Entry> entries;
    return entries;
}

std::atomic<int> g_display_height{0};
// -1, or y << 16 | x.
std::atomic<int> g_pointer_hover{-1};

bool accepts(const NdsTitleSetting& s, int value) {
    if (s.choices && s.choice_count) {
        for (size_t i = 0; i < s.choice_count; ++i) {
            const int v = s.choice_values ? s.choice_values[i]
                                          : s.minimum + static_cast<int>(i);
            if (v == value) return true;
        }
        return false;
    }
    return value >= s.minimum && value <= s.maximum;
}

}  // namespace

int nds_title_setting_register(const NdsTitleSetting* setting) {
    std::deque<Entry>& entries = registry();
    entries.emplace_back();
    entries.back().setting = setting;
    entries.back().value.store(setting->default_value,
                               std::memory_order_relaxed);
    return static_cast<int>(entries.size() - 1);
}

int nds_title_setting_value(int id) {
    return registry()[static_cast<size_t>(id)].value.load(
        std::memory_order_relaxed);
}

size_t nds_title_setting_count() { return registry().size(); }

const NdsTitleSetting* nds_title_setting_at(int id) {
    if (id < 0 || static_cast<size_t>(id) >= registry().size()) return nullptr;
    return registry()[static_cast<size_t>(id)].setting;
}

int nds_title_setting_find(const char* key) {
    if (!key) return -1;
    const std::deque<Entry>& entries = registry();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (std::strcmp(entries[i].setting->key, key) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

bool nds_title_setting_set(int id, int value) {
    const NdsTitleSetting* setting = nds_title_setting_at(id);
    if (!setting || !accepts(*setting, value)) return false;
    registry()[static_cast<size_t>(id)].value.store(
        value, std::memory_order_relaxed);
    return true;
}

namespace {
struct ZoomBinding {
    int value_id = -1;
    int mode_id = -1;
    int active_mode_value = 0;
};
ZoomBinding g_zoom_binding;
}  // namespace

void nds_title_set_zoom_binding(int value_id, int mode_id,
                                int active_mode_value) {
    g_zoom_binding = {value_id, mode_id, active_mode_value};
}

bool nds_title_zoom_add(int delta) {
    const ZoomBinding b = g_zoom_binding;
    const NdsTitleSetting* setting = nds_title_setting_at(b.value_id);
    if (!setting || delta == 0) return false;
    if (nds_title_setting_value(b.mode_id) != b.active_mode_value)
        return false;
    const int current = nds_title_setting_value(b.value_id);
    long next = static_cast<long>(current) + delta;
    if (next < setting->minimum) next = setting->minimum;
    if (next > setting->maximum) next = setting->maximum;
    if (next == current) return false;
    return nds_title_setting_set(b.value_id, static_cast<int>(next));
}

namespace {
std::atomic<NdsTitleStackedLayoutQuery> g_stacked_layout_query{nullptr};
}  // namespace

void nds_title_set_stacked_layout_query(NdsTitleStackedLayoutQuery query) {
    g_stacked_layout_query.store(query, std::memory_order_relaxed);
}

bool nds_title_stacked_layout_wanted() {
    const NdsTitleStackedLayoutQuery query =
        g_stacked_layout_query.load(std::memory_order_relaxed);
    return query && query();
}

int nds_title_display_height() {
    return g_display_height.load(std::memory_order_relaxed);
}

void nds_title_set_display_height(int points) {
    g_display_height.store(points > 0 ? points : 0,
                           std::memory_order_relaxed);
}

bool nds_title_pointer_hover(int* x, int* y) {
    const int packed = g_pointer_hover.load(std::memory_order_relaxed);
    if (packed < 0) return false;
    *x = packed & 0xFFFF;
    *y = packed >> 16;
    return true;
}

void nds_title_set_pointer_hover(int x, int y, bool hovering) {
    g_pointer_hover.store(hovering ? (y << 16 | x) : -1,
                          std::memory_order_relaxed);
}

namespace {
std::atomic<bool> g_right_mouse_held{false};
}  // namespace

bool nds_title_right_mouse_held() {
    return g_right_mouse_held.load(std::memory_order_relaxed);
}

void nds_title_set_right_mouse_held(bool held) {
    g_right_mouse_held.store(held, std::memory_order_relaxed);
}

namespace {
// bit 32 = active, then int16 y << 16 | uint16 x.
std::atomic<uint64_t> g_left_stick{0};
}  // namespace

bool nds_title_left_stick(int* x, int* y) {
    const uint64_t v = g_left_stick.load(std::memory_order_relaxed);
    if (!(v >> 32)) return false;
    *x = static_cast<int16_t>(v & 0xFFFF);
    *y = static_cast<int16_t>((v >> 16) & 0xFFFF);
    return true;
}

void nds_title_set_left_stick(int x, int y, bool active) {
    auto axis = [](int v) {
        v = v < -32767 ? -32767 : v > 32767 ? 32767 : v;
        return static_cast<uint64_t>(static_cast<uint16_t>(v));
    };
    g_left_stick.store(active ? (uint64_t{1} << 32 | axis(y) << 16 | axis(x))
                              : 0,
                       std::memory_order_relaxed);
}

namespace {
// -1, or y << 16 | x.
std::atomic<int> g_top_pointer{-1};
// count << 32 | y << 16 | x of the latest click.
std::atomic<uint64_t> g_top_click{0};
}  // namespace

bool nds_title_top_pointer(int* x, int* y) {
    const int packed = g_top_pointer.load(std::memory_order_relaxed);
    if (packed < 0) return false;
    *x = packed & 0xFFFF;
    *y = packed >> 16;
    return true;
}

unsigned nds_title_top_click(int* x, int* y) {
    const uint64_t v = g_top_click.load(std::memory_order_relaxed);
    *x = static_cast<int>(v & 0xFFFF);
    *y = static_cast<int>((v >> 16) & 0xFFFF);
    return static_cast<unsigned>(v >> 32);
}

void nds_title_set_top_pointer(int x, int y, bool over) {
    g_top_pointer.store(over ? (y << 16 | x) : -1, std::memory_order_relaxed);
}

void nds_title_note_top_click(int x, int y) {
    const uint64_t old = g_top_click.load(std::memory_order_relaxed);
    const uint64_t count = ((old >> 32) + 1) & 0xFFFFFFFFu;
    g_top_click.store(count << 32 | static_cast<uint64_t>(y & 0xFFFF) << 16 |
                          static_cast<uint64_t>(x & 0xFFFF),
                      std::memory_order_relaxed);
}

namespace {
// bit 32 = active, then int16 y << 16 | int16 x.
std::atomic<uint64_t> g_top_drag{0};
}  // namespace

bool nds_title_top_drag(int* x, int* y) {
    const uint64_t v = g_top_drag.load(std::memory_order_relaxed);
    if (!(v >> 32)) return false;
    *x = static_cast<int16_t>(v & 0xFFFF);
    *y = static_cast<int16_t>((v >> 16) & 0xFFFF);
    return true;
}

void nds_title_set_top_drag(int x, int y, bool active) {
    auto coord = [](int v) {
        v = v < -32767 ? -32767 : v > 32767 ? 32767 : v;
        return static_cast<uint64_t>(static_cast<uint16_t>(v));
    };
    g_top_drag.store(active ? (uint64_t{1} << 32 | coord(y) << 16 | coord(x))
                            : 0,
                     std::memory_order_relaxed);
}

namespace {
std::atomic<uint64_t> g_unscaled_touch_rect{0};
}  // namespace

void nds_title_set_unscaled_touch_rect(int x0, int y0, int x1, int y1) {
    auto clamp = [](int v) {
        return static_cast<uint64_t>(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v);
    };
    g_unscaled_touch_rect.store(
        clamp(x0) | clamp(y0) << 16 | clamp(x1) << 32 | clamp(y1) << 48,
        std::memory_order_relaxed);
}

bool nds_title_unscaled_touch_rect(int* x0, int* y0, int* x1, int* y1) {
    const uint64_t p = g_unscaled_touch_rect.load(std::memory_order_relaxed);
    *x0 = static_cast<int>(p & 0xFFFF);
    *y0 = static_cast<int>(p >> 16 & 0xFFFF);
    *x1 = static_cast<int>(p >> 32 & 0xFFFF);
    *y1 = static_cast<int>(p >> 48 & 0xFFFF);
    return *x1 > *x0 && *y1 > *y0;
}

namespace {
std::atomic<bool> g_touch_unscaled{false};
}  // namespace

void nds_title_set_arm9_overclock(int multiple) {
    if (multiple > 0) nds_request_arm9_overclock(static_cast<unsigned>(multiple));
}

void nds_title_set_card_speedup(int factor) {
    if (factor > 0) nds_request_card_speedup(static_cast<unsigned>(factor));
}

void nds_title_set_event_windows(bool enabled) {
    nds_request_event_windows(enabled);
}

void nds_title_set_touch_unscaled(bool unscaled) {
    g_touch_unscaled.store(unscaled, std::memory_order_relaxed);
}

bool nds_title_touch_unscaled() {
    return g_touch_unscaled.load(std::memory_order_relaxed);
}
