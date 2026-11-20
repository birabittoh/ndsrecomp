// touch_buttons.cpp -- see touch_buttons.h.

#include "touch_buttons.h"

#include <atomic>
#include <cstring>

namespace {

constexpr int kScreenHeight = 192;
constexpr int kCell = 2;
static_assert(kNdsTouchButtonWidth == 11 * kCell &&
                  kNdsTouchButtonHeight == kNdsTouchButtonArtRows * kCell,
              "icons are 11 x 9 cells");
constexpr uint32_t kBack = 0xA0000000u;
constexpr uint32_t kLit = 0xFFFFFFFFu;
// A button that is on (a toggle): the other way round.
constexpr uint32_t kActiveBack = 0xE0FFFFFFu;
constexpr uint32_t kActiveLit = 0xFF000000u;
// Extra DS pixels around a button that still count as a press on it.
constexpr float kSlack = 4.0f;

// Constant-initialized, so a title's static initializer may register first.
std::atomic<const NdsTouchButtonSpec*> g_specs[kNdsTouchButtonMax] = {};
std::atomic<int> g_count{0};
std::atomic<bool> g_shown[kNdsTouchButtonMax] = {};
std::atomic<bool> g_active[kNdsTouchButtonMax] = {};
std::atomic<bool (*)()> g_query[kNdsTouchButtonMax] = {};
std::atomic<int (*)()> g_opacity[kNdsTouchButtonMax] = {};

// `argb` with its alpha scaled to `percent`.
uint32_t Faded(uint32_t argb, int percent) {
    const uint32_t alpha = (argb >> 24) * static_cast<uint32_t>(percent) / 100;
    return (alpha << 24) | (argb & 0x00FFFFFFu);
}

const NdsTouchButtonSpec kChat = {
    {
        // A speech bubble with three dots.
        "...........",
        "..#######..",
        ".#########.",
        ".##.#.#.##.",
        ".#########.",
        "..#######..",
        "...##......",
        "...#.......",
        "...........",
    },
    NdsTouchButtonPlace::BottomRight,
    0,
    nullptr,
};

const NdsTouchButtonSpec kSend = {
    {
        // An arrow head pointing right.
        "...........",
        "..##.......",
        "..####.....",
        "..######...",
        "..########.",
        "..######...",
        "..####.....",
        "..##.......",
        "...........",
    },
    NdsTouchButtonPlace::BottomRight,
    0,
    nullptr,
};

const NdsTouchButtonSpec kFastForward = {
    {
        // Two arrow heads pointing right.
        "...........",
        ".#....#....",
        ".##...##...",
        ".###..###..",
        ".####.####.",
        ".###..###..",
        ".##...##...",
        ".#....#....",
        "...........",
    },
    NdsTouchButtonPlace::TopCenter,
    0,
    nullptr,
};

struct Box {
    int id;
    int x, y;
};

// The shown buttons and where they sit; returns how many.
int Boxes(int ds_width, Box out[kNdsTouchButtonMax]) {
    const int count = g_count.load(std::memory_order_acquire);
    int n = 0, row = 0;
    for (int id = 0; id < count && id < kNdsTouchButtonMax; ++id) {
        const NdsTouchButtonSpec* spec =
            g_specs[id].load(std::memory_order_acquire);
        if (!spec || !g_shown[id].load(std::memory_order_acquire)) continue;
        int x = kNdsTouchButtonMargin;
        int y = (kScreenHeight - kNdsTouchButtonHeight) / 2;
        switch (spec->place) {
            case NdsTouchButtonPlace::BottomRight:
                x = ds_width - (++row) * (kNdsTouchButtonWidth +
                                          kNdsTouchButtonMargin);
                y = kScreenHeight - kNdsTouchButtonMargin -
                    kNdsTouchButtonHeight;
                break;
            case NdsTouchButtonPlace::LeftMiddle:
                break;
            case NdsTouchButtonPlace::RightMiddle:
                x = ds_width - kNdsTouchButtonMargin - kNdsTouchButtonWidth;
                break;
            case NdsTouchButtonPlace::TopCenter:
                x = (ds_width - kNdsTouchButtonWidth) / 2;
                y = kNdsTouchButtonMargin;
                break;
            case NdsTouchButtonPlace::BottomCenter:
                x = (ds_width - kNdsTouchButtonWidth) / 2;
                y = kScreenHeight - kNdsTouchButtonMargin -
                    kNdsTouchButtonHeight;
                break;
        }
        if (x < 0) continue;
        out[n++] = {id, x, y};
    }
    return n;
}

}  // namespace

int nds_touch_button_register(const NdsTouchButtonSpec* spec) {
    if (!spec) return -1;
    int id = g_count.load(std::memory_order_relaxed);
    do {
        if (id >= kNdsTouchButtonMax) return -1;
    } while (!g_count.compare_exchange_weak(id, id + 1,
                                            std::memory_order_acq_rel));
    g_specs[id].store(spec, std::memory_order_release);
    return id;
}

const NdsTouchButtonSpec* nds_touch_button_spec(int id) {
    if (id < 0 || id >= kNdsTouchButtonMax) return nullptr;
    return g_specs[id].load(std::memory_order_acquire);
}

