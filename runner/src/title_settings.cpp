#include "title_settings.h"

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

void nds_title_set_touch_unscaled(bool unscaled) {
    g_touch_unscaled.store(unscaled, std::memory_order_relaxed);
}

bool nds_title_touch_unscaled() {
    return g_touch_unscaled.load(std::memory_order_relaxed);
}
