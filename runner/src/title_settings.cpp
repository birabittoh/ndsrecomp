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

int nds_title_display_height() {
    return g_display_height.load(std::memory_order_relaxed);
}

void nds_title_set_display_height(int points) {
    g_display_height.store(points > 0 ? points : 0,
                           std::memory_order_relaxed);
}