int nds_touch_button_chat() {
    static const int id = nds_touch_button_register(&kChat);
    return id;
}

int nds_touch_button_send() {
    static const int id = nds_touch_button_register(&kSend);
    return id;
}

int nds_touch_button_fast_forward() {
    static const int id = nds_touch_button_register(&kFastForward);
    return id;
}

void nds_touch_button_set_query(int id, bool (*shown)()) {
    if (id >= 0 && id < kNdsTouchButtonMax)
        g_query[id].store(shown, std::memory_order_release);
}

void nds_touch_button_set_opacity_query(int id, int (*opacity)()) {
    if (id >= 0 && id < kNdsTouchButtonMax)
        g_opacity[id].store(opacity, std::memory_order_release);
}

void nds_touch_button_set_active(int id, bool active) {
    if (id >= 0 && id < kNdsTouchButtonMax)
        g_active[id].store(active, std::memory_order_release);
}

void nds_touch_button_set_shown(int id, bool shown) {
    if (id >= 0 && id < kNdsTouchButtonMax)
        g_shown[id].store(shown, std::memory_order_release);
}

bool nds_touch_button_shown(int id) {
    return id >= 0 && id < kNdsTouchButtonMax &&
           g_shown[id].load(std::memory_order_acquire);
}

void nds_touch_buttons_update() {
    const int count = g_count.load(std::memory_order_acquire);
    for (int id = 0; id < count && id < kNdsTouchButtonMax; ++id) {
        const NdsTouchButtonSpec* spec =
            g_specs[id].load(std::memory_order_acquire);
        bool (*shown)() = g_query[id].load(std::memory_order_acquire);
        if (!shown && spec) shown = spec->shown;
        if (spec && shown)
            g_shown[id].store(shown(), std::memory_order_release);
    }
}

void nds_touch_buttons_layout(int ds_width,
                              std::vector<NdsTouchButtonRect>* out) {
    Box boxes[kNdsTouchButtonMax];
    const int n = Boxes(ds_width, boxes);
    for (int i = 0; i < n; ++i) {
        const Box& b = boxes[i];
        const bool active = g_active[b.id].load(std::memory_order_acquire);
        int (*opacity)() = g_opacity[b.id].load(std::memory_order_acquire);
        int percent = opacity ? opacity() : 100;
        percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
        const uint32_t back = Faded(active ? kActiveBack : kBack, percent);
        const uint32_t lit = Faded(active ? kActiveLit : kLit, percent);
        out->push_back({b.x, b.y, kNdsTouchButtonWidth, kNdsTouchButtonHeight,
                        back});
        const NdsTouchButtonSpec* spec = nds_touch_button_spec(b.id);
        for (int r = 0; r < kNdsTouchButtonArtRows; ++r) {
            const char* row = spec->art[r];
            if (!row) continue;
            const int len = static_cast<int>(std::strlen(row));
            for (int c = 0; c < len && c < 11;) {
                if (row[c] != '#') { ++c; continue; }
                int run = 1;
                while (c + run < len && c + run < 11 && row[c + run] == '#')
                    ++run;
                out->push_back({b.x + c * kCell, b.y + r * kCell,
                                run * kCell, kCell, lit});
                c += run;
            }
        }
    }
}

bool nds_touch_buttons_any() {
    const int count = g_count.load(std::memory_order_acquire);
    for (int id = 0; id < count && id < kNdsTouchButtonMax; ++id)
        if (g_specs[id].load(std::memory_order_acquire) &&
            g_shown[id].load(std::memory_order_acquire))
            return true;
    return false;
}

int nds_touch_buttons_extent() {
    const int count = g_count.load(std::memory_order_acquire);
    int row = 0;
    for (int id = 0; id < count && id < kNdsTouchButtonMax; ++id) {
        const NdsTouchButtonSpec* spec =
            g_specs[id].load(std::memory_order_acquire);
        if (spec && spec->place == NdsTouchButtonPlace::BottomRight &&
            g_shown[id].load(std::memory_order_acquire))
            ++row;
    }
    return row ? kNdsTouchButtonMargin +
                     row * (kNdsTouchButtonWidth + kNdsTouchButtonMargin)
               : 0;
}

int nds_touch_buttons_hit(int ds_width, float x, float y) {
    if (x < 0.0f || x >= ds_width || y < 0.0f || y >= kScreenHeight)
        return -1;
    Box boxes[kNdsTouchButtonMax];
    const int n = Boxes(ds_width, boxes);
    int best = -1;
    float best_distance = 0.0f;
    for (int i = 0; i < n; ++i) {
        const Box& b = boxes[i];
        if (x < b.x - kSlack || x >= b.x + kNdsTouchButtonWidth + kSlack ||
            y < b.y - kSlack || y >= b.y + kNdsTouchButtonHeight + kSlack)
            continue;
        const float dx = x - (b.x + kNdsTouchButtonWidth / 2.0f);
        const float dy = y - (b.y + kNdsTouchButtonHeight / 2.0f);
        const float distance = dx * dx + dy * dy;
        if (best < 0 || distance < best_distance) {
            best = b.id;
            best_distance = distance;
        }
    }
    return best;
}
