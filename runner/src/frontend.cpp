#include "frontend.h"
#include "host_profile.h"
#include "frontend_window_size.h"
#include "frontend_screen_route.h"
#include "arm9_native.h"
#include "cpu_render_handoff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "debug_server.h"
#include "diagnostics.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "melonds_compute/TextureUpscale.h"
#include "io.h"
#include "profile_report.h"
#include "relative_mouse_touch.h"
#include "runtime_arm.h"
#include "scheduler.h"
#include "savestate_slots.h"
#include "spu.h"
#include "title_patches.h"
#include "title_menu.h"
#include "title_settings.h"
#include "wifi_net.h"
#if defined(NDS_HAVE_COMPUTE_RENDERER)
#include "melonds_compute/ComputeHost.h"
#endif

namespace {
// Written per presented frame by the frontend loop, read from the same
// thread by the `frontend_stats` debug command at the debug_pump() safe
// point. Stays all-zero (active=0) when no frontend is running.
NdsFrontendLiveStats g_live_stats{};
NdsFrontendPacingStats g_pacing_stats{};
uint64_t g_pacing_previous_counter = 0;

void record_frontend_pacing(uint64_t counter, uint64_t emu_ticks,
                            uint64_t present_ticks, uint64_t frequency,
                            uint64_t overlapped_work_ticks = 0) {
    if (!g_pacing_stats.armed) return;
    g_pacing_stats.frequency = frequency;
    g_pacing_stats.max_emu_ticks =
        std::max(g_pacing_stats.max_emu_ticks, emu_ticks);
    g_pacing_stats.max_present_ticks =
        std::max(g_pacing_stats.max_present_ticks, present_ticks);
    const auto work_ticks=overlapped_work_ticks ? overlapped_work_ticks : emu_ticks+present_ticks;
    if (work_ticks > frequency / 60u)
        ++g_pacing_stats.work_over_budget;
    const uint64_t work_us = work_ticks * 1000000u / frequency;
    ++g_pacing_stats.work_histogram[std::min<uint64_t>(256u, work_us / 250u)];
    ++g_pacing_stats.work_samples;
    if (work_us <= 13300u) ++g_pacing_stats.work_at_most_13300us;
    if (g_pacing_previous_counter != 0 && counter >= g_pacing_previous_counter) {
        const uint64_t delta = counter - g_pacing_previous_counter;
        g_pacing_stats.max_interval_ticks =
            std::max(g_pacing_stats.max_interval_ticks, delta);
        const uint64_t microseconds = delta * 1000000u / frequency;
        ++g_pacing_stats.interval_histogram[std::min<uint64_t>(256u, microseconds / 250u)];
        constexpr uint64_t bounds[] = {17000, 20000, 25000, 33334, 50000};
        unsigned bucket = 0;
        while (bucket < 5 && microseconds > bounds[bucket]) ++bucket;
        ++g_pacing_stats.interval_buckets[bucket];
        ++g_pacing_stats.samples;
    }
    g_pacing_previous_counter = counter;
}
NdsFrontendBlackBandCapture g_black_band{};
NdsFrontendInputDebugState g_input_debug{};

// ---- Presented-frame digest ring (frontend.h) ----------------------------
constexpr uint32_t kFrameDigestRing = 8192;   // ~136 s at 60 fps
NdsFrontendFrameDigest g_frame_digests[kFrameDigestRing];
std::atomic<uint64_t> g_frame_digest_count{0};
// Incremented on every successful savestate LOAD. Two processes launched
// independently present a different number of frames before the load lands, so
// the epoch, not the frame index, is what aligns their digest streams.
uint32_t g_frame_digest_epoch = 0;
// Latched once, at the first present, from the environment: the ring covers
// the whole process life or none of it. Never armed by a probe.
int g_frame_digest_enabled = -1;

bool frame_digest_enabled() {
    if (g_frame_digest_enabled < 0) {
        const char* const value = std::getenv("NDS_FRAME_HASH");
        g_frame_digest_enabled =
            (value && value[0] == '1' && value[1] == '\0') ? 1 : 0;
    }
    return g_frame_digest_enabled != 0;
}

// FNV-1a over 32-bit words. Chosen for being trivially reproducible from a
// Python-side reimplementation, not for cryptographic strength.
uint64_t digest_words(const uint32_t* words, size_t count, uint64_t seed) {
    uint64_t h = seed;
    for (size_t i = 0; i < count; ++i) {
        h ^= words[i];
        h *= 1099511628211ull;
    }
    return h;
}

void record_frame_digest(uint64_t frame, const uint32_t* top, uint32_t top_w,
                         const uint32_t* bottom, uint32_t bottom_w,
                         bool direct_present,
                         const NdsGpu2dHdFrame* owned_hd = nullptr,
                         bool owned = false) {
    if (!frame_digest_enabled()) return;
    NdsGpu2dHdFrame hd{};
    const bool have_hd = owned ? (owned_hd ? (hd=*owned_hd,true) : false)
                               : nds_gpu2d_hd_frame_peek(&hd);
    NdsFrontendFrameDigest entry{};
    entry.frame = frame;
    entry.top_width = top_w;
    entry.bottom_width = bottom_w;
    entry.flags = (direct_present ? NDS_FRAME_DIGEST_DIRECT_PRESENT : 0u) |
                  (have_hd ? NDS_FRAME_DIGEST_HD : 0u);
    entry.epoch = g_frame_digest_epoch;
    // On a direct-present frame the top surface lives on the GPU and
    // `top_pixels` is still the 256-wide native framebuffer, while `top_width`
    // has already been widened for the presenter's geometry. Hashing top_width
    // words would read past the native buffer.
    const uint32_t top_hash_width = direct_present ? 256u : top_w;
    if (top)
        entry.top_hash = digest_words(
            top, static_cast<size_t>(top_hash_width) * 192u,
            1469598103934665603ull);
    if (bottom)
        entry.bottom_hash = digest_words(
            bottom, static_cast<size_t>(bottom_w) * 192u,
            1469598103934665603ull);
    if (have_hd && hd.top_pixels && hd.below_pixels) {
        const size_t words = static_cast<size_t>(hd.width) * 192u * 2u;
        uint64_t h = digest_words(hd.top_pixels, words,
                                  1469598103934665603ull);
        h = digest_words(hd.below_pixels, words, h);
        const uint32_t tail[7] = {
            hd.width, hd.priority_3d, hd.order_3d, hd.bldcnt,
            hd.master_bright,
            static_cast<uint32_t>(hd.eva) | (uint32_t{hd.evb} << 8) |
                (uint32_t{hd.evy} << 16),
            hd.render_xpos,
        };
        entry.hd_hash = digest_words(tail, 7, h);
    }
    const uint64_t index =
        g_frame_digest_count.load(std::memory_order_relaxed);
    g_frame_digests[index % kFrameDigestRing] = entry;
    g_frame_digest_count.store(index + 1, std::memory_order_release);
}

void observe_top_black_bands(const uint32_t* pixels, uint64_t frame,
                             uint64_t system_timestamp = UINT64_MAX) {
    if (!g_black_band.enabled || !pixels) return;
    ++g_black_band.scanned_frames;

    uint32_t current_start = 0;
    uint32_t current_rows = 0;
    uint32_t longest_start = 0;
    uint32_t longest_rows = 0;
    for (uint32_t y = 0; y < 192; ++y) {
        uint32_t black_pixels = 0;
        for (uint32_t x = 0; x < 256; ++x) {
            if ((pixels[y * 256 + x] & 0x00FFFFFFu) == 0)
                ++black_pixels;
        }
        if (black_pixels >= 252) {
            if (current_rows == 0) current_start = y;
            ++current_rows;
            if (current_rows > longest_rows) {
                longest_start = current_start;
                longest_rows = current_rows;
            }
        } else {
            current_rows = 0;
        }
    }

    // Ignore an intentional full-screen fade. The reported artifact is a
    // partial band surrounded by otherwise-published image rows.
    if (longest_rows < 8 || longest_rows >= 192) return;
    ++g_black_band.band_frames;
    if (longest_rows <= g_black_band.worst_row_count) return;
    g_black_band.has_capture = 1;
    g_black_band.worst_frame = frame;
    g_black_band.worst_system_timestamp = system_timestamp == UINT64_MAX
        ? scheduler_system_timestamp() : system_timestamp;
    g_black_band.worst_start_row = longest_start;
    g_black_band.worst_row_count = longest_rows;
    std::memcpy(g_black_band.top_pixels, pixels,
                sizeof(g_black_band.top_pixels));
}
}  // namespace

#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
#define SDL_MAIN_HANDLED
#if defined(NDS_HAVE_SDL3)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#else
#include <SDL.h>
#endif
#include "recomp_runtime_ui.h"
#if NDS_ONLINE
#include "netplay_menu.h"
#endif

#include "frontend_latency.h"

namespace {

#if defined(NDS_HAVE_SDL3)
#undef SDL_GameController
#undef SDL_GameControllerButton
#undef SDL_GameControllerClose
#undef SDL_GameControllerGetAxis
#undef SDL_GameControllerName
#undef SDL_GameControllerOpen
#undef SDL_CONTROLLER_AXIS_LEFTX
#undef SDL_CONTROLLER_AXIS_LEFTY
#undef SDL_CONTROLLER_AXIS_RIGHTX
#undef SDL_CONTROLLER_AXIS_RIGHTY
#undef SDL_CONTROLLER_AXIS_TRIGGERLEFT
#undef SDL_CONTROLLER_AXIS_TRIGGERRIGHT
#undef SDL_CONTROLLER_BUTTON_A
#undef SDL_CONTROLLER_BUTTON_B
#undef SDL_CONTROLLER_BUTTON_BACK
#undef SDL_CONTROLLER_BUTTON_DPAD_DOWN
#undef SDL_CONTROLLER_BUTTON_DPAD_LEFT
#undef SDL_CONTROLLER_BUTTON_DPAD_RIGHT
#undef SDL_CONTROLLER_BUTTON_DPAD_UP
#undef SDL_CONTROLLER_BUTTON_INVALID
#undef SDL_CONTROLLER_BUTTON_LEFTSHOULDER
#undef SDL_CONTROLLER_BUTTON_LEFTSTICK
#undef SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
#undef SDL_CONTROLLER_BUTTON_RIGHTSTICK
#undef SDL_CONTROLLER_BUTTON_START
#undef SDL_CONTROLLER_BUTTON_X
#undef SDL_CONTROLLER_BUTTON_Y
#undef SDL_CONTROLLERBUTTONDOWN
#undef SDL_CONTROLLERBUTTONUP
#undef SDL_CONTROLLERDEVICEADDED
#undef SDL_CONTROLLERDEVICEREMOVED
#undef SDL_KEYDOWN
#undef SDL_KEYUP
#undef SDL_MOUSEBUTTONDOWN
#undef SDL_MOUSEBUTTONUP
#undef SDL_MOUSEMOTION
#undef SDL_MOUSEWHEEL
#undef SDL_QUIT
#undef SDL_FALSE
#undef SDL_TRUE
#undef SDL_INIT_GAMECONTROLLER
#undef SDL_ScaleModeNearest
#undef SDL_WINDOW_ALLOW_HIGHDPI
#undef AUDIO_S16SYS
#define SDL_GameController SDL_Gamepad
#define SDL_GameControllerButton SDL_GamepadButton
#define SDL_GameControllerClose SDL_CloseGamepad
#define SDL_GameControllerGetAxis SDL_GetGamepadAxis
#define SDL_GameControllerName SDL_GetGamepadName
#define SDL_GameControllerOpen SDL_OpenGamepad
#define SDL_CONTROLLER_AXIS_LEFTX SDL_GAMEPAD_AXIS_LEFTX
#define SDL_CONTROLLER_AXIS_LEFTY SDL_GAMEPAD_AXIS_LEFTY
#define SDL_CONTROLLER_AXIS_RIGHTX SDL_GAMEPAD_AXIS_RIGHTX
#define SDL_CONTROLLER_AXIS_RIGHTY SDL_GAMEPAD_AXIS_RIGHTY
#define SDL_CONTROLLER_AXIS_TRIGGERLEFT SDL_GAMEPAD_AXIS_LEFT_TRIGGER
#define SDL_CONTROLLER_AXIS_TRIGGERRIGHT SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
#define SDL_CONTROLLER_BUTTON_A SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_CONTROLLER_BUTTON_B SDL_GAMEPAD_BUTTON_EAST
#define SDL_CONTROLLER_BUTTON_BACK SDL_GAMEPAD_BUTTON_BACK
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN SDL_GAMEPAD_BUTTON_DPAD_DOWN
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT SDL_GAMEPAD_BUTTON_DPAD_LEFT
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT SDL_GAMEPAD_BUTTON_DPAD_RIGHT
#define SDL_CONTROLLER_BUTTON_DPAD_UP SDL_GAMEPAD_BUTTON_DPAD_UP
#define SDL_CONTROLLER_BUTTON_INVALID SDL_GAMEPAD_BUTTON_INVALID
#define SDL_CONTROLLER_BUTTON_LEFTSHOULDER SDL_GAMEPAD_BUTTON_LEFT_SHOULDER
#define SDL_CONTROLLER_BUTTON_LEFTSTICK SDL_GAMEPAD_BUTTON_LEFT_STICK
#define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER
#define SDL_CONTROLLER_BUTTON_RIGHTSTICK SDL_GAMEPAD_BUTTON_RIGHT_STICK
#define SDL_CONTROLLER_BUTTON_START SDL_GAMEPAD_BUTTON_START
#define SDL_CONTROLLER_BUTTON_X SDL_GAMEPAD_BUTTON_WEST
#define SDL_CONTROLLER_BUTTON_Y SDL_GAMEPAD_BUTTON_NORTH
#define SDL_CONTROLLERBUTTONDOWN SDL_EVENT_GAMEPAD_BUTTON_DOWN
#define SDL_CONTROLLERBUTTONUP SDL_EVENT_GAMEPAD_BUTTON_UP
#define SDL_CONTROLLERDEVICEADDED SDL_EVENT_GAMEPAD_ADDED
#define SDL_CONTROLLERDEVICEREMOVED SDL_EVENT_GAMEPAD_REMOVED
#define SDL_KEYDOWN SDL_EVENT_KEY_DOWN
#define SDL_KEYUP SDL_EVENT_KEY_UP
#define SDL_MOUSEBUTTONDOWN SDL_EVENT_MOUSE_BUTTON_DOWN
#define SDL_MOUSEBUTTONUP SDL_EVENT_MOUSE_BUTTON_UP
#define SDL_MOUSEMOTION SDL_EVENT_MOUSE_MOTION
#define SDL_MOUSEWHEEL SDL_EVENT_MOUSE_WHEEL
#define SDL_QUIT SDL_EVENT_QUIT
#define SDL_FALSE false
#define SDL_TRUE true
#define SDL_INIT_GAMECONTROLLER SDL_INIT_GAMEPAD
#define SDL_ScaleModeNearest SDL_SCALEMODE_NEAREST
#define SDL_WINDOW_ALLOW_HIGHDPI SDL_WINDOW_HIGH_PIXEL_DENSITY
#define AUDIO_S16SYS SDL_AUDIO_S16
#else
#define SDL_EVENT_WINDOW_CLOSE_REQUESTED SDL_WINDOWEVENT_CLOSE
#define SDL_EVENT_WINDOW_FOCUS_GAINED SDL_WINDOWEVENT_FOCUS_GAINED
#define SDL_EVENT_WINDOW_FOCUS_LOST SDL_WINDOWEVENT_FOCUS_LOST
#define SDL_EVENT_WINDOW_MOUSE_LEAVE SDL_WINDOWEVENT_LEAVE
#define SDL_EVENT_WINDOW_RESIZED SDL_WINDOWEVENT_SIZE_CHANGED
#endif

constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;

enum class MphPrimeInputKind : uint8_t {
    None,
    Key,
    Mouse,
};

struct MphPrimeBinding {
    MphPrimeInputKind kind = MphPrimeInputKind::None;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    uint8_t mouse = 0;
};

enum class MphPrimeAction : uint8_t {
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Jump,
    MorphBall,
    BoostZoom,
    ScanVisor,
    UiLeft,
    UiRight,
    UiOk,
    Shoot,
    ScanShoot,
    Beam,
    Missile,
    Weapon1,
    Weapon2,
    Weapon3,
    Weapon4,
    Weapon5,
    Weapon6,
    VirtualStylus,
    Menu,
    Count,
};

struct MphPrimeBindingSet {
    std::array<MphPrimeBinding,
               static_cast<size_t>(MphPrimeAction::Count)> bindings{};
    bool valid = true;
};

struct MphTouchStep {
    uint16_t x = 0;
    uint16_t y = 0;
    bool down = false;
    uint8_t frames = 0;
};

struct MphTouchSequence {
    std::array<MphTouchStep, 6> steps{};
    uint8_t count = 0;
    uint8_t index = 0;
    uint8_t remaining = 0;

    bool active() const { return index < count; }

    void start(std::initializer_list<MphTouchStep> source) {
        count = static_cast<uint8_t>(
            std::min(source.size(), steps.size()));
        std::copy_n(source.begin(), count, steps.begin());
        index = 0;
        remaining = count ? steps[0].frames : 0;
    }

    void tick() {
        if (!active()) return;
        const MphTouchStep& step = steps[index];
        nds_set_touch(step.x, step.y, step.down);
        if (remaining > 0) --remaining;
        if (remaining == 0) {
            ++index;
            if (active()) remaining = steps[index].frames;
        }
    }
};

std::string binding_name_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       if (c == '_' || c == '-') return ' ';
                       return static_cast<char>(std::tolower(c));
                   });
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

SDL_Scancode scancode_from_binding_name(const std::string& value) {
    const std::string normalized = binding_name_lower(value);
    SDL_Scancode key = SDL_GetScancodeFromName(value.c_str());
    if (key != SDL_SCANCODE_UNKNOWN)
        return key;
    if (normalized.size() == 1) {
        const char c = normalized[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<SDL_Scancode>(
                SDL_SCANCODE_A + (c - 'a'));
        }
        if (c >= '1' && c <= '9') {
            return static_cast<SDL_Scancode>(
                SDL_SCANCODE_1 + (c - '1'));
        }
        if (c == '0')
            return SDL_SCANCODE_0;
    }
    if (normalized == "ctrl" || normalized == "control" ||
        normalized == "left ctrl" || normalized == "left control") {
        return SDL_SCANCODE_LCTRL;
    }
    if (normalized == "right ctrl" || normalized == "right control")
        return SDL_SCANCODE_RCTRL;
    if (normalized == "shift" || normalized == "left shift")
        return SDL_SCANCODE_LSHIFT;
    if (normalized == "right shift")
        return SDL_SCANCODE_RSHIFT;
    if (normalized == "space")
        return SDL_SCANCODE_SPACE;
    if (normalized == "grave" || normalized == "backtick" ||
        normalized == "backquote")
        return SDL_SCANCODE_GRAVE;
    if (normalized == "tab")
        return SDL_SCANCODE_TAB;
    if (normalized == "enter" || normalized == "return")
        return SDL_SCANCODE_RETURN;
    if (normalized == "backspace")
        return SDL_SCANCODE_BACKSPACE;
    return SDL_SCANCODE_UNKNOWN;
}

MphPrimeBinding parse_mph_prime_binding(const std::string& value) {
    const std::string normalized = binding_name_lower(value);
    if (normalized.empty() || normalized == "none" ||
        normalized == "unbound") {
        return {};
    }
    if (normalized == "mouse left" || normalized == "left mouse")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_LEFT};
    if (normalized == "mouse right" || normalized == "right mouse")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_RIGHT};
    if (normalized == "mouse middle" || normalized == "middle mouse")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_MIDDLE};
    if (normalized == "mouse 4")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_X1};
    if (normalized == "mouse 5")
        return {MphPrimeInputKind::Mouse, SDL_SCANCODE_UNKNOWN,
                SDL_BUTTON_X2};

    SDL_Scancode key = scancode_from_binding_name(value);
    return {key == SDL_SCANCODE_UNKNOWN ? MphPrimeInputKind::None
                                        : MphPrimeInputKind::Key,
            key, 0};
}

// ── Gamepad bindings for Prime Controls actions ─────────────────────────
enum class MphPadInputKind : uint8_t {
    None,
    Button,
    TriggerLeft,
    TriggerRight,
};

struct MphPadBinding {
    MphPadInputKind kind = MphPadInputKind::None;
    SDL_GameControllerButton button = SDL_CONTROLLER_BUTTON_INVALID;
};

struct MphPadBindingSet {
    std::array<MphPadBinding,
               static_cast<size_t>(MphPrimeAction::Count)> bindings{};
    bool valid = true;
};

MphPadBinding parse_mph_pad_binding(const std::string& value,
                                    bool* recognized) {
    if (recognized) *recognized = true;
    const std::string normalized = binding_name_lower(value);
    if (normalized.empty() || normalized == "none" ||
        normalized == "unbound") {
        return {};
    }
    auto button = [](SDL_GameControllerButton b) {
        return MphPadBinding{MphPadInputKind::Button, b};
    };
    if (normalized == "pad a") return button(SDL_CONTROLLER_BUTTON_A);
    if (normalized == "pad b") return button(SDL_CONTROLLER_BUTTON_B);
    if (normalized == "pad x") return button(SDL_CONTROLLER_BUTTON_X);
    if (normalized == "pad y") return button(SDL_CONTROLLER_BUTTON_Y);
    if (normalized == "pad lb")
        return button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    if (normalized == "pad rb")
        return button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    if (normalized == "pad l3")
        return button(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    if (normalized == "pad r3")
        return button(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (normalized == "pad up") return button(SDL_CONTROLLER_BUTTON_DPAD_UP);
    if (normalized == "pad down")
        return button(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    if (normalized == "pad left")
        return button(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    if (normalized == "pad right")
        return button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (normalized == "pad start")
        return button(SDL_CONTROLLER_BUTTON_START);
    if (normalized == "pad back" || normalized == "pad select")
        return button(SDL_CONTROLLER_BUTTON_BACK);
    if (normalized == "pad lt")
        return {MphPadInputKind::TriggerLeft, SDL_CONTROLLER_BUTTON_INVALID};
    if (normalized == "pad rt")
        return {MphPadInputKind::TriggerRight, SDL_CONTROLLER_BUTTON_INVALID};
    if (recognized) *recognized = false;
    return {};
}

bool pad_binding_matches(const MphPadBinding& binding,
                         MphPadInputKind kind,
                         SDL_GameControllerButton button) {
    if (binding.kind != kind) return false;
    return kind != MphPadInputKind::Button || binding.button == button;
}

MphPadBindingSet make_mph_pad_bindings(
    const NdsMphPrimeControlBindings& source) {
    MphPadBindingSet set{};
    auto put = [&](MphPrimeAction action, const std::string& value) {
        bool recognized = true;
        set.bindings[static_cast<size_t>(action)] =
            parse_mph_pad_binding(value, &recognized);
        if (!recognized) {
            std::fprintf(stderr,
                         "[sdl] invalid MPH Prime Controls pad binding: %s\n",
                         value.c_str());
            set.valid = false;
        }
    };
    put(MphPrimeAction::MoveForward, source.move_forward);
    put(MphPrimeAction::MoveBack, source.move_back);
    put(MphPrimeAction::MoveLeft, source.move_left);
    put(MphPrimeAction::MoveRight, source.move_right);
    put(MphPrimeAction::Jump, source.jump);
    put(MphPrimeAction::MorphBall, source.morph_ball);
    put(MphPrimeAction::BoostZoom, source.boost_zoom);
    put(MphPrimeAction::ScanVisor, source.scan_visor);
    put(MphPrimeAction::UiLeft, source.ui_left);
    put(MphPrimeAction::UiRight, source.ui_right);
    put(MphPrimeAction::UiOk, source.ui_ok);
    put(MphPrimeAction::Shoot, source.shoot);
    put(MphPrimeAction::ScanShoot, source.scan_shoot);
    put(MphPrimeAction::Beam, source.beam);
    put(MphPrimeAction::Missile, source.missile);
    put(MphPrimeAction::Weapon1, source.weapon1);
    put(MphPrimeAction::Weapon2, source.weapon2);
    put(MphPrimeAction::Weapon3, source.weapon3);
    put(MphPrimeAction::Weapon4, source.weapon4);
    put(MphPrimeAction::Weapon5, source.weapon5);
    put(MphPrimeAction::Weapon6, source.weapon6);
    put(MphPrimeAction::VirtualStylus, source.virtual_stylus);
    put(MphPrimeAction::Menu, source.menu);
    return set;
}

bool binding_matches_key(const MphPrimeBinding& binding, SDL_Scancode key) {
    return binding.kind == MphPrimeInputKind::Key && binding.key == key;
}

bool binding_matches_mouse(const MphPrimeBinding& binding, uint8_t button) {
    return binding.kind == MphPrimeInputKind::Mouse &&
           binding.mouse == button;
}

uint16_t mph_prime_hold_mask(MphPrimeAction action) {
    switch (action) {
        case MphPrimeAction::MoveForward: return 1u << 6;  // Up
        case MphPrimeAction::MoveBack:    return 1u << 7;  // Down
        case MphPrimeAction::MoveLeft:    return 1u << 5;  // Left
        case MphPrimeAction::MoveRight:   return 1u << 4;  // Right
        case MphPrimeAction::Jump:        return 1u << 1;  // B
        case MphPrimeAction::BoostZoom:   return 1u << 8;  // R
        case MphPrimeAction::Shoot:
        case MphPrimeAction::ScanShoot:   return 1u << 9;  // L
        case MphPrimeAction::Menu:        return 1u << 3;  // Start
        default:                          return 0;
    }
}

void start_mph_touch_action(MphPrimeAction action,
                            MphTouchSequence& sequence) {
    const MphTouchStep up{0, 0, false, 2};
    auto tap = [&](uint16_t x, uint16_t y, uint8_t touch_frames = 2,
                   uint8_t release_frames = 2) {
        sequence.start({up, {x, y, true, touch_frames},
                        {0, 0, false, release_frames}});
    };
    switch (action) {
        case MphPrimeAction::MorphBall:
            // melonPrimeDS releases late here; boost ball is unreliable if
            // the stylus is restored to aim-center immediately after morph.
            tap(231, 167, 2, 8);
            break;
        case MphPrimeAction::ScanVisor:
            // Hold the visor touch long enough for the transition path that
            // melonPrimeDS handled with a 30-frame loop.
            tap(128, 173, 30, 2);
            break;
        case MphPrimeAction::UiOk:
            tap(128, 142);
            break;
        case MphPrimeAction::UiLeft:
            tap(71, 141);
            break;
        case MphPrimeAction::UiRight:
            tap(185, 141);
            break;
        case MphPrimeAction::Beam:
            if (nds_title_patches_request_mph_weapon(0))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Missile:
            if (nds_title_patches_request_mph_weapon(2))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Weapon1:
            if (nds_title_patches_request_mph_weapon(7))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Weapon2:
            if (nds_title_patches_request_mph_weapon(6))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Weapon3:
            if (nds_title_patches_request_mph_weapon(5))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Weapon4:
            if (nds_title_patches_request_mph_weapon(4))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Weapon5:
            if (nds_title_patches_request_mph_weapon(3))
                sequence.start({up, {128, 96, true, 2}});
            break;
        case MphPrimeAction::Weapon6:
            if (nds_title_patches_request_mph_weapon(1))
                sequence.start({up, {128, 96, true, 2}});
            break;
        default:
            break;
    }
}

MphPrimeBindingSet make_mph_prime_bindings(
    const NdsMphPrimeControlBindings& source) {
    MphPrimeBindingSet set{};
    auto put = [&](MphPrimeAction action, const std::string& value) {
        MphPrimeBinding binding = parse_mph_prime_binding(value);
        if (binding.kind == MphPrimeInputKind::None &&
            binding_name_lower(value) != "none" &&
            binding_name_lower(value) != "unbound") {
            std::fprintf(stderr,
                         "[sdl] invalid MPH Prime Controls binding: %s\n",
                         value.c_str());
            set.valid = false;
        }
        set.bindings[static_cast<size_t>(action)] = binding;
    };
    put(MphPrimeAction::MoveForward, source.move_forward);
    put(MphPrimeAction::MoveBack, source.move_back);
    put(MphPrimeAction::MoveLeft, source.move_left);
    put(MphPrimeAction::MoveRight, source.move_right);
    put(MphPrimeAction::Jump, source.jump);
    put(MphPrimeAction::MorphBall, source.morph_ball);
    put(MphPrimeAction::BoostZoom, source.boost_zoom);
    put(MphPrimeAction::ScanVisor, source.scan_visor);
    put(MphPrimeAction::UiLeft, source.ui_left);
    put(MphPrimeAction::UiRight, source.ui_right);
    put(MphPrimeAction::UiOk, source.ui_ok);
    put(MphPrimeAction::Shoot, source.shoot);
    put(MphPrimeAction::ScanShoot, source.scan_shoot);
    put(MphPrimeAction::Beam, source.beam);
    put(MphPrimeAction::Missile, source.missile);
    put(MphPrimeAction::Weapon1, source.weapon1);
    put(MphPrimeAction::Weapon2, source.weapon2);
    put(MphPrimeAction::Weapon3, source.weapon3);
    put(MphPrimeAction::Weapon4, source.weapon4);
    put(MphPrimeAction::Weapon5, source.weapon5);
    put(MphPrimeAction::Weapon6, source.weapon6);
    put(MphPrimeAction::VirtualStylus, source.virtual_stylus);
    put(MphPrimeAction::Menu, source.menu);
    return set;
}
constexpr int kWindowScale = 2;
constexpr uint64_t kSystemCyclesPerFrame = 2130ull * 263ull;
constexpr int kAudioFrequency = 33513982 / 1024;
constexpr uint32_t kAudioQueueFrames = 2048;
// Playback starts only once kAudioStartFrames (~1.5 s) are queued. The cold
// boot's frames ~5-131 emulate below real time with a measured cumulative
// production deficit of up to ~1.15 s; prebuffering more than that rides the
// whole window out with zero gaps — the stream stays bit-exact, only the
// initial latency is higher. The bounded drain (see drain_audio) then glides
// the queue back down to the ~63 ms steady-state target over a couple of
// seconds without ever freezing video, well before the Health & Safety
// screen needs interactive input.
constexpr uint32_t kAudioStartFrames = 57344;
constexpr uint32_t kAudioFrameBytes = 2u * sizeof(int16_t);
constexpr uint32_t kAudioCapacityFrames = 65536;

struct AudioQueue {
    // Host output only. Consumption, guest mixing and pacing remain active.
    bool output_muted = false;
#if !defined(NDS_HAVE_SDL3)
    // Read by the audio callback; SDL3 applies it as the stream gain instead.
    std::atomic<float> gain{1.0f};
    std::array<int16_t, kAudioCapacityFrames * 2> samples{};
    uint32_t read = 0;
    uint32_t write = 0;
    uint32_t count = 0;
#endif
    std::atomic<uint64_t> underruns{0};
    std::atomic<bool> started{false};
};

#if defined(NDS_HAVE_SDL3)
struct NdsAudioDevice {
    SDL_AudioStream* stream = nullptr;
    explicit operator bool() const { return stream != nullptr; }
};

// SDL3 has no mixer callback that can observe a short read the way the SDL2
// ring-buffer callback below does, but the stream "get" callback carries the
// same fact: additional_amount is how many more bytes the device needs beyond
// what is already queued to satisfy this pull. A nonzero value while playback
// is running is exactly the SDL2 path's `take < requested` condition — the
// device ran dry. Without this the underrun counter would be permanently zero
// under SDL3 and NDS_FRONTEND_REQUIRE_AUDIO would assert nothing.
// Runs on the audio thread; `underruns` is atomic for that reason.
void SDLCALL audio_stream_underrun_callback(void* userdata,
                                            SDL_AudioStream* /*stream*/,
                                            int additional_amount,
                                            int total_amount) {
    if (additional_amount <= 0) return;
    auto* queue = static_cast<AudioQueue*>(userdata);
    if (queue && queue->started.load(std::memory_order_relaxed))
    {
        queue->underruns.fetch_add(1, std::memory_order_relaxed);
        nds_frontend_latency::shortage(additional_amount, total_amount);
    }
}
#else
using NdsAudioDevice = SDL_AudioDeviceID;

void SDLCALL audio_callback(void* userdata, Uint8* stream, int len) {
    // SDL owns this thread, so it cannot be registered at creation; it
    // registers itself the first time it runs instead. One thread_local test
    // per callback, and the whole point is that a mixing stall or an underrun
    // storm becomes attributable to host symbols (host_profile.h).
    static thread_local bool hostprof_registered = false;
    if (!hostprof_registered) {
        hostprof_registered = true;
        nds_hostprof_register_current_thread(NDS_HOSTPROF_ROLE_AUDIO);
    }
    auto* queue = static_cast<AudioQueue*>(userdata);
    std::memset(stream, 0, static_cast<size_t>(len));
    if (!queue || len <= 0) return;
    const uint32_t requested = static_cast<uint32_t>(len) / kAudioFrameBytes;
    const uint32_t take = std::min(requested, queue->count);
    auto* output = reinterpret_cast<int16_t*>(stream);
    const uint32_t first = std::min(take, kAudioCapacityFrames - queue->read);
    std::memcpy(output, queue->samples.data() + queue->read * 2u,
                first * kAudioFrameBytes);
    if (first < take)
        std::memcpy(output + first * 2u, queue->samples.data(),
                    (take - first) * kAudioFrameBytes);
    queue->read = (queue->read + take) % kAudioCapacityFrames;
    queue->count -= take;
    if (queue->output_muted) {
        std::memset(stream, 0, static_cast<size_t>(len));
    } else if (const float gain = queue->gain.load(std::memory_order_relaxed);
               gain < 1.0f) {
        for (uint32_t i = 0; i < take * 2u; ++i)
            output[i] = static_cast<int16_t>(output[i] * gain);
    }
    if (take < requested && queue->started.load(std::memory_order_relaxed)) {
        queue->underruns.fetch_add(1, std::memory_order_relaxed);
        nds_frontend_latency::shortage(
            static_cast<int>((requested - take) * kAudioFrameBytes), len);
    }
}
#endif

bool sdl_push_event(SDL_Event& event) {
#if defined(NDS_HAVE_SDL3)
    return SDL_PushEvent(&event);
#else
    return SDL_PushEvent(&event) == 1;
#endif
}

bool sdl_init_frontend() {
#if defined(NDS_HAVE_SDL3)
    return SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                    SDL_INIT_GAMECONTROLLER);
#else
    return SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                    SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) == 0;
#endif
}

bool sdl_set_thread_priority_high() {
#if defined(NDS_HAVE_SDL3)
    return SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#else
    return SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) == 0;
#endif
}

bool sdl_set_relative_mouse_mode(SDL_Window* window, bool enabled) {
#if defined(NDS_HAVE_SDL3)
    return SDL_SetWindowRelativeMouseMode(window, enabled);
#else
    (void)window;
    return SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0;
#endif
}

SDL_Scancode sdl_event_scancode(const SDL_Event& event) {
#if defined(NDS_HAVE_SDL3)
    return event.key.scancode;
#else
    return event.key.keysym.scancode;
#endif
}

bool sdl_event_alt(const SDL_Event& event) {
#if defined(NDS_HAVE_SDL3)
    return (event.key.mod & SDL_KMOD_ALT) != 0;
#else
    return (event.key.keysym.mod & KMOD_ALT) != 0;
#endif
}

bool sdl_event_shift(const SDL_Event& event) {
#if defined(NDS_HAVE_SDL3)
    return (event.key.mod & SDL_KMOD_SHIFT) != 0;
#else
    return (event.key.keysym.mod & KMOD_SHIFT) != 0;
#endif
}

unsigned savestate_function_key(SDL_Scancode scancode) {
    if (scancode < SDL_SCANCODE_F1 || scancode > SDL_SCANCODE_F12)
        return 0u;
    return static_cast<unsigned>(scancode - SDL_SCANCODE_F1) + 1u;
}

void sdl_set_event_scancode(SDL_Event& event, SDL_Scancode key) {
#if defined(NDS_HAVE_SDL3)
    event.key.scancode = key;
    event.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, true);
    event.key.down = event.type == SDL_KEYDOWN;
#else
    event.key.keysym.scancode = key;
    event.key.keysym.sym = SDL_GetKeyFromScancode(key);
#endif
}

void sdl_set_mouse_button_state(SDL_Event& event, bool down) {
#if defined(NDS_HAVE_SDL3)
    event.button.down = down;
#else
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
#endif
}

bool sdl_window_event_is(const SDL_Event& event, Uint32 window_event) {
#if defined(NDS_HAVE_SDL3)
    return event.type == window_event;
#else
    return event.type == SDL_WINDOWEVENT && event.window.event == window_event;
#endif
}

void sdl_make_window_event(SDL_Event& event, Uint32 window_event,
                           uint32_t window_id) {
#if defined(NDS_HAVE_SDL3)
    event.type = window_event;
#else
    event.type = SDL_WINDOWEVENT;
    event.window.event = static_cast<Uint8>(window_event);
#endif
    event.window.windowID = window_id;
}

SDL_JoystickID sdl_controller_device_id(const SDL_Event& event) {
#if defined(NDS_HAVE_SDL3)
    return event.gdevice.which;
#else
    return event.cdevice.which;
#endif
}

uint8_t sdl_controller_button(const SDL_Event& event) {
#if defined(NDS_HAVE_SDL3)
    return event.gbutton.button;
#else
    return event.cbutton.button;
#endif
}

SDL_JoystickID sdl_controller_id(SDL_GameController* controller) {
#if defined(NDS_HAVE_SDL3)
    return SDL_GetGamepadID(controller);
#else
    return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
#endif
}

uint16_t controller_bit(SDL_GameControllerButton button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:             return 1u << 0;
        case SDL_CONTROLLER_BUTTON_B:             return 1u << 1;
        case SDL_CONTROLLER_BUTTON_BACK:          return 1u << 2;
        case SDL_CONTROLLER_BUTTON_START:         return 1u << 3;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return 1u << 4;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return 1u << 5;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return 1u << 6;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return 1u << 7;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 1u << 8;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return 1u << 9;
        case SDL_CONTROLLER_BUTTON_X:         return 1u << 10;
        case SDL_CONTROLLER_BUTTON_Y:             return 1u << 11;
        default:                                  return 0;
    }
}

uint16_t gamepad_button_mask(const NdsFrontendOptions& options,
                             SDL_GameControllerButton button) {
    if (options.custom_gamepad_bindings)
        return nds_binding_mask(options.gamepad_bindings, button + 1);
    if (options.gamepad_positional)
        return nds_binding_mask(kNdsPositionalPad, button + 1);
    return controller_bit(button);
}

SDL_GameController* open_first_controller() {
#if defined(NDS_HAVE_SDL3)
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (!gamepads) return nullptr;
    SDL_GameController* controller = nullptr;
    for (int index = 0; index < count && !controller; ++index) {
        if (!SDL_IsGamepad(gamepads[index])) continue;
        controller = SDL_GameControllerOpen(gamepads[index]);
    }
    SDL_free(gamepads);
    if (controller) {
        std::fprintf(stderr, "[sdl] Player 1 controller: %s\n",
                     SDL_GameControllerName(controller));
    }
    return controller;
#else
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index)) continue;
        if (SDL_GameController* controller = SDL_GameControllerOpen(index)) {
            std::fprintf(stderr, "[sdl] Player 1 controller: %s\n",
                         SDL_GameControllerName(controller));
            return controller;
        }
    }
    return nullptr;
#endif
}

// The fixed-size layers (a title's UI shrunk by nds_gpu2d_set_fixed_size_layers)
// as the touchscreen last showed them: scale 1 when it showed none. The
// shrunken layer is where the player sees (and points at) the menus, so the
// mouse has to be mapped back through it to reach the DS coordinates the
// guest's UI uses. Written by the presenter, read by the event loop.
std::atomic<float> g_touch_ui_scale{1.0f};
std::atomic<float> g_touch_ui_anchor_x{0.0f};
std::atomic<float> g_touch_ui_anchor_y{0.0f};

// Maps a point on the touchscreen (DS pixels, as displayed) back to the DS
// pixel of the unscaled layer under it. False when the point is outside the
// DS screen after the mapping, which is where the shrunken UI has nothing.
bool touch_ui_unscale(float* x, float* y) {
    const float s = g_touch_ui_scale.load(std::memory_order_relaxed);
    if (s >= 1.0f || s <= 0.0f || nds_title_touch_unscaled()) return true;
    const float ax = g_touch_ui_anchor_x.load(std::memory_order_relaxed);
    const float ay = g_touch_ui_anchor_y.load(std::memory_order_relaxed);
    // Something the title draws at its native place (DQ9's Menu button) is
    // hit where it is shown, unless the shrunken UI is shown there too.
    int rx0, ry0, rx1, ry1;
    if (nds_title_unscaled_touch_rect(&rx0, &ry0, &rx1, &ry1) &&
        *x >= float(rx0) && *x < float(rx1) &&
        *y >= float(ry0) && *y < float(ry1)) {
        const float inside_x0 = ax * (1.0f - s);
        const float inside_y0 = ay * (1.0f - s);
        const bool in_scaled = *x >= inside_x0 &&
            *x < inside_x0 + kScreenWidth * s && *y >= inside_y0 &&
            *y < inside_y0 + kScreenHeight * s;
        if (!in_scaled) return true;
    }
    *x = ax + (*x - ax) / s;
    *y = ay + (*y - ay) / s;
    return *x >= 0.0f && *x < kScreenWidth && *y >= 0.0f &&
           *y < kScreenHeight;
}

void set_touch_from_mouse(float x, float y, bool down,
                          NdsScreenLayout layout, int logical_width) {
    // Coordinates are expected in the renderer's logical DS-space.
    const float bottom_origin =
        layout == NdsScreenLayout::Separate ||
                layout == NdsScreenLayout::Single
            ? 0.0f : kScreenHeight;
    const float left =
        static_cast<float>((logical_width - kScreenWidth) / 2);
    if (!down || x < left || x >= left + kScreenWidth ||
        y < bottom_origin || y >= bottom_origin + kScreenHeight) {
        nds_set_touch(0, 0, false);
        return;
    }
    float ds_x = x - left;
    float ds_y = y - bottom_origin;
    if (!touch_ui_unscale(&ds_x, &ds_y)) {
        nds_set_touch(0, 0, false);
        return;
    }
    const auto touch_x = static_cast<uint16_t>(std::clamp<int>(
        static_cast<int>(ds_x), 0, kScreenWidth - 1));
    const auto touch_y = static_cast<uint16_t>(std::clamp<int>(
        static_cast<int>(ds_y), 0, kScreenHeight - 1));
    nds_set_touch(touch_x, touch_y, true);
}

// The pointer position for nds_title_pointer_hover, from the same logical
// coordinates and touchscreen rectangle as set_touch_from_mouse.
void set_hover_from_mouse(float x, float y, bool hovering,
                          NdsScreenLayout layout, int logical_width) {
    const float bottom_origin =
        layout == NdsScreenLayout::Separate ||
                layout == NdsScreenLayout::Single
            ? 0.0f : kScreenHeight;
    const float left =
        static_cast<float>((logical_width - kScreenWidth) / 2);
    if (!hovering || x < left || x >= left + kScreenWidth ||
        y < bottom_origin || y >= bottom_origin + kScreenHeight) {
        nds_title_set_pointer_hover(0, 0, false);
        return;
    }
    float ds_x = x - left;
    float ds_y = y - bottom_origin;
    if (!touch_ui_unscale(&ds_x, &ds_y)) {
        nds_title_set_pointer_hover(0, 0, false);
        return;
    }
    nds_title_set_pointer_hover(static_cast<int>(ds_x),
                                static_cast<int>(ds_y), true);
}

// The mouse position on the top screen in DS pixels, for
// nds_title_top_pointer / nds_title_top_click. `left` is where the top
// screen's 256 pixels start in the window's logical x; the top screen always
// starts at the window's top. False when the point is outside the screen.
bool top_screen_point(float x, float y, int left, int* ds_x, int* ds_y) {
    const float fx = x - static_cast<float>(left);
    if (fx < 0.0f || fx >= kScreenWidth || y < 0.0f || y >= kScreenHeight)
        return false;
    *ds_x = static_cast<int>(fx);
    *ds_y = static_cast<int>(y);
    return true;
}

uint32_t audio_queue_count(const NdsAudioDevice& device, AudioQueue& queue) {
    if (!device) return 0;
#if defined(NDS_HAVE_SDL3)
    (void)queue;
    const int queued_bytes = SDL_GetAudioStreamQueued(device.stream);
    return queued_bytes > 0
        ? static_cast<uint32_t>(queued_bytes) / kAudioFrameBytes : 0;
#else
    SDL_LockAudioDevice(device);
    const uint32_t count = queue.count;
    SDL_UnlockAudioDevice(device);
    return count;
#endif
}

uint32_t drain_audio(const NdsAudioDevice& device, AudioQueue& queue,
                     bool throttle, uint32_t pace_floor, bool& queue_error,
                     const std::vector<int16_t>* owned_pcm = nullptr) {
    if (!device) return 0;
    std::array<int16_t, 2048> samples{};
    size_t pcm_offset=0;
    for (;;) {
        const uint32_t frames = owned_pcm
            ? static_cast<uint32_t>(std::min<size_t>(1024,(owned_pcm->size()-pcm_offset)/2u))
            : nds_spu_read_output(samples.data(), 1024);
        if (!frames) break;
        if (owned_pcm) {
            std::copy_n(owned_pcm->data()+pcm_offset,frames*2u,samples.data());
            pcm_offset+=frames*2u;
        }
        bool pushed = false;
        while (!pushed) {
#if defined(NDS_HAVE_SDL3)
            const uint32_t queued = audio_queue_count(device, queue);
            if (kAudioCapacityFrames - queued >= frames) {
                if (SDL_PutAudioStreamData(device.stream, samples.data(),
                                           static_cast<int>(
                                               frames * kAudioFrameBytes))) {
                    pushed = true;
                } else {
                    queue_error = true;
                    return audio_queue_count(device, queue);
                }
            }
#else
            SDL_LockAudioDevice(device);
            if (kAudioCapacityFrames - queue.count >= frames) {
                const uint32_t first = std::min(
                    frames, kAudioCapacityFrames - queue.write);
                std::memcpy(queue.samples.data() + queue.write * 2u,
                            samples.data(), first * kAudioFrameBytes);
                if (first < frames)
                    std::memcpy(queue.samples.data(),
                                samples.data() + first * 2u,
                                (frames - first) * kAudioFrameBytes);
                queue.write = (queue.write + frames) % kAudioCapacityFrames;
                queue.count += frames;
                pushed = true;
            }
            SDL_UnlockAudioDevice(device);
#endif
            if (!pushed) {
                if (!throttle) {
                    queue_error = true;
                    return audio_queue_count(device, queue);
                }
                SDL_Delay(1);
            }
        }
    }
    // Audio is the host's real-time clock. Never drop a produced block: if the
    // emulator is faster than the DS cadence, let SDL consume the backlog
    // before emulating another frame. pace_floor is the current allowance:
    // kAudioQueueFrames in steady state, temporarily higher right after the
    // boot prebuffer (the caller decays it a fixed step per frame). The sleep
    // only stops the queue RISING above the floor — it never forces the queue
    // down while the emulator is running behind, so a slow stretch spends the
    // buffered runway instead of having it slept away.
    uint32_t queued = audio_queue_count(device, queue);
    while (throttle && queued > pace_floor) {
        // Spend existing audio pacing slack preparing qualified native code.
        // Guest CPUs remain paused; under load/no slack demand execution wins.
        if (!nds_arm9_native_prepare_idle(1000)) SDL_Delay(1);
        queued = audio_queue_count(device, queue);
    }
    return queued;
}

void clear_audio_queue(const NdsAudioDevice& device, AudioQueue& queue) {
    if (!device) return;
#if defined(NDS_HAVE_SDL3)
    SDL_ClearAudioStream(device.stream);
    (void)queue;
#else
    SDL_LockAudioDevice(device);
    queue.read = 0;
    queue.write = 0;
    queue.count = 0;
    SDL_UnlockAudioDevice(device);
#endif
}

void pause_audio(const NdsAudioDevice& device, bool paused) {
    if (!device) return;
#if defined(NDS_HAVE_SDL3)
    if (paused) SDL_PauseAudioStreamDevice(device.stream);
    else SDL_ResumeAudioStreamDevice(device.stream);
#else
    SDL_PauseAudioDevice(device, paused ? 1 : 0);
#endif
}

void close_audio(NdsAudioDevice& device) {
    if (!device) return;
#if defined(NDS_HAVE_SDL3)
    SDL_DestroyAudioStream(device.stream);
    device.stream = nullptr;
#else
    SDL_CloseAudioDevice(device);
    device = 0;
#endif
}

NdsAudioDevice open_audio_device(AudioQueue& queue,
                                 const SDL_AudioSpec& want) {
#if defined(NDS_HAVE_SDL3)
    NdsAudioDevice device{};
    device.stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
    if (device.stream && queue.output_muted &&
        !SDL_SetAudioStreamGain(device.stream, 0.0f)) {
        // An unattended real-device test must never fall back to audible
        // output if the requested per-stream mute cannot be applied.
        std::fprintf(stderr, "[sdl] cannot mute audio output: %s\n", SDL_GetError());
        close_audio(device);
    }
    if (device.stream &&
        !SDL_SetAudioStreamGetCallback(
            device.stream, audio_stream_underrun_callback, &queue)) {
        // Refuse a device that cannot report underruns rather than silently
        // shipping a frontend whose audio gate asserts nothing.
        std::fprintf(stderr,
            "[sdl] audio underrun callback unavailable: %s\n", SDL_GetError());
        close_audio(device);
    }
    return device;
#else
    SDL_AudioSpec got{};
    NdsAudioDevice device = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (device && (got.freq != want.freq || got.format != want.format ||
                   got.channels != want.channels)) {
        std::fprintf(stderr,
            "[sdl] refusing mismatched audio format: want=%d/%u/%u "
            "got=%d/%u/%u\n",
            want.freq, want.format, want.channels,
            got.freq, got.format, got.channels);
        close_audio(device);
    }
    return device;
#endif
}

// Sets the host output level. Guest mixing, queue consumption and audio pacing
// carry on untouched, so muting never changes timing. NDS_FRONTEND_AUDIO_MUTE
// (output_muted) always wins.
void apply_audio_output_level(const NdsAudioDevice& device, AudioQueue& queue,
                              uint8_t volume, bool muted) {
    if (!device) return;
    const float gain = muted || queue.output_muted
        ? 0.0f : static_cast<float>(volume) / 100.0f;
#if defined(NDS_HAVE_SDL3)
    if (!SDL_SetAudioStreamGain(device.stream, gain))
        std::fprintf(stderr, "[sdl] cannot set audio volume: %s\n",
                     SDL_GetError());
#else
    queue.gain.store(gain, std::memory_order_relaxed);
#endif
}

void discard_spu_output() {
    std::array<int16_t, 2048> samples{};
    while (nds_spu_read_output(samples.data(), 1024) != 0) {}
}

uint64_t environment_u64(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end == value ? 0 : static_cast<uint64_t>(parsed);
}

uint64_t framebuffer_rgb_fnv(int screen) {
    const uint32_t* framebuffer = nds_gpu2d_framebuffer(screen);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < 256u * 192u; ++i) {
        const uint32_t pixel = framebuffer[i];
        const uint8_t rgb[3] = {
            static_cast<uint8_t>(pixel >> 16),
            static_cast<uint8_t>(pixel >> 8),
            static_cast<uint8_t>(pixel),
        };
        for (const uint8_t byte : rgb) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

constexpr const char* kRuntimeMouseSensitivityKey =
    "input.mouse_sensitivity";
constexpr const char* kRuntimeGamepadLayoutKey = "input.gamepad_layout";
constexpr const char* kRuntimeScalingKey = "display.scaling";
constexpr const char* kRuntimeResolutionScaleKey = "display.resolution_scale";
// Menu steps for resolution_scale; 0 is "off" (the fixed multiplier). TOML
// and the CLI take any percentage, which the menu shows as the nearest step.
constexpr int kResolutionScaleChoiceValues[] = {0, 25, 50, 75, 100, 150, 200};
constexpr const char* kRuntimeSecondScreenKey = "display.second_screen_size";
constexpr const char* kRuntimeShowFpsKey = "display.show_fps";
constexpr const char* kRuntimeSmooth3dKey = "display.smooth_3d";
constexpr const char* kRuntimeVolumeKey = "audio.volume";
constexpr const char* kRuntimeMuteKey = "audio.mute";
// Rows for the title's own settings (title_settings.h): "title.<key>".
constexpr const char* kRuntimeTitlePrefix = "title.";
constexpr const char* kRuntimeResumeKey = "system.resume";
constexpr const char* kRuntimeQuitKey = "system.quit";

struct RuntimeMenuState {
    NdsFrontendOptions* options = nullptr;
    bool* running = nullptr;
    bool close_requested = false;
    // Resolution scale needs the GL presenter on the compute renderer.
    bool resolution_scale_available = false;
    const NdsAudioDevice* audio = nullptr;
    AudioQueue* audio_queue = nullptr;
    RecompRuntimeUi* ui = nullptr;  // for confirm prompts and status text
};

// The title setting behind a "title.<key>" row, or -1.
int runtime_menu_title_setting(const RecompRuntimeUiItem* item) {
    const size_t prefix = std::strlen(kRuntimeTitlePrefix);
    if (std::strncmp(item->key, kRuntimeTitlePrefix, prefix) != 0) return -1;
    return nds_title_setting_find(item->key + prefix);
}

// Rewrites (or appends) each key=value pair in the launcher-owned settings
// file, keeping every other line as it was.
bool save_runtime_settings(
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& values) {
    if (path.empty()) return false;
    std::filesystem::path settings(path);
    std::error_code error;
    if (!settings.parent_path().empty()) {
        std::filesystem::create_directories(settings.parent_path(), error);
        if (error) return false;
    }

    std::vector<std::string> lines;
    std::vector<bool> found(values.size(), false);
    bool saw_version = false;
    {
        std::ifstream in(settings);
        std::string line;
        while (std::getline(in, line)) {
            const size_t equals = line.find('=');
            if (equals != std::string::npos) {
                const std::string key = line.substr(0, equals);
                if (key == "settings_version") saw_version = true;
                for (size_t i = 0; i < values.size(); ++i) {
                    if (key != values[i].first) continue;
                    line = values[i].first + "=" + values[i].second;
                    found[i] = true;
                }
            }
            lines.push_back(line);
        }
    }
    if (!saw_version)
        lines.insert(lines.begin(), "settings_version=12");
    for (size_t i = 0; i < values.size(); ++i) {
        if (!found[i])
            lines.push_back(values[i].first + "=" + values[i].second);
    }

    std::ofstream out(settings, std::ios::trunc);
    if (!out) return false;
    for (const std::string& line : lines)
        out << line << '\n';
    return static_cast<bool>(out);
}

int runtime_menu_get_value(void* context, const RecompRuntimeUiItem* item,
                           int* value_out) {
    auto* state = static_cast<RuntimeMenuState*>(context);
    if (!state || !state->options || !item || !value_out) return 0;
#if NDS_ONLINE
    if (nds_netplay_menu_owns(item))
        return nds_netplay_menu_get_value(item, value_out);
#endif
    if (const int title = runtime_menu_title_setting(item); title >= 0) {
        *value_out = nds_title_setting_value(title);
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeMouseSensitivityKey) == 0) {
        *value_out = state->options->relative_mouse_sensitivity;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeGamepadLayoutKey) == 0) {
        *value_out = state->options->gamepad_positional ? 1 : 0;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeResolutionScaleKey) == 0) {
        const int current = state->options->resolution_scale;
        int best = 0;
        if (current > 0) {
            best = kResolutionScaleChoiceValues[1];
            for (const int v : kResolutionScaleChoiceValues) {
                if (v > 0 && std::abs(v - current) < std::abs(best - current))
                    best = v;
            }
        }
        *value_out = best;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeScalingKey) == 0) {
        *value_out = static_cast<int>(state->options->scaling);
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeSecondScreenKey) == 0) {
        *value_out = state->options->second_screen_size;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeShowFpsKey) == 0) {
        *value_out = state->options->show_fps ? 1 : 0;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeSmooth3dKey) == 0) {
        *value_out = state->options->smooth_3d ? 1 : 0;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeVolumeKey) == 0) {
        *value_out = state->options->audio_volume;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeMuteKey) == 0) {
        *value_out = state->options->audio_muted ? 1 : 0;
        return 1;
    }
    return 0;
}

int runtime_menu_set_value(void* context, const RecompRuntimeUiItem* item,
                           int value) {
    auto* state = static_cast<RuntimeMenuState*>(context);
    if (!state || !state->options || !item) return 0;
#if NDS_ONLINE
    if (nds_netplay_menu_owns(item))
        return nds_netplay_menu_set_value(item, value);
#endif
    if (const int title = runtime_menu_title_setting(item); title >= 0) {
        if (!nds_title_setting_set(title, value)) return 0;
        std::fprintf(stderr, "[title] %s -> %d\n",
                     nds_title_setting_at(title)->key, value);
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeMouseSensitivityKey) == 0) {
        if (value < 10 || value > 400) return 0;
        state->options->relative_mouse_sensitivity =
            static_cast<uint16_t>(value);
        std::fprintf(stderr, "[sdl] mouse sensitivity set to %d%%\n", value);
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeGamepadLayoutKey) == 0) {
        if (value < 0 || value > 1) return 0;
        state->options->gamepad_positional = value != 0;
        std::fprintf(stderr, "[sdl] gamepad layout -> %s\n",
                     value ? "positional" : "labels");
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeResolutionScaleKey) == 0) {
        if (value < 0 || value > 1000) return 0;
        state->options->resolution_scale = static_cast<uint16_t>(value);
        if (value == 0)
            std::fprintf(stderr,
                         "[display] resolution scale -> off (fixed %ux)\n",
                         static_cast<unsigned>(
                             state->options->internal_resolution));
        else
            std::fprintf(stderr, "[display] resolution scale -> %d%%\n",
                         value);
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeScalingKey) == 0) {
        if (value < 0 || value > static_cast<int>(NdsScalingFilter::Nearest))
            return 0;
        state->options->scaling = static_cast<NdsScalingFilter>(value);
        std::fprintf(stderr, "[display] scaling -> %s\n",
                     nds_scaling_filter_name(state->options->scaling));
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeSecondScreenKey) == 0) {
        if (value < 0 || value > 60) return 0;
        state->options->second_screen_size = static_cast<uint8_t>(value);
        std::fprintf(stderr, "[display] second screen size -> %d%%\n", value);
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeShowFpsKey) == 0) {
        if (value < 0 || value > 1) return 0;
        state->options->show_fps = value != 0;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeSmooth3dKey) == 0) {
        if (value < 0 || value > 1) return 0;
        state->options->smooth_3d = value != 0;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeVolumeKey) == 0 ||
        std::strcmp(item->key, kRuntimeMuteKey) == 0) {
        if (!state->audio || !state->audio_queue) return 0;
        if (std::strcmp(item->key, kRuntimeVolumeKey) == 0) {
            if (value < 0 || value > 100) return 0;
            state->options->audio_volume = static_cast<uint8_t>(value);
        } else {
            state->options->audio_muted = value != 0;
        }
        apply_audio_output_level(*state->audio, *state->audio_queue,
                                 state->options->audio_volume,
                                 state->options->audio_muted);
        return 1;
    }
    return 0;
}

int runtime_menu_run_action(void* context, const RecompRuntimeUiItem* item) {
    auto* state = static_cast<RuntimeMenuState*>(context);
    if (!state || !item) return 0;
#if NDS_ONLINE
    if (nds_netplay_menu_owns(item))
        return nds_netplay_menu_run_action(state->ui, item);
#endif
    if (std::strcmp(item->key, kRuntimeResumeKey) == 0) {
        state->close_requested = true;
        return 1;
    }
    if (std::strcmp(item->key, kRuntimeQuitKey) == 0) {
        if (state->running) *state->running = false;
        return 1;
    }
    return 0;
}

int runtime_menu_is_enabled(void* context, const RecompRuntimeUiItem* item) {
    auto* state = static_cast<RuntimeMenuState*>(context);
    if (!state || !state->options || !item) return 0;
#if NDS_ONLINE
    if (nds_netplay_menu_owns(item)) return nds_netplay_menu_is_enabled(item);
#endif
    if (std::strcmp(item->key, kRuntimeMouseSensitivityKey) == 0)
        return state->options->relative_mouse_touch ? 1 : 0;
    if (std::strcmp(item->key, kRuntimeGamepadLayoutKey) == 0)
        return state->options->custom_gamepad_bindings ? 0 : 1;
    if (std::strcmp(item->key, kRuntimeResolutionScaleKey) == 0)
        return state->resolution_scale_available ? 1 : 0;
    if (std::strcmp(item->key, kRuntimeSecondScreenKey) == 0)
        return state->options->screen_layout == NdsScreenLayout::Single
            ? 1 : 0;
    if (std::strcmp(item->key, kRuntimeVolumeKey) == 0)
        return state->audio && *state->audio &&
               !state->options->audio_muted ? 1 : 0;
    if (std::strcmp(item->key, kRuntimeMuteKey) == 0)
        return state->audio && *state->audio ? 1 : 0;
    return 1;
}

void runtime_menu_save(void* context) {
    auto* state = static_cast<RuntimeMenuState*>(context);
    if (!state || !state->options) return;
    const NdsFrontendOptions& options = *state->options;
    if (save_runtime_settings(
            options.runtime_settings_path,
            {{"mouse_sensitivity",
              std::to_string(options.relative_mouse_sensitivity)},
             {"gamepad_layout",
              options.gamepad_positional ? "positional" : "labels"},
             {"scaling", nds_scaling_filter_name(options.scaling)},
             {"resolution_scale",
              std::to_string(options.resolution_scale)},
             {"second_screen_size",
              std::to_string(options.second_screen_size)},
             {"audio_volume", std::to_string(options.audio_volume)},
             {"audio_muted", options.audio_muted ? "1" : "0"}})) {
        std::fprintf(stderr, "[sdl] saved runtime settings\n");
    }
    if (options.settings_path.empty()) return;
    const auto quoted = [](const char* name) {
        return nds_toml_string(name);
    };
    std::vector<NdsSettingUpdate> updates = {
        {"display", "scaling", quoted(nds_scaling_filter_name(options.scaling))},
        {"display", "resolution_scale",
         std::to_string(options.resolution_scale)},
        {"display", "second_screen_size",
         std::to_string(options.second_screen_size)},
        {"display", "show_fps", options.show_fps ? "true" : "false"},
        {"display", "smooth_3d", options.smooth_3d ? "true" : "false"},
        {"audio", "volume", std::to_string(options.audio_volume)},
        {"audio", "mute", options.audio_muted ? "true" : "false"},
        {"controls", "gamepad_layout",
         quoted(options.gamepad_positional ? "positional" : "labels")},
    };
    // The two are exclusive in TOML unless the scale is off.
    if (options.resolution_scale != 0)
        updates.push_back({"display", "internal_resolution", "", true});
    for (size_t i = 0; i < nds_title_setting_count(); ++i) {
        const int id = static_cast<int>(i);
        updates.push_back({"title", nds_title_setting_at(id)->key,
                           std::to_string(nds_title_setting_value(id))});
    }
    if (options.relative_mouse_touch)
        updates.push_back({"controls", "mouse_sensitivity",
                           std::to_string(options.relative_mouse_sensitivity)});
#if NDS_ONLINE
    bool netplay = false;
    std::string lobby_name;
    if (nds_netplay_menu_settings(&netplay, &lobby_name)) {
        updates.push_back({"online", "enabled", netplay ? "true" : "false"});
        updates.push_back({"online", "lobby_name", quoted(lobby_name.c_str())});
    }
#endif
    std::string error;
    if (!nds_update_settings_file(options.settings_path, updates, &error))
        std::fprintf(stderr, "[settings] could not save %s: %s\n",
                     options.settings_path.c_str(), error.c_str());
}

// Writes the window's current logical size to [display] window_width/height.
// Fullscreen and minimized sizes are not worth coming back to, and the logical
// unit (not pixels) is what SDL_SetWindowSize reads, so a scaled display does
// not grow the window on every launch. `saved` caches what the file holds.
void persist_window_size(NdsFrontendOptions& options, SDL_Window* window) {
    if (options.settings_path.empty() || !window) return;
    const auto flags = SDL_GetWindowFlags(window);
#if defined(NDS_HAVE_SDL3)
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED)) return;
#else
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP |
                 SDL_WINDOW_MINIMIZED))
        return;
#endif
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width < 160 || height < 160 || width > 8192 || height > 8192) return;
    if (width == options.window_width && height == options.window_height)
        return;
    options.window_width = width;
    options.window_height = height;
    std::string error;
    if (!nds_update_settings_file(
            options.settings_path,
            {{"display", "window_width", std::to_string(width)},
             {"display", "window_height", std::to_string(height)}},
            &error))
        std::fprintf(stderr, "[settings] could not save %s: %s\n",
                     options.settings_path.c_str(), error.c_str());
}

// Writes [display] fullscreen so the next launch starts in the mode the window
// was last left in.
void persist_fullscreen(const NdsFrontendOptions& options) {
    if (options.settings_path.empty()) return;
    std::string error;
    if (!nds_update_settings_file(
            options.settings_path,
            {{"display", "fullscreen",
              nds_toml_string(nds_fullscreen_mode_name(options.fullscreen))}},
            &error))
        std::fprintf(stderr, "[settings] could not save %s: %s\n",
                     options.settings_path.c_str(), error.c_str());
}

// The menu's rows for title hooks (title_menu.h): publishes a copy with the
// current values, then applies what the hooks asked for since last frame.
void runtime_menu_serve_title(RuntimeMenuState* state,
                              const std::vector<RecompRuntimeUiItem>& items) {
    bool changed = false;
    for (const NdsTitleMenuRequest& request : nds_title_menu_take_requests()) {
        for (const RecompRuntimeUiItem& item : items) {
            if (request.key != item.key) continue;
            if (!runtime_menu_is_enabled(state, &item)) break;
            if (request.run) {
                if (item.type == RECOMP_RUNTIME_UI_ACTION)
                    changed |= runtime_menu_run_action(state, &item) != 0;
            } else if (item.type == RECOMP_RUNTIME_UI_BOOL ||
                       item.type == RECOMP_RUNTIME_UI_INT ||
                       item.type == RECOMP_RUNTIME_UI_CHOICE) {
                changed |=
                    runtime_menu_set_value(state, &item, request.value) != 0;
            }
            break;
        }
    }
    if (changed) runtime_menu_save(state);

    std::vector<NdsTitleMenuRow> rows;
    rows.reserve(items.size());
    for (const RecompRuntimeUiItem& item : items) {
        NdsTitleMenuRow row;
        row.key = item.key ? item.key : "";
        row.section = item.section ? item.section : "";
        row.label = item.label ? item.label : "";
        row.description = item.description ? item.description : "";
        switch (item.type) {
        case RECOMP_RUNTIME_UI_BOOL:
            row.type = NdsTitleMenuRowType::Bool; break;
        case RECOMP_RUNTIME_UI_INT:
            row.type = NdsTitleMenuRowType::Int; break;
        case RECOMP_RUNTIME_UI_CHOICE:
            row.type = NdsTitleMenuRowType::Choice; break;
        case RECOMP_RUNTIME_UI_TEXT:
            row.type = NdsTitleMenuRowType::Text; break;
        default:
            row.type = NdsTitleMenuRowType::Action; break;
        }
        row.minimum = item.minimum;
        row.maximum = item.maximum;
        row.step = item.step > 0 ? item.step : 1;
        for (size_t i = 0; item.choices && i < item.choice_count; ++i) {
            row.choices.emplace_back(item.choices[i] ? item.choices[i] : "");
            row.choice_values.push_back(
                item.choice_values ? item.choice_values[i]
                                   : item.minimum + static_cast<int>(i));
        }
        runtime_menu_get_value(state, &item, &row.value);
        row.enabled = runtime_menu_is_enabled(state, &item) != 0;
        rows.push_back(std::move(row));
    }
    nds_title_menu_publish(std::move(rows));
}

bool runtime_menu_key_input(SDL_Scancode scancode,
                            RecompRuntimeUiInput* input) {
    if (!input) return false;
    switch (scancode) {
        case SDL_SCANCODE_ESCAPE:
            *input = RECOMP_RUNTIME_UI_INPUT_TOGGLE;
            return true;
        case SDL_SCANCODE_BACKSPACE:
            *input = RECOMP_RUNTIME_UI_INPUT_BACK;
            return true;
        case SDL_SCANCODE_UP:
            *input = RECOMP_RUNTIME_UI_INPUT_UP;
            return true;
        case SDL_SCANCODE_DOWN:
            *input = RECOMP_RUNTIME_UI_INPUT_DOWN;
            return true;
        case SDL_SCANCODE_LEFT:
            *input = RECOMP_RUNTIME_UI_INPUT_LEFT;
            return true;
        case SDL_SCANCODE_RIGHT:
            *input = RECOMP_RUNTIME_UI_INPUT_RIGHT;
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            *input = RECOMP_RUNTIME_UI_INPUT_ACCEPT;
            return true;
        default:
            return false;
    }
}

bool runtime_menu_controller_input(SDL_GameControllerButton button,
                                   RecompRuntimeUiInput* input) {
    if (!input) return false;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_START:
            *input = RECOMP_RUNTIME_UI_INPUT_TOGGLE;
            return true;
        case SDL_CONTROLLER_BUTTON_BACK:
        case SDL_CONTROLLER_BUTTON_B:
            *input = RECOMP_RUNTIME_UI_INPUT_BACK;
            return true;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            *input = RECOMP_RUNTIME_UI_INPUT_UP;
            return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            *input = RECOMP_RUNTIME_UI_INPUT_DOWN;
            return true;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            *input = RECOMP_RUNTIME_UI_INPUT_LEFT;
            return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            *input = RECOMP_RUNTIME_UI_INPUT_RIGHT;
            return true;
        case SDL_CONTROLLER_BUTTON_A:
            *input = RECOMP_RUNTIME_UI_INPUT_ACCEPT;
            return true;
        default:
            return false;
    }
}

const uint32_t* runtime_menu_overlay(RecompRuntimeUi* ui,
                                     const uint32_t* pixels,
                                     int width,
                                     int height) {
    // A toast shows over the game with the menu closed too.
    if (!ui ||
        (!recomp_runtime_ui_is_open(ui) && !recomp_runtime_ui_toast_visible(ui)) ||
        !pixels || width <= 0 || height <= 0) {
        return pixels;
    }
    static std::vector<uint32_t> surface;
    const size_t count = static_cast<size_t>(width) *
                         static_cast<size_t>(height);
    surface.assign(pixels, pixels + count);
    recomp_runtime_ui_render_argb8888(
        ui, surface.data(), width, height, width * sizeof(uint32_t));
    return surface.data();
}

struct FrontendPresentation {
    bool separate = false;
    // Single-screen layout: one window (windows[1] aliases windows[0]) that
    // shows the physical LCD `shown_screen`. With gl_top it is the GL window.
    bool single = false;
    int shown_screen = 0;
    // The other LCD's size beside it, percent; 0 hides it.
    int inset_percent = 0;
    // A game mode forces both screens stacked at the same size (single
    // layout only): inset_percent is 100 and the top LCD is the main one.
    bool stacked = false;
    bool gl_top = false;
    SDL_Window* windows[2]{};
    SDL_Renderer* renderers[2]{};
    SDL_Texture* textures[2]{};
    SDL_Texture* sample_targets[2]{};
    uint32_t window_ids[2]{};
    int screen_widths[2]{kScreenWidth, kScreenWidth};
    int canvas_width = kScreenWidth;
    int configured_sample_scale = 1;
    int sample_scale = 1;
    uint8_t antialiasing = 0;
    // Live copies of the [display] geometry options; the runtime menu edits
    // the options and the frame loop syncs them here before each present.
    NdsWindowMode window_mode = NdsWindowMode::Letterbox;
    NdsScalingFilter scaling = NdsScalingFilter::Integer;
    // Live [display] resolution_scale / internal_resolution, for hosts that
    // size the 3D raster from the presented rect (Plume).
    uint16_t resolution_scale = 0;
    uint8_t internal_resolution = 1;
};

// The top window's host presenter composes the 3D layer itself (direct OBJ and
// HD frames). Only then are those surfaces worth emitting; otherwise the CPU
// composite carries the 3D layer, as on the SDL path.
bool presenter_composes_3d(const FrontendPresentation& presentation) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    return presentation.gl_top && nds_compute_host_composes_3d();
#else
    (void)presentation;
    return false;
#endif
}

// The window flag the accelerated top-window presenter needs: a GL context,
// a Vulkan surface for Plume on Linux, or a Metal view for Plume on Apple
// (MoltenVK draws into its CAMetalLayer). Plume on Windows presents through
// D3D12 on the plain HWND, and a Vulkan-flagged window would make SDL require
// a Vulkan loader there for nothing.
uint64_t accelerated_window_flag() {
#if defined(NDS_USE_PLUME) && defined(__linux__)
    return static_cast<uint64_t>(SDL_WINDOW_VULKAN);
#elif defined(NDS_USE_PLUME) && defined(__APPLE__)
    return static_cast<uint64_t>(SDL_WINDOW_METAL);
#elif defined(NDS_USE_PLUME)
    return 0u;
#else
    return static_cast<uint64_t>(SDL_WINDOW_OPENGL);
#endif
}

// docs/frame_interpolation.md, MVP blend mode. Holds the previous DS frame's
// post-compositor ARGB pixels per screen plus the scratch the blend is built
// into. These are copies taken after composition, so nothing the guest can
// observe is involved and no scheduler work is attached to them.
struct FrameBlendCache {
    std::vector<uint32_t> previous[2];
    std::vector<uint32_t> blended[2];
    int widths[2]{0, 0};
    bool valid = false;
};

// 50/50 per-channel average, the alpha 0.5 midpoint the 120 Hz target wants.
// The low bit of each channel is dropped rather than rounded (invisible at 8
// bits, one pass of cheap word arithmetic over ~100k pixels), and the alpha
// byte is carried straight from the current frame so a blended upload is
// byte-compatible with the real uploads either side of it.
void blend_half(const uint32_t* previous, const uint32_t* current,
                uint32_t* out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        out[i] = (current[i] & 0xFF000000u) |
                 (((previous[i] >> 1) & 0x007F7F7Fu) +
                  ((current[i] >> 1) & 0x007F7F7Fu));
    }
}

void cache_presented_frame(FrameBlendCache& cache, int screen,
                           const uint32_t* pixels, int width) {
    const size_t count = static_cast<size_t>(width) * kScreenHeight;
    if (cache.widths[screen] != width) {
        cache.widths[screen] = width;
        cache.previous[screen].assign(count, 0u);
        cache.blended[screen].assign(count, 0u);
    }
    std::memcpy(cache.previous[screen].data(), pixels,
                count * sizeof(uint32_t));
}

#if !defined(NDS_HAVE_SDL3)
uint32_t fullscreen_flags(NdsFullscreenMode mode) {
    switch (mode) {
        case NdsFullscreenMode::Borderless:
            return SDL_WINDOW_FULLSCREEN_DESKTOP;
        case NdsFullscreenMode::Exclusive:
            return static_cast<uint32_t>(SDL_WINDOW_FULLSCREEN);
        default:
            return 0;
    }
}
#endif

void destroy_presentation(FrontendPresentation& presentation) {
    for (SDL_Texture*& texture : presentation.sample_targets) {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    for (SDL_Texture*& texture : presentation.textures) {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (presentation.separate && presentation.renderers[1])
        SDL_DestroyRenderer(presentation.renderers[1]);
    if (presentation.renderers[0])
        SDL_DestroyRenderer(presentation.renderers[0]);
    presentation.renderers[0] = nullptr;
    presentation.renderers[1] = nullptr;
    if (presentation.separate && presentation.windows[1])
        SDL_DestroyWindow(presentation.windows[1]);
    if (presentation.windows[0])
        SDL_DestroyWindow(presentation.windows[0]);
    presentation.windows[0] = nullptr;
    presentation.windows[1] = nullptr;
}

SDL_Renderer* create_renderer(SDL_Window* window) {
#if defined(NDS_HAVE_SDL3)
    // Driver order is NOT left to SDL3's default here. SDL3 orders its Windows
    // render drivers direct3d11 first; SDL2 ordered direct3d (D3D9) first, and
    // every release through v0.5.2 was tuned against that. Measured on this
    // workstation over 600 presented frames of the direct-boot soak:
    //
    //   SDL2 (D3D9)      swap 0.147 s   0.25 ms/frame
    //   SDL3 direct3d    swap 0.175 s   0.29 ms/frame
    //   SDL3 direct3d11  swap 2.219 s   3.70 ms/frame   <- SDL3's default
    //   SDL3 direct3d12  swap 2.639 s   4.40 ms/frame
    //
    // The cost is not vsync (SDL_RENDER_VSYNC=0 and =1 measure the same); it is
    // intrinsic to the D3D11 present path. This frontend's pacing clock is the
    // audio queue, so a multi-millisecond present is taken straight out of the
    // frame budget and surfaces as audio underruns -- SDL3-on-D3D11 underran a
    // 2,400-frame soak that SDL2 and SDL3-on-D3D9 both complete clean.
    //
    // So: ask for the driver SDL2 would have picked, and fall through to SDL3's
    // own choice wherever it does not exist (Linux, macOS), then to software.
    // NDS_SDL_RENDER_DRIVER forces a specific driver for diagnosis.
    SDL_Renderer* renderer = nullptr;
    if (const char* forced = std::getenv("NDS_SDL_RENDER_DRIVER")) {
        if (forced[0] != '\0') {
            renderer = SDL_CreateRenderer(window, forced);
            if (!renderer)
                std::fprintf(stderr,
                    "[sdl] render driver '%s' unavailable: %s\n",
                    forced, SDL_GetError());
        }
    }
    if (!renderer) renderer = SDL_CreateRenderer(window, "direct3d");
    if (!renderer) renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, "software");
    if (renderer) {
        const char* name = SDL_GetRendererName(renderer);
        std::fprintf(stderr, "[sdl] render driver: %s\n",
                     name ? name : "(unknown)");
    }
#else
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
#endif
    return renderer;
}

// The cartridge icon for every window we open. SDL applies it per backend:
// the Win32 class icon, X11 _NET_WM_ICON, and Wayland's xdg-toplevel-icon
// where the compositor offers it (others take the icon from the .desktop
// file matching the app id above).
const std::vector<uint8_t>* g_window_icon = nullptr;

void apply_window_icon(SDL_Window* window) {
    if (!window || !g_window_icon || g_window_icon->size() != 256 * 256 * 4)
        return;
    void* pixels = const_cast<uint8_t*>(g_window_icon->data());
#if defined(NDS_HAVE_SDL3)
    SDL_Surface* icon = SDL_CreateSurfaceFrom(
        256, 256, SDL_PIXELFORMAT_RGBA32, pixels, 256 * 4);
#else
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, 256, 256, 32, 256 * 4, SDL_PIXELFORMAT_RGBA32);
#endif
    if (!icon)
        return;
    SDL_SetWindowIcon(window, icon);
#if defined(NDS_HAVE_SDL3)
    SDL_DestroySurface(icon);
#else
    SDL_FreeSurface(icon);
#endif
}

SDL_Window* create_window(const char* title, int x, int y, int w, int h,
                          uint64_t flags) {
    // Unattended frontend/GPU checks must not show or activate desktop windows.
    if (const char* hidden = std::getenv("NDS_FRONTEND_HIDDEN");
        hidden && std::strcmp(hidden, "1") == 0)
        flags |= SDL_WINDOW_HIDDEN;
#if defined(NDS_HAVE_SDL3)
    SDL_Window* window = SDL_CreateWindow(
        title, w, h, static_cast<SDL_WindowFlags>(flags));
    if (window)
        SDL_SetWindowPosition(window, x, y);
#else
    SDL_Window* window =
        SDL_CreateWindow(title, x, y, w, h, static_cast<uint32_t>(flags));
#endif
    apply_window_icon(window);
    return window;
}

bool set_window_fullscreen(SDL_Window* window, NdsFullscreenMode mode) {
#if defined(NDS_HAVE_SDL3)
    return SDL_SetWindowFullscreen(window, mode != NdsFullscreenMode::Off);
#else
    return SDL_SetWindowFullscreen(window, fullscreen_flags(mode)) == 0;
#endif
}

// Presentation works in drawable (physical) pixels: no SDL logical
// presentation, because SDL has no mode for a centred, unscaled 1:1 image.
// Every rect comes from nds_present_rect(), which the GL presenter and the
// mouse-to-touch inversion share.
void window_drawable_size(const FrontendPresentation& presentation,
                          int screen, int* width, int* height) {
    *width = *height = 0;
    SDL_Window* window = presentation.windows[screen];
    if (!window) return;
#if defined(NDS_HAVE_SDL3)
    SDL_GetWindowSizeInPixels(window, width, height);
#else
    if (presentation.renderers[screen])
        SDL_GetRendererOutputSize(presentation.renderers[screen], width,
                                  height);
    else
        SDL_GL_GetDrawableSize(window, width, height);
#endif
}

// The DS-space content a window shows: one screen in the separate layout,
// both screens stacked on one canvas otherwise.
void window_content_size(const FrontendPresentation& presentation,
                         int screen, int* width, int* height) {
    if (presentation.separate) {
        *width = presentation.screen_widths[screen];
        *height = kScreenHeight;
    } else if (presentation.single) {
        *width = presentation.screen_widths[presentation.shown_screen & 1];
        *height = kScreenHeight;
    } else {
        *width = presentation.canvas_width;
        *height = kScreenHeight * 2;
    }
}

NdsPresentRect window_present_rect(const FrontendPresentation& presentation,
                                   int screen, int drawable_w,
                                   int drawable_h) {
    int content_w = 0, content_h = 0;
    window_content_size(presentation, screen, &content_w, &content_h);
    if (presentation.single)
        return nds_single_layout(presentation.window_mode,
                                 presentation.scaling, drawable_w, drawable_h,
                                 content_w, content_h,
                                 presentation.inset_percent,
                                 presentation.stacked).main;
    return nds_present_rect(presentation.window_mode, presentation.scaling,
                            drawable_w, drawable_h, content_w, content_h);
}

// How tall window 0 shows the main DS screen, in window points
// (nds_title_display_height). 0 when it cannot tell, e.g. minimized.
int presented_screen_points(const FrontendPresentation& presentation) {
    if (!presentation.windows[0]) return 0;
    int drawable_w = 0, drawable_h = 0;
    window_drawable_size(presentation, 0, &drawable_w, &drawable_h);
    int content_w = 0, content_h = 0;
    window_content_size(presentation, 0, &content_w, &content_h);
    if (drawable_w <= 0 || drawable_h <= 0 || content_h <= 0) return 0;
    const NdsPresentRect rect =
        window_present_rect(presentation, 0, drawable_w, drawable_h);
    double density = 1.0;
#if defined(NDS_HAVE_SDL3)
    const float window_density =
        SDL_GetWindowPixelDensity(presentation.windows[0]);
    if (window_density > 0) density = window_density;
#endif
    return static_cast<int>(std::lround(
        static_cast<double>(rect.h) * kScreenHeight / content_h / density));
}

// The 3D multiplier the options ask for: with resolution_scale, that percent
// of the GL window's presented rect (window_mode/scaling from the options, so
// a menu change counts at once); otherwise the fixed internal_resolution.
uint8_t requested_internal_scale_for(const FrontendPresentation& presentation,
                                     const NdsFrontendOptions& options,
                                     NdsPresentRect* rect_out = nullptr) {
    if (options.resolution_scale == 0u || !presentation.windows[0])
        return options.internal_resolution;
    int drawable_w = 0, drawable_h = 0;
    window_drawable_size(presentation, 0, &drawable_w, &drawable_h);
    int content_w = 0, content_h = 0;
    window_content_size(presentation, 0, &content_w, &content_h);
    const NdsPresentRect rect = nds_single_layout(
        options.window_mode, options.scaling, drawable_w, drawable_h,
        content_w, content_h,
        presentation.single
            ? (presentation.stacked ? 100 : options.second_screen_size)
            : 0,
        presentation.stacked).main;
    if (rect_out) *rect_out = rect;
    return nds_auto_internal_scale(rect.h, content_h, options.scaling,
                                   options.resolution_scale,
                                   kNdsMaxInternalScale);
}

// A DS-space sub-rect of a window's content, mapped into drawable pixels.
SDL_Rect content_sub_rect(const NdsPresentRect& rect, int content_w,
                          int content_h, int x, int y, int w, int h) {
    auto map_x = [&](int v) {
        return rect.x + static_cast<int>(
            static_cast<long long>(v) * rect.w / content_w);
    };
    auto map_y = [&](int v) {
        return rect.y + static_cast<int>(
            static_cast<long long>(v) * rect.h / content_h);
    };
    const int x0 = map_x(x), y0 = map_y(y);
    return SDL_Rect{x0, y0, map_x(x + w) - x0, map_y(y + h) - y0};
}

// Current refresh rate of the display the window sits on, in whole Hz, or 0
// when SDL cannot answer (SDL_GetError() then carries the reason). The frame
// interpolation gate is the only consumer: it refuses to insert a synthetic
// present unless the panel is comfortably above 60 Hz.
int window_refresh_hz(SDL_Window* window) {
#if defined(NDS_HAVE_SDL3)
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) return 0;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
    if (!mode) return 0;
    // SDL3 reports refresh_rate as a float (e.g. 164.998); round to the
    // nearest whole Hz so the >= comparison against the integer gate behaves
    // the same as SDL2's integer report.
    return static_cast<int>(std::lround(mode->refresh_rate));
#else
    const int display_index = SDL_GetWindowDisplayIndex(window);
    if (display_index < 0) return 0;
    SDL_DisplayMode mode{};
    if (SDL_GetCurrentDisplayMode(display_index, &mode) != 0) return 0;
    return mode.refresh_rate;
#endif
}

// Mouse positions arrive in window coordinates. Everything downstream (touch,
// the stacked-layout routing, the self-tests) works in the window's logical
// DS-space content coordinates, so convert here through the same rect the
// presenter draws into: window -> drawable pixels -> content. Clicks on the
// bars land outside [0, content) and are therefore never touches. Relative
// motion (xrel/yrel) is left in window units.
// Where the last converted mouse event lies on the top screen's content (the
// window's logical x and y), when it does. With the top LCD shown and the
// touchscreen inset over it, the event itself is converted for the inset
// (bottom LCD coordinates), so the top position is kept here.
struct TopLogicalHit {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
};
TopLogicalHit g_top_logical_hit;

void convert_mouse_event_to_logical_coordinates(
    SDL_Event& event, const FrontendPresentation& presentation) {
    auto screen_for_window = [&](uint32_t window_id) -> int {
        for (int screen = 0; screen < 2; ++screen) {
            if (window_id == presentation.window_ids[screen] &&
                presentation.windows[screen])
                return screen;
        }
        return -1;
    };
    auto convert = [&](uint32_t window_id, auto& x, auto& y) {
        const int screen = screen_for_window(window_id);
        if (screen < 0) return;
        int window_w = 0, window_h = 0;
        SDL_GetWindowSize(presentation.windows[screen], &window_w, &window_h);
        int drawable_w = 0, drawable_h = 0;
        window_drawable_size(presentation, screen, &drawable_w, &drawable_h);
        if (window_w <= 0 || window_h <= 0 || drawable_w <= 0 ||
            drawable_h <= 0)
            return;
        const float px = static_cast<float>(x) * drawable_w / window_w;
        const float py = static_cast<float>(y) * drawable_h / window_h;
        int content_w = 0, content_h = 0;
        window_content_size(presentation, screen, &content_w, &content_h);
        NdsPresentRect rect = window_present_rect(
            presentation, screen, drawable_w, drawable_h);
        // With the top LCD shown, the touchscreen is the inset. Either way
        // the result is bottom-LCD coordinates.
        if (presentation.single && presentation.shown_screen == 0 &&
            presentation.inset_percent > 0) {
            rect = nds_single_layout(presentation.window_mode,
                                     presentation.scaling, drawable_w,
                                     drawable_h, content_w, content_h,
                                     presentation.inset_percent,
                                     presentation.stacked).inset;
        }
        float cx = 0.0f, cy = 0.0f;
        nds_present_rect_to_content(rect, content_w, content_h, px, py,
                                    &cx, &cy);
        g_top_logical_hit = {};
        if (presentation.single && presentation.shown_screen == 0 &&
            presentation.inset_percent > 0) {
            // The main rect is the top LCD; the inset covers part of it.
            const NdsPresentRect main_rect = window_present_rect(
                presentation, screen, drawable_w, drawable_h);
            float tx = 0.0f, ty = 0.0f;
            nds_present_rect_to_content(main_rect, content_w, content_h, px,
                                        py, &tx, &ty);
            const bool in_inset = cx >= 0.0f && cx < content_w &&
                                  cy >= 0.0f && cy < content_h;
            g_top_logical_hit = {!in_inset, tx, ty};
        } else if (presentation.single && presentation.shown_screen == 1 &&
                   presentation.inset_percent > 0) {
            // The touchscreen is the main rect; the top LCD is the inset,
            // over it. A point in the inset is the top screen's, never a
            // touch.
            const NdsPresentRect inset_rect = nds_single_layout(
                presentation.window_mode, presentation.scaling, drawable_w,
                drawable_h, content_w, content_h,
                presentation.inset_percent, presentation.stacked).inset;
            float tx = 0.0f, ty = 0.0f;
            nds_present_rect_to_content(inset_rect, content_w, content_h, px,
                                        py, &tx, &ty);
            const bool in_inset = tx >= 0.0f && tx < content_w &&
                                  ty >= 0.0f && ty < content_h;
            g_top_logical_hit = {in_inset, tx, ty};
            if (in_inset) {
                cx = -1.0f;
                cy = -1.0f;
            }
        } else {
            g_top_logical_hit = {true, cx, cy};
        }
        using Coordinate = std::remove_reference_t<decltype(x)>;
        x = static_cast<Coordinate>(std::floor(cx));
        y = static_cast<Coordinate>(std::floor(cy));
    };
    if (event.type == SDL_MOUSEBUTTONDOWN ||
        event.type == SDL_MOUSEBUTTONUP) {
        convert(event.button.windowID, event.button.x, event.button.y);
    } else if (event.type == SDL_MOUSEMOTION) {
        convert(event.motion.windowID, event.motion.x, event.motion.y);
    }
}

bool set_texture_scale_nearest(SDL_Texture* texture) {
#if defined(NDS_HAVE_SDL3)
    return SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
#else
    return SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest) == 0;
#endif
}

bool set_texture_scale_linear(SDL_Texture* texture, bool linear) {
    if (!linear) return set_texture_scale_nearest(texture);
#if defined(NDS_HAVE_SDL3)
    return SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
#else
    return SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear) == 0;
#endif
}

void render_texture(SDL_Renderer* renderer, SDL_Texture* texture,
                    const SDL_Rect* destination) {
#if defined(NDS_HAVE_SDL3)
    SDL_FRect dst{};
    const SDL_FRect* dst_ptr = nullptr;
    if (destination) {
        dst.x = static_cast<float>(destination->x);
        dst.y = static_cast<float>(destination->y);
        dst.w = static_cast<float>(destination->w);
        dst.h = static_cast<float>(destination->h);
        dst_ptr = &dst;
    }
    SDL_RenderTexture(renderer, texture, nullptr, dst_ptr);
#else
    SDL_RenderCopy(renderer, texture, nullptr, destination);
#endif
}

void fill_rect(SDL_Renderer* renderer, const SDL_Rect& rect) {
#if defined(NDS_HAVE_SDL3)
    const SDL_FRect frect{static_cast<float>(rect.x),
                          static_cast<float>(rect.y),
                          static_cast<float>(rect.w),
                          static_cast<float>(rect.h)};
    SDL_RenderFillRect(renderer, &frect);
#else
    SDL_RenderFillRect(renderer, &rect);
#endif
}

const char* notice_glyph(char ch) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
        case 'A': return "01110100011000111111100011000110001";
        case 'B': return "11110100011000111110100011000111110";
        case 'C': return "01111100001000010000100001000001111";
        case 'D': return "11110100011000110001100011000111110";
        case 'E': return "11111100001000011110100001000011111";
        case 'F': return "11111100001000011110100001000010000";
        case 'G': return "01111100001000010111100011000101111";
        case 'H': return "10001100011000111111100011000110001";
        case 'I': return "11111001000010000100001000010011111";
        case 'J': return "00111000100001000010100101001001100";
        case 'K': return "10001100101010011000101001001010001";
        case 'L': return "10000100001000010000100001000011111";
        case 'M': return "10001110111010110101100011000110001";
        case 'N': return "10001110011010110011100011000110001";
        case 'O': return "01110100011000110001100011000101110";
        case 'P': return "11110100011000111110100001000010000";
        case 'Q': return "01110100011000110001101011001001101";
        case 'R': return "11110100011000111110101001001010001";
        case 'S': return "01111100001000001110000010000111110";
        case 'T': return "11111001000010000100001000010000100";
        case 'U': return "10001100011000110001100011000101110";
        case 'V': return "10001100011000110001100010101000100";
        case 'W': return "10001100011000110101101011101110001";
        case 'X': return "10001100010101000100010101000110001";
        case 'Y': return "10001100010101000100001000010000100";
        case 'Z': return "11111000010001000100010001000011111";
        case '0': return "01110100011001110101110011000101110";
        case '1': return "00100011000010000100001000010001110";
        case '2': return "01110100010000100010001000100011111";
        case '3': return "11110000010000101110000010000111110";
        case '4': return "00010001100101010010111110001000010";
        case '5': return "11111100001000011110000010000111110";
        case '6': return "01110100001000011110100011000101110";
        case '7': return "11111000010001000100010000100001000";
        case '8': return "01110100011000101110100011000101110";
        case '9': return "01110100011000101111000010000101110";
        case ':': return "00000001000010000000001000010000000";
        case '.': return "00000000000000000000000000010000100";
        case '-': return "00000000000000011111000000000000000";
        case '/': return "00001000100001000100010001000010000";
        case '(': return "00010001000100001000010000010000010";
        case ')': return "01000001000001000010000100010001000";
        case '_': return "00000000000000000000000000000011111";
        case ' ': return "00000000000000000000000000000000000";
        default:  return "01110000010001000100001000000000100";
    }
}

// Overlays are authored in DS-space pixels of one screen and drawn scaled
// into that screen's rect, so they keep their size relative to the image in
// every window mode. The target is either an SDL renderer (rect in drawable
// pixels) or a CPU ARGB overlay buffer for the GL presenter (rect in buffer
// pixels, alpha 0 = transparent).
struct DsCanvas {
    SDL_Rect dest;
    int width;   // DS-space width the dest rect represents
    int height;  // DS-space height the dest rect represents
};

struct DsTarget {
    SDL_Renderer* renderer = nullptr;
    uint32_t* pixels = nullptr;
    int pixels_width = 0;
    int pixels_height = 0;
    DsCanvas canvas{};
    uint32_t color = 0xFFFFFFFFu;

    void set_color(uint8_t r, uint8_t g, uint8_t b) {
        if (renderer) SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        color = 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    }
    void fill(int x, int y, int w, int h) const {
        if (canvas.width <= 0 || canvas.height <= 0) return;
        auto map_x = [&](int v) {
            return canvas.dest.x + static_cast<int>(std::floor(
                static_cast<double>(v) * canvas.dest.w / canvas.width));
        };
        auto map_y = [&](int v) {
            return canvas.dest.y + static_cast<int>(std::floor(
                static_cast<double>(v) * canvas.dest.h / canvas.height));
        };
        const int x0 = map_x(x), y0 = map_y(y);
        const SDL_Rect rect{x0, y0, std::max(1, map_x(x + w) - x0),
                            std::max(1, map_y(y + h) - y0)};
        if (renderer) {
            fill_rect(renderer, rect);
            return;
        }
        if (!pixels) return;
        const int left = std::max(rect.x, 0);
        const int top = std::max(rect.y, 0);
        const int right = std::min(rect.x + rect.w, pixels_width);
        const int bottom = std::min(rect.y + rect.h, pixels_height);
        // Wholly off the buffer: left > right would make std::fill below
        // write backwards through memory.
        if (left >= right || top >= bottom) return;
        for (int py = top; py < bottom; ++py)
            std::fill(pixels + size_t(py) * pixels_width + left,
                      pixels + size_t(py) * pixels_width + right, color);
    }
};

void draw_notice_glyph(const DsTarget& target, int x, int y, char ch) {
    const char* glyph = notice_glyph(ch);
    for (int gy = 0; gy < 7; ++gy) {
        for (int gx = 0; gx < 5; ++gx) {
            if (glyph[gy * 5 + gx] == '1')
                target.fill(x + gx, y + gy, 1, 1);
        }
    }
}

void draw_savestate_notice(DsTarget target, const char* text) {
    if ((!target.renderer && !target.pixels) || !text || !*text) return;
    constexpr int kColumns = 40;
    constexpr int kRows = 5;
    constexpr int kGlyphWidth = 6;
    constexpr int kGlyphHeight = 8;
    const SDL_Rect background{5,
                              target.canvas.height - kRows * kGlyphHeight - 5,
                              kColumns * kGlyphWidth + 6,
                              kRows * kGlyphHeight + 4};
    target.set_color(10, 10, 10);
    target.fill(background.x, background.y, background.w, background.h);
    target.set_color(255, 255, 255);
    int row = 0;
    int column = 0;
    for (const char* p = text; *p && row < kRows; ++p) {
        if (*p == '\n' || column == kColumns) {
            ++row;
            column = 0;
            if (*p == '\n') continue;
            if (row >= kRows) break;
        }
        draw_notice_glyph(target, background.x + 3 + column * kGlyphWidth,
                          background.y + 2 + row * kGlyphHeight, *p);
        ++column;
    }
}

void draw_fps_counter(DsTarget target, const char* text) {
    if ((!target.renderer && !target.pixels) || !text || !*text) return;
    constexpr int kGlyphWidth = 6;
    const int columns = static_cast<int>(std::strlen(text));
    target.set_color(10, 10, 10);
    target.fill(2, 2, columns * kGlyphWidth + 3, 11);
    target.set_color(255, 255, 255);
    for (int column = 0; column < columns; ++column)
        draw_notice_glyph(target, 4 + column * kGlyphWidth, 4, text[column]);
}

// The stylus lives in the 256x192 touch space spread over the screen's rect.
void draw_virtual_stylus(DsTarget target, float stylus_x, float stylus_y) {
    target.canvas.width = 256;
    target.canvas.height = 192;
    const int x = static_cast<int>(std::lround(stylus_x));
    const int y = static_cast<int>(std::lround(stylus_y));
    target.set_color(255, 255, 255);
    target.fill(x - 5, y, 4, 1);
    target.fill(x + 2, y, 4, 1);
    target.fill(x, y - 5, 1, 4);
    target.fill(x, y + 2, 1, 4);
}

DsTarget sdl_ds_target(SDL_Renderer* renderer, const SDL_Rect& dest,
                       int ds_width) {
    DsTarget target{};
    target.renderer = renderer;
    target.canvas = {dest, ds_width, kScreenHeight};
    return target;
}

bool create_presentation(const NdsFrontendOptions& options,
                         FrontendPresentation& presentation,
                         bool allow_gl_top = true) {
    presentation.separate =
        options.screen_layout == NdsScreenLayout::Separate;
    presentation.single = options.screen_layout == NdsScreenLayout::Single;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    const char* direct_selection =
        std::getenv("NDS_COMPUTE_DIRECT_PRESENT");
    const bool direct_enabled =
        !direct_selection || !*direct_selection ||
        std::strcmp(direct_selection, "1") == 0;
    // resolution_scale asks for the GL presenter up front, whatever the
    // startup multiplier: auto scale can move between 1x and HD at runtime,
    // and the presenter runs at 1x too, so a change never recreates windows.
    const bool direct_top_requested =
        (options.adaptive_screens & NDS_ADAPTIVE_TOP) != 0u ||
        options.internal_resolution > 1u || options.resolution_scale > 0u;
    presentation.gl_top = (presentation.separate || presentation.single) &&
        allow_gl_top &&
        direct_top_requested &&
        nds_gpu3d_renderer_prefers_compute() &&
        direct_enabled;
#endif
    presentation.antialiasing = options.antialiasing;
    presentation.window_mode = options.window_mode;
    presentation.scaling = options.scaling;
    presentation.resolution_scale = options.resolution_scale;
    presentation.internal_resolution = options.internal_resolution;
    presentation.inset_percent =
        presentation.single ? options.second_screen_size : 0;
    const int aa_scale = presentation.gl_top ? 1 : options.antialiasing >= 8 ? 4 :
                         options.antialiasing >= 4 ? 3 :
                         options.antialiasing >= 2 ? 2 : 1;
    presentation.configured_sample_scale =
        std::max<int>(options.supersampling, aa_scale);
    presentation.sample_scale = presentation.configured_sample_scale;
    for (int screen = 0; screen < 2; ++screen) {
        const uint8_t bit = static_cast<uint8_t>(1u << screen);
        if ((options.adaptive_screens & bit) &&
            (options.adaptive_supported & bit)) {
            presentation.screen_widths[screen] =
                options.adaptive_max_width[screen];
        }
    }
    presentation.canvas_width = std::max(
        presentation.screen_widths[0],
        presentation.screen_widths[1]);
    const int first_height = presentation.separate || presentation.single
        ? kScreenHeight * kWindowScale
        : kScreenHeight * 2 * kWindowScale;
    const uint64_t top_window_flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
        (presentation.gl_top ? accelerated_window_flag() : 0u);
    const std::string single_title =
        options.game_name.empty() ? "ndsrecomp" : options.game_name;
    presentation.windows[0] = create_window(
        presentation.separate ? "ndsrecomp - Top Screen"
        : presentation.single ? single_title.c_str()
                              : "ndsrecomp firmware preview",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        (presentation.separate ? presentation.screen_widths[0]
                               : presentation.canvas_width) * kWindowScale *
            (100 + presentation.inset_percent) / 100,
        first_height,
        top_window_flags);
    if (!presentation.windows[0]) {
        std::fprintf(stderr, "[sdl] window failed: %s\n", SDL_GetError());
        return false;
    }
    NdsInitialWindowSize initial_size{
        presentation.screen_widths[0] * kWindowScale,
        presentation.screen_widths[1] * kWindowScale, first_height};
    if (options.window_scale) {
        SDL_Rect usable{};
        float coordinate_scale = 1.0f;
#if defined(NDS_HAVE_SDL3)
        const auto display = SDL_GetDisplayForWindow(presentation.windows[0]);
        SDL_GetDisplayUsableBounds(display, &usable);
        const float density = SDL_GetWindowPixelDensity(presentation.windows[0]);
        if (density > 0)
            coordinate_scale = SDL_GetWindowDisplayScale(presentation.windows[0]) / density;
#else
        const int display = SDL_GetWindowDisplayIndex(presentation.windows[0]);
        SDL_GetDisplayUsableBounds(display, &usable);
#endif
        initial_size = nds_initial_window_size(
            presentation.screen_widths[0], presentation.screen_widths[1],
            presentation.separate, options.window_scale, coordinate_scale,
            usable.w, usable.h, presentation.single);
        SDL_SetWindowSize(presentation.windows[0],
            presentation.separate ? initial_size.top_width
                : std::max(initial_size.top_width, initial_size.bottom_width) *
                      (100 + presentation.inset_percent) / 100,
            initial_size.height);
        SDL_SetWindowPosition(presentation.windows[0],
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (presentation.separate && usable.w > 96) {
            const int pair_width = initial_size.top_width + initial_size.bottom_width + 32;
            SDL_SetWindowPosition(presentation.windows[0],
                usable.x + (usable.w - pair_width) / 2,
                usable.y + (usable.h - initial_size.height) / 2);
        }
        std::fprintf(stderr,
            "[sdl] initial window scale=%u dpi=%.3f top=%dx%d bottom=%dx%d\n",
            options.window_scale, coordinate_scale, initial_size.top_width,
            initial_size.height, initial_size.bottom_width, initial_size.height);
    }
    // The size the user last left the window at wins over the scale-derived
    // one. The two-window layout has no single size to restore.
    if (!presentation.separate && options.window_width >= 160 &&
        options.window_height >= 160) {
        SDL_SetWindowSize(presentation.windows[0], options.window_width,
                          options.window_height);
        SDL_SetWindowPosition(presentation.windows[0],
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        std::fprintf(stderr, "[sdl] restored window size %dx%d\n",
                     options.window_width, options.window_height);
    }
    if (!presentation.gl_top) {
        presentation.renderers[0] = create_renderer(presentation.windows[0]);
        if (!presentation.renderers[0]) {
            std::fprintf(stderr, "[sdl] renderer failed: %s\n", SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
    }

    if (presentation.separate) {
        int top_x = 0;
        int top_y = 0;
        SDL_GetWindowPosition(presentation.windows[0], &top_x, &top_y);
        presentation.windows[1] = create_window(
            "ndsrecomp - Bottom Screen",
            top_x + initial_size.top_width + 32,
            top_y,
            initial_size.bottom_width,
            initial_size.height,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!presentation.windows[1]) {
            std::fprintf(stderr, "[sdl] bottom window failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
        presentation.renderers[1] =
            create_renderer(presentation.windows[1]);
        if (!presentation.renderers[1]) {
            std::fprintf(stderr, "[sdl] bottom renderer failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
    } else {
        presentation.windows[1] = presentation.windows[0];
        presentation.renderers[1] = presentation.renderers[0];
    }

    // Fullscreen is applied only after both separate-layout windows have their
    // final placement. The primary combined/top window is deliberately the
    // sole target so the separate touch window remains usable.
    if (!set_window_fullscreen(presentation.windows[0], options.fullscreen)) {
        std::fprintf(stderr, "[sdl] fullscreen (%s) failed: %s\n",
                     nds_fullscreen_mode_name(options.fullscreen),
                     SDL_GetError());
        destroy_presentation(presentation);
        return false;
    }

    for (int screen = 0; screen < 2; ++screen) {
        // The single GL window presents both LCDs through the GL presenter.
        if ((screen == 0 || presentation.single) && presentation.gl_top) {
            presentation.window_ids[screen] =
                SDL_GetWindowID(presentation.windows[screen]);
            continue;
        }
        presentation.textures[screen] = SDL_CreateTexture(
            presentation.renderers[screen], SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            presentation.screen_widths[screen], kScreenHeight);
        if (!presentation.textures[screen]) {
            std::fprintf(stderr, "[sdl] texture failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
        if (!set_texture_scale_nearest(presentation.textures[screen])) {
            std::fprintf(stderr, "[sdl] texture scale mode failed: %s\n",
                         SDL_GetError());
            destroy_presentation(presentation);
            return false;
        }
        if (presentation.sample_scale > 1) {
            presentation.sample_targets[screen] = SDL_CreateTexture(
                presentation.renderers[screen], SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET,
                presentation.screen_widths[screen] *
                    presentation.sample_scale,
                kScreenHeight * presentation.sample_scale);
            if (!presentation.sample_targets[screen]) {
                std::fprintf(stderr,
                             "[sdl] supersample target failed: %s\n",
                             SDL_GetError());
                destroy_presentation(presentation);
                return false;
            }
            if (!set_texture_scale_nearest(
                    presentation.sample_targets[screen])) {
                std::fprintf(
                    stderr,
                    "[sdl] supersample target scale mode failed: %s\n",
                    SDL_GetError());
                destroy_presentation(presentation);
                return false;
            }
        }
        presentation.window_ids[screen] =
            SDL_GetWindowID(presentation.windows[screen]);
    }
    return true;
}

void render_screen(FrontendPresentation& presentation, int screen,
                   const SDL_Rect& destination) {
    SDL_Renderer* renderer = presentation.renderers[screen];
    SDL_Texture* source = presentation.textures[screen];
    // The supersample target is an exact multiple of the native texture, so
    // that hop stays nearest; only the final draw into `destination` follows
    // the scaling filter.
    const bool linear = presentation.scaling == NdsScalingFilter::Bilinear;
    if (presentation.sample_scale > 1 && presentation.sample_targets[screen]) {
        set_texture_scale_nearest(source);
        SDL_SetRenderTarget(renderer, presentation.sample_targets[screen]);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        render_texture(renderer, source, nullptr);
        SDL_SetRenderTarget(renderer, nullptr);
        source = presentation.sample_targets[screen];
    }
    set_texture_scale_linear(source, linear);
    render_texture(renderer, source, &destination);
}

struct PresentationTicks {
    uint64_t upload = 0;
    uint64_t draw = 0;
    uint64_t swap = 0;
    uint64_t pool_wait = 0, gl_setup = 0, gl_error = 0, overlay = 0;
    bool ok = true;
};

// All-or-nothing. The only fallible step is the renderer rebuild, so it runs
// FIRST and nothing else is touched until it has succeeded: a failed apply
// leaves the readback latency, the sample scale and the HD emit flag exactly
// as the currently installed stage left them, so the caller can report the
// stage that is actually running.
bool apply_performance_governor_stage(
        FrontendPresentation& presentation,
        uint8_t stage, uint8_t requested_scale) {
    // The governor caps the requested multiplier (fixed or auto); it never
    // raises it.
    const uint8_t target_scale = stage >= 2u ? 1u : requested_scale;
    if (!nds_gpu3d_set_runtime_internal_scale(target_scale)) {
        std::fprintf(stderr,
                     "[governor] could not apply internal scale %u\n",
                     static_cast<unsigned>(target_scale));
        return false;
    }
    nds_gpu3d_set_display_readback_latency(stage >= 1u);
    presentation.sample_scale =
        stage >= 2u ? 1 : presentation.configured_sample_scale;
    nds_gpu2d_set_hd_emit(target_scale > 1u &&
                          presenter_composes_3d(presentation));
    return true;
}

#if defined(NDS_HAVE_COMPUTE_RENDERER)
// Two native-sized slots. Only the producer writes the building slot; main
// presents the prior completed slot, then joins before consuming/reusing either
// slot or touching live simulation. No CPU images grow with internal scale.
struct OwnedPresentationFrame {
    std::vector<uint32_t> pixels[2], native_top, hd_top, hd_below, objects, raster;
    std::vector<int16_t> pcm;
    NdsComputePresentationFrame gl{};
    uint16_t widths[2]{256,256};
    uint64_t system_timestamp{}, audio_epoch{}, digest_epoch{}, job{}, adaptive_ticks{};
    uint64_t emu_begin{}, emu_end{}, cycles_begin{}, cycles_end{};
    bool direct{}, valid{}, powered_off{};

    void Capture(const NdsFrontendOptions& options, bool gl_top) {
        valid=false; gl={}; pcm.clear();
        powered_off=nds_powered_off();
        if (powered_off || nds_gpu3d_compute_runtime_failed()) return;
        const auto* native=nds_gpu2d_framebuffer(0);
        native_top.assign(native,native+256u*192u);
        const uint32_t* top=native;
        const uint32_t* bottom=nds_gpu2d_framebuffer(1);
        widths[0]=widths[1]=256;
        nds_gpu2d_invalidate_hd_frame();
        direct=nds_gpu2d_direct_present_frame_active();
        const auto start=SDL_GetPerformanceCounter();
        const bool native_hd=gl_top && nds_gpu3d_internal_scale()>1u &&
                             nds_gpu3d_output_width()==256u;
        if (((options.adaptive_screens & NDS_ADAPTIVE_TOP) || native_hd) && !direct)
            top=nds_gpu2d_adaptive_framebuffer(0,&widths[0]);
        else if (direct) widths[0]=nds_gpu3d_output_width();
        if (options.adaptive_screens & NDS_ADAPTIVE_BOTTOM)
            bottom=nds_gpu2d_adaptive_framebuffer(1,&widths[1]);
        adaptive_ticks=SDL_GetPerformanceCounter()-start;
        pixels[0].assign(top,top+(direct ? 256u : widths[0])*192u);
        pixels[1].assign(bottom,bottom+widths[1]*192u);
        gl.has_direct=nds_gpu2d_direct_frame(&gl.direct);
        if (gl.has_direct) {
            objects.assign(gl.direct.object_pixels,
                           gl.direct.object_pixels+gl.direct.width*192u);
            gl.direct.object_pixels=objects.data();
            if (gl.direct.raster_params) {
                raster.assign(gl.direct.raster_params,gl.direct.raster_params+192u*4u);
                gl.direct.raster_params=raster.data();
            }
        }
        gl.has_hd=nds_gpu2d_hd_frame_peek(&gl.hd);
        if (gl.has_hd) {
            const size_t count=size_t(gl.hd.width)*192u*2u;
            hd_top.assign(gl.hd.top_pixels,gl.hd.top_pixels+count);
            hd_below.assign(gl.hd.below_pixels,gl.hd.below_pixels+count);
            gl.hd.top_pixels=hd_top.data(); gl.hd.below_pixels=hd_below.data();
        }
        gl.native_texture=nds_gpu3d_compute_output_texture();
        gl.hires_texture=nds_gpu3d_compute_output_texture_hires();
        gl.scale=nds_gpu3d_internal_scale();
        std::array<int16_t,2048> samples{};
        for (uint32_t frames; (frames=nds_spu_read_output(samples.data(),1024));) {
            if (pcm.size()+frames*2u>65536u)
                throw std::runtime_error("CPU frame PCM exceeds bounded publication");
            pcm.insert(pcm.end(),samples.begin(),samples.begin()+frames*2u);
        }
        system_timestamp=scheduler_system_timestamp();
        audio_epoch=nds_spu_presentation_epoch();
        valid=true;
    }
};
#endif

#if defined(NDS_HAVE_COMPUTE_RENDERER)
// Hands the presenter this frame's fixed-size layers, if gpu2d split them:
// the layer at its scale about its native anchor, and the image without it.
// Returns whether it did; the caller clears it after the present.
bool set_fixed_layers(int width) {
    NdsGpu2dFixedLayersFrame fixed{};
    if (width != 256 || !nds_gpu2d_fixed_layers_frame(&fixed)) return false;
    const float s = fixed.scale;
    nds_compute_host_set_layer(fixed.layer, fixed.base, 256u,
                               float(fixed.anchor_x) / 256.0f * (1.0f - s),
                               float(fixed.anchor_y) / 192.0f * (1.0f - s),
                               s, s);
    return true;
}
#endif

// One separate-layout window: clear, draw its screen into the presentation
// rect, then the overlays that belong to it. Does not present.
void draw_separate_screen(FrontendPresentation& presentation, int screen,
                          bool virtual_stylus_visible, float virtual_stylus_x,
                          float virtual_stylus_y,
                          const char* savestate_notice,
                          const char* fps_counter) {
    SDL_Renderer* renderer = presentation.renderers[screen];
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);
    int drawable_w = 0, drawable_h = 0;
    window_drawable_size(presentation, screen, &drawable_w, &drawable_h);
    const NdsPresentRect rect =
        window_present_rect(presentation, screen, drawable_w, drawable_h);
    const SDL_Rect screen_rect{rect.x, rect.y, rect.w, rect.h};
    render_screen(presentation, screen, screen_rect);
    const DsTarget target = sdl_ds_target(
        renderer, screen_rect, presentation.screen_widths[screen]);
    if (virtual_stylus_visible)
        draw_virtual_stylus(target, virtual_stylus_x, virtual_stylus_y);
    draw_savestate_notice(target, savestate_notice);
    draw_fps_counter(target, fps_counter);
}

// Single-screen layout: present presentation.shown_screen, with the other LCD
// as an optional inset next to it, and the runtime menu, virtual stylus (bottom
// LCD only) and savestate notice on the shown screen.
// The GL window has no SDL renderer, so there the stylus and notice go
// through the presenter's overlay layer, and an open runtime menu (drawn into
// the CPU pixels) makes that frame present the CPU composite instead of HD.
PresentationTicks present_single_screen(FrontendPresentation& presentation,
                                        const uint32_t* top_pixels,
                                        int top_width,
                                        const uint32_t* bottom_pixels,
                                        int bottom_width,
                                        RecompRuntimeUi* runtime_ui,
                                        bool virtual_stylus_visible,
                                        float virtual_stylus_x,
                                        float virtual_stylus_y,
                                        const char* savestate_notice,
                                        const char* fps_counter) {
    PresentationTicks ticks{};
    const int shown = presentation.shown_screen & 1;
    const uint32_t* pixels = shown ? bottom_pixels : top_pixels;
    const int width = shown ? bottom_width : top_width;
    const uint32_t* other_pixels = shown ? top_pixels : bottom_pixels;
    const int other_width = shown ? top_width : bottom_width;
    const bool stylus = virtual_stylus_visible && shown == 1;
    const bool menu_open = runtime_ui && recomp_runtime_ui_is_open(runtime_ui);
    pixels = runtime_menu_overlay(runtime_ui, pixels, width, kScreenHeight);
    if (presentation.gl_top) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        // The OBJ-only direct path draws engine A on the top LCD only.
        NdsGpu2dDirectFrame direct_frame{};
        const bool direct = shown == 0 && !menu_open &&
                            nds_gpu2d_direct_frame(&direct_frame);
        if (menu_open) nds_gpu2d_invalidate_hd_frame();
        static std::vector<uint32_t> overlay;
        const bool want_overlay =
            stylus || (savestate_notice && *savestate_notice) ||
            (fps_counter && *fps_counter);
        if (want_overlay) {
            overlay.assign(size_t(width) * kScreenHeight, 0u);
            DsTarget target{};
            target.pixels = overlay.data();
            target.pixels_width = width;
            target.pixels_height = kScreenHeight;
            target.canvas = {SDL_Rect{0, 0, width, kScreenHeight}, width,
                             kScreenHeight};
            if (stylus)
                draw_virtual_stylus(target, virtual_stylus_x,
                                    virtual_stylus_y);
            draw_savestate_notice(target, savestate_notice);
            draw_fps_counter(target, fps_counter);
        }
        nds_compute_host_set_overlay(want_overlay ? overlay.data() : nullptr,
                                     static_cast<unsigned>(width));
        NdsComputePresentTicks gl_ticks{};
        gl_ticks.diagnostic =
            nds_frontend_latency::enabled.load(std::memory_order_relaxed);
        nds_compute_host_set_present_mode(presentation.window_mode,
                                          presentation.scaling);
        nds_compute_host_set_raster_request(presentation.resolution_scale,
                                            presentation.internal_resolution);
        nds_compute_host_set_inset(other_pixels,
                                   static_cast<unsigned>(other_width),
                                   presentation.inset_percent);
        // An open menu is drawn into `pixels`, the full composite, and has
        // already dropped the split along with the HD surfaces.
        const bool layer_set = !menu_open && !direct && set_fixed_layers(width);
        NdsGpu2dFixedLayersFrame fixed_shown{};
        if (layer_set && shown == 1 &&
            nds_gpu2d_fixed_layers_frame(&fixed_shown)) {
            g_touch_ui_anchor_x.store(float(fixed_shown.anchor_x));
            g_touch_ui_anchor_y.store(float(fixed_shown.anchor_y));
            g_touch_ui_scale.store(fixed_shown.scale);
        } else {
            g_touch_ui_scale.store(1.0f);
        }
        const bool ok = nds_compute_host_present_top(
            pixels, static_cast<uint16_t>(width),
            direct ? &direct_frame : nullptr, &gl_ticks,
            presentation.antialiasing);
        nds_compute_host_set_overlay(nullptr, 0);
        nds_compute_host_set_inset(nullptr, 0, 0);
        nds_compute_host_set_layer(nullptr, nullptr, 0, 0, 0, 0, 0);
        if (!ok) {
            std::fprintf(stderr, "[gpu3d] single-screen presentation failed\n");
            ticks.ok = false;
            return ticks;
        }
        ticks.upload += gl_ticks.upload;
        ticks.draw += gl_ticks.draw;
        ticks.swap += gl_ticks.swap;
        ticks.gl_setup = gl_ticks.setup;
        ticks.gl_error = gl_ticks.error;
#endif
        return ticks;
    }
    uint64_t start = SDL_GetPerformanceCounter();
    SDL_UpdateTexture(presentation.textures[shown], nullptr, pixels,
                      width * sizeof(uint32_t));
    if (presentation.inset_percent > 0)
        SDL_UpdateTexture(presentation.textures[shown ^ 1], nullptr,
                          other_pixels, other_width * sizeof(uint32_t));
    ticks.upload += SDL_GetPerformanceCounter() - start;
    SDL_Renderer* renderer = presentation.renderers[0];
    start = SDL_GetPerformanceCounter();
    SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
    SDL_RenderClear(renderer);
    int drawable_w = 0, drawable_h = 0;
    window_drawable_size(presentation, 0, &drawable_w, &drawable_h);
    const NdsSingleLayout layout = nds_single_layout(
        presentation.window_mode, presentation.scaling, drawable_w, drawable_h,
        width, kScreenHeight, presentation.inset_percent,
        presentation.stacked);
    const NdsPresentRect& rect = layout.main;
    const SDL_Rect screen_rect{rect.x, rect.y, rect.w, rect.h};
    render_screen(presentation, shown, screen_rect);
    if (layout.inset.w > 0 && layout.inset.h > 0) {
        const SDL_Rect inset_rect{layout.inset.x, layout.inset.y,
                                  layout.inset.w, layout.inset.h};
        render_screen(presentation, shown ^ 1, inset_rect);
    }
    const DsTarget target = sdl_ds_target(renderer, screen_rect, width);
    if (stylus)
        draw_virtual_stylus(target, virtual_stylus_x, virtual_stylus_y);
    draw_savestate_notice(target, savestate_notice);
    draw_fps_counter(target, fps_counter);
    ticks.draw += SDL_GetPerformanceCounter() - start;
    start = SDL_GetPerformanceCounter();
    SDL_RenderPresent(renderer);
    ticks.swap += SDL_GetPerformanceCounter() - start;
    return ticks;
}

PresentationTicks present_screens(FrontendPresentation& presentation,
                                  const uint32_t* top_pixels,
                                  int top_width,
                                  const uint32_t* bottom_pixels,
                                  int bottom_width,
                                  RecompRuntimeUi* runtime_ui,
                                  bool virtual_stylus_visible,
                                  float virtual_stylus_x,
                                  float virtual_stylus_y,
                                  const char* savestate_notice,
                                  const char* fps_counter
#if defined(NDS_HAVE_COMPUTE_RENDERER)
                                  ,const NdsComputePresentationFrame* owned = nullptr
#endif
                                  ) {
    PresentationTicks ticks{};
    if (presentation.single)
        return present_single_screen(
            presentation, top_pixels, top_width, bottom_pixels, bottom_width,
            runtime_ui, virtual_stylus_visible, virtual_stylus_x,
            virtual_stylus_y, savestate_notice, fps_counter);
    if (presentation.gl_top) {
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        NdsGpu2dDirectFrame direct_frame{};
        const bool latency = nds_frontend_latency::enabled.load(std::memory_order_relaxed);
        const auto pool_begin = latency ? SDL_GetPerformanceCounter() : 0;
        const bool direct = owned ? (direct_frame=owned->direct,owned->has_direct)
                                  : nds_gpu2d_direct_frame(&direct_frame);
        if (latency) ticks.pool_wait = SDL_GetPerformanceCounter() - pool_begin;
        NdsComputePresentTicks gl_ticks{};
        gl_ticks.diagnostic = latency;
        nds_compute_host_set_present_mode(presentation.window_mode,
                                          presentation.scaling);
        nds_compute_host_set_raster_request(presentation.resolution_scale,
                                            presentation.internal_resolution);
        g_touch_ui_scale.store(1.0f);  // the touchscreen has its own window
        if (!owned && !direct) set_fixed_layers(top_width);
        const bool top_ok = nds_compute_host_present_top(
                top_pixels, static_cast<uint16_t>(top_width),
                direct ? &direct_frame : nullptr, &gl_ticks,
                presentation.antialiasing,owned);
        nds_compute_host_set_layer(nullptr, nullptr, 0, 0, 0, 0, 0);
        if (!top_ok) {
            std::fprintf(stderr, "[gpu3d] direct top presentation failed\n");
            ticks.ok = false;
            return ticks;
        }
        ticks.upload += gl_ticks.upload;
        ticks.draw += gl_ticks.draw;
        ticks.swap += gl_ticks.swap;
        ticks.gl_setup = gl_ticks.setup;
        ticks.gl_error = gl_ticks.error;
        const auto overlay_begin = latency ? SDL_GetPerformanceCounter() : 0;
        bottom_pixels = runtime_menu_overlay(
            runtime_ui, bottom_pixels, bottom_width, kScreenHeight);
        if (latency) ticks.overlay = SDL_GetPerformanceCounter() - overlay_begin;
        uint64_t start = SDL_GetPerformanceCounter();
        SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,
                          bottom_width * sizeof(uint32_t));
        ticks.upload += SDL_GetPerformanceCounter() - start;
        SDL_Renderer* renderer = presentation.renderers[1];
        start = SDL_GetPerformanceCounter();
        draw_separate_screen(presentation, 1, virtual_stylus_visible,
                             virtual_stylus_x, virtual_stylus_y,
                             savestate_notice, fps_counter);
        ticks.draw += SDL_GetPerformanceCounter() - start;
        start = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        ticks.swap += SDL_GetPerformanceCounter() - start;
#endif
        return ticks;
    }
    top_pixels = runtime_menu_overlay(
        runtime_ui, top_pixels, top_width, kScreenHeight);
    uint64_t start = SDL_GetPerformanceCounter();
    SDL_UpdateTexture(presentation.textures[0], nullptr, top_pixels,
                      top_width * sizeof(uint32_t));
    SDL_UpdateTexture(presentation.textures[1], nullptr, bottom_pixels,
                      bottom_width * sizeof(uint32_t));
    ticks.upload += SDL_GetPerformanceCounter() - start;
    if (!presentation.separate) {
        SDL_Renderer* renderer = presentation.renderers[0];
        start = SDL_GetPerformanceCounter();
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        int drawable_w = 0, drawable_h = 0;
        window_drawable_size(presentation, 0, &drawable_w, &drawable_h);
        const NdsPresentRect rect =
            window_present_rect(presentation, 0, drawable_w, drawable_h);
        const int canvas_w = presentation.canvas_width;
        const int canvas_h = kScreenHeight * 2;
        const SDL_Rect top_rect = content_sub_rect(
            rect, canvas_w, canvas_h,
            (canvas_w - presentation.screen_widths[0]) / 2, 0,
            presentation.screen_widths[0], kScreenHeight);
        const SDL_Rect bottom_rect = content_sub_rect(
            rect, canvas_w, canvas_h,
            (canvas_w - presentation.screen_widths[1]) / 2, kScreenHeight,
            presentation.screen_widths[1], kScreenHeight);
        render_screen(presentation, 0, top_rect);
        render_screen(presentation, 1, bottom_rect);
        const DsTarget target = sdl_ds_target(
            renderer, bottom_rect, presentation.screen_widths[1]);
        if (virtual_stylus_visible)
            draw_virtual_stylus(target, virtual_stylus_x, virtual_stylus_y);
        draw_savestate_notice(target, savestate_notice);
        draw_fps_counter(target, fps_counter);
        ticks.draw += SDL_GetPerformanceCounter() - start;
        start = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        ticks.swap += SDL_GetPerformanceCounter() - start;
        return ticks;
    }

    for (int screen = 0; screen < 2; ++screen) {
        SDL_Renderer* renderer = presentation.renderers[screen];
        start = SDL_GetPerformanceCounter();
        draw_separate_screen(presentation, screen,
                             screen == 1 && virtual_stylus_visible,
                             virtual_stylus_x, virtual_stylus_y,
                             screen == 1 ? savestate_notice : nullptr,
                             screen == 1 ? fps_counter : nullptr);
        ticks.draw += SDL_GetPerformanceCounter() - start;
        start = SDL_GetPerformanceCounter();
        SDL_RenderPresent(renderer);
        ticks.swap += SDL_GetPerformanceCounter() - start;
    }
    return ticks;
}

} // namespace

int nds_run_interactive_frontend(const NdsFrontendOptions& initial_options) {
    NdsFrontendOptions options = initial_options;
    g_window_icon = &options.window_icon_rgba;
    // Wayland identifies a window by app id, not by title or icon.
#if defined(NDS_HAVE_SDL3)
    SDL_SetHint(SDL_HINT_APP_ID, options.app_name.empty()
                                     ? "ndsrecomp" : options.app_name.c_str());
#endif
    SDL_SetMainReady();
    // SDL_INIT_TIMER matters on Windows: it raises the OS timer resolution
    // to 1 ms (SDL_HINT_TIMER_RESOLUTION default). Without it SDL_Delay(1)
    // sleeps a full ~15.6 ms scheduler quantum, so the audio-queue throttle
    // overshoots every frame, pinning the loop at ~57 FPS and cyclically
    // starving the audio queue (the audible boot crackle).
    if (!sdl_init_frontend()) {
        std::fprintf(stderr, "[sdl] init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!sdl_set_thread_priority_high())
        std::fprintf(stderr, "[sdl] thread priority unchanged: %s\n",
                     SDL_GetError());

#if !defined(NDS_HAVE_SDL3)
    if (!SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, "0",
                                 SDL_HINT_OVERRIDE)) {
        std::fprintf(stderr,
                     "[sdl] render scale quality hint was not applied\n");
    }
#endif
    FrontendPresentation presentation{};
    if (!create_presentation(options, presentation)) {
        SDL_Quit();
        return 1;
    }
    const int bottom_logical_width =
        presentation.separate || presentation.single
        ? presentation.screen_widths[1]
        : presentation.canvas_width;
    const int bottom_content_left =
        (bottom_logical_width - kScreenWidth) / 2;
    const int top_content_left = presentation.separate
        ? 0 : (presentation.canvas_width - presentation.screen_widths[0]) / 2;
    const NdsLogicalRect stacked_top_screen{
        top_content_left, 0, presentation.screen_widths[0], kScreenHeight};
    const NdsLogicalRect stacked_bottom_touch{
        bottom_content_left, kScreenHeight, kScreenWidth, kScreenHeight};
    std::fprintf(stderr,
        "[sdl] layout=%s fullscreen=%s window_mode=%s scaling=%s "
        "adaptive=%s supersampling=%ux aa=%ux internal=%ux\n",
        nds_screen_layout_name(options.screen_layout),
        nds_fullscreen_mode_name(options.fullscreen),
        nds_window_mode_name(options.window_mode),
        nds_scaling_filter_name(options.scaling),
        nds_adaptive_screens_name(options.adaptive_screens),
        static_cast<unsigned>(options.supersampling),
        static_cast<unsigned>(options.antialiasing),
        static_cast<unsigned>(options.internal_resolution));
    const uint16_t output_width = static_cast<uint16_t>(std::max(
        presentation.screen_widths[0],
        presentation.screen_widths[1]));
    if (!nds_gpu3d_set_output_width(output_width)) {
        std::fprintf(stderr,
                     "[sdl] adaptive 3D width %u is unavailable\n",
                     output_width);
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
    // Must precede nds_compute_host_start(): the scale is a baked shader
    // constant, so it has to be known before the renderer compiles anything.
    //
    // Deliberately NOT gated on the direct presenter. Rendering at scale and
    // presenting at scale are separate: the scaled raster still point-samples
    // the same native surface every faithful consumer reads, so a run without
    // the GPU presenter is a valid way to prove that invariance holds. Only
    // the visible benefit needs gl_top, so that is what the notice says.
    // The multiplier the user asked for (fixed, or resolution_scale of the
    // presented rect). The perf governor caps it; the auto-scale controller
    // in the frame loop moves it as the window changes.
    const bool resolution_scale_live =
        options.resolution_scale > 0u && presentation.gl_top;
    if (options.resolution_scale > 0u && !presentation.gl_top) {
        std::fprintf(stderr,
            "[display] resolution_scale ignored: needs the OpenGL compute "
            "renderer and the separate or single layout\n");
    }
    uint8_t requested_internal_scale = options.internal_resolution;
    if (resolution_scale_live) {
        NdsPresentRect rect{};
        requested_internal_scale =
            requested_internal_scale_for(presentation, options, &rect);
        std::fprintf(stderr,
                     "[display] resolution scale %u%%: internal %ux "
                     "(presented %dx%d)\n",
                     static_cast<unsigned>(options.resolution_scale),
                     static_cast<unsigned>(requested_internal_scale),
                     rect.w, rect.h);
    }
    const uint8_t initial_internal_resolution =
        options.perf_governor_mode == NdsPerfGovernorMode::ForceStage2
            ? 1u
            : requested_internal_scale;
    if (!nds_gpu3d_set_internal_scale(initial_internal_resolution)) {
        std::fprintf(stderr,
                     "[sdl] internal resolution %ux is unavailable\n",
                     static_cast<unsigned>(initial_internal_resolution));
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
    if (options.internal_resolution > 1 && !presentation.gl_top) {
        std::fprintf(stderr,
            "[sdl] internal resolution %ux renders but is not presented: "
            "the extra sample density needs the direct OpenGL top-screen "
            "presenter\n",
            static_cast<unsigned>(options.internal_resolution));
    }
    nds_texture_upscale_set_factor(options.texture_upscale);

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Activate only after every fallible visible-frontend allocation. From
    // here onward teardown always destroys the compute renderer while this
    // context is current.
    if (!options.settings_path.empty()) {
        // Driver pipeline caches live beside settings.toml.
        const std::string path = options.settings_path;
        const size_t slash = path.find_last_of("/\\");
        nds_compute_host_set_cache_directory(
            slash == std::string::npos ? "." : path.substr(0, slash).c_str());
    }
    if (!nds_compute_host_start(
            presentation.gl_top ? presentation.windows[0] : nullptr)) {
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
    // Auto is allowed to recover from missing/failed GL 4.3. A direct-top
    // presentation was created without an SDL renderer, so rebuild the same
    // windows on the faithful SDL/software path after that fallback.
    if (presentation.gl_top && !nds_compute_host_active()) {
        destroy_presentation(presentation);
        presentation = {};
        if (!create_presentation(options, presentation, false)) {
            SDL_Quit();
            return 1;
        }
    }
    nds_gpu2d_set_direct_present(
        nds_compute_host_has_visible_context() &&
        nds_compute_host_composes_3d() && nds_compute_host_presents_direct());
    const char* window_direct = std::getenv("NDS_COMPUTE_WINDOW_DIRECT");
    nds_gpu2d_set_direct_windows(window_direct &&
        std::strcmp(window_direct, "1") == 0 && options.internal_resolution == 1u &&
        options.resolution_scale == 0u);
#else
    if (nds_gpu3d_renderer_policy() == NdsGpu3dRendererPolicy::Auto) {
        std::fprintf(stderr,
            "[gpu3d] OpenGL renderer not built; "
            "automatic fallback to threaded soft\n");
    }
#endif
    // The adaptive compositor only pays for the extra per-pixel stores when
    // something can consume them. Decided only now: whether the presenter
    // composes 3D depends on the renderer having started, and before that it
    // always reads false, which left the HD path off (a 1x, pixelated
    // picture) until a resize changed the multiplier and asked again.
    nds_gpu2d_set_hd_emit(initial_internal_resolution > 1 &&
                          presenter_composes_3d(presentation));

    NdsPerfGovernorState perf_governor{};
    // Auto-scale controller state (see the frame loop's join point).
    bool auto_scale_enabled = true;
    uint8_t auto_scale_candidate = 0u;
    uint64_t auto_scale_since = 0u;
    // Stage 2 drops 3D to 1x, which only relieves a GPU-bound frame; a
    // CPU-bound title (DQ9 runs ~13 ms of ARM per frame) would otherwise be
    // pinned at 1x by load the multiplier does not cause.
    NdsPerfGovernorConfig perf_governor_config{};
    // 4 ms is about the headroom such a frame has left. Boot reads 1-4 ms
    // with no real GPU load, and DQ9 plays at ~0.4 ms even with HD frames.
    perf_governor_config.min_stage2_gpu_wait_ms = 4.0;
    // Five minutes at 60 fps, then a flapping machine gets another try.
    perf_governor_config.flap_hold_frames = 18000;
    nds_perf_governor_init(&perf_governor,
                           options.perf_governor_mode,
                           perf_governor_config);
    // Governor off is byte-identical to the pre-governor frontend: the scale,
    // the HD emit flag and the readback latency were all already established
    // above from the options, so there is nothing to apply and no new way to
    // fail at startup.
    if (options.perf_governor_mode != NdsPerfGovernorMode::Off &&
        !apply_performance_governor_stage(
            presentation, perf_governor.stage, requested_internal_scale)) {
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }

    // "Frame interpolation (experimental)" — docs/frame_interpolation.md.
    // Resolved here, after the compute-renderer fallback above, because that
    // can rebuild the presentation on the SDL path and clear gl_top.
    //
    // Diagnostic-only override: NDS_FRAME_INTERPOLATION_MIN_REFRESH=<hz>
    // lowers the refresh gate so the blend path can be exercised on a 60 Hz
    // panel during validation. Leave it unset in normal use — a synthetic
    // present has nowhere to land on a display that is not comfortably above
    // 60 Hz, and forcing one only spends present time for no visible frame.
    constexpr int kInterpolationMinRefreshHz = 100;
    // The audio queue is this frontend's real-time clock, so it is also the
    // budget for the extra present: a blend happens only while the queue
    // still holds a comfortable runway (half the steady-state target, ~31 ms).
    // At or below that the loop is not keeping up and the synthetic present is
    // skipped for that frame. Nothing in this path ever sleeps or busy-loops.
    constexpr uint32_t kInterpolationAudioFloorFrames = kAudioQueueFrames / 2;
    int interpolation_min_refresh_hz = kInterpolationMinRefreshHz;
    if (const uint64_t forced_min_refresh =
            environment_u64("NDS_FRAME_INTERPOLATION_MIN_REFRESH")) {
        interpolation_min_refresh_hz =
            static_cast<int>(std::min<uint64_t>(forced_min_refresh, 1000));
        std::fprintf(stderr,
            "[sdl] frame interpolation: diagnostic min-refresh override "
            "%d Hz\n", interpolation_min_refresh_hz);
    }
    const bool interpolation_requested =
        options.frame_interpolation == NdsFrameInterpolation::Blend;
    bool interpolation_active = false;
    int interpolation_refresh_hz = 0;
    if (interpolation_requested) {
        if (presentation.gl_top) {
            std::fprintf(stderr,
                "[sdl] frame interpolation (experimental) is disabled: the "
                "direct OpenGL top-screen presenter owns its own swap and "
                "needs an offscreen output texture before a blended frame "
                "can be inserted\n");
        } else {
            interpolation_refresh_hz =
                window_refresh_hz(presentation.windows[0]);
            if (interpolation_refresh_hz <= 0) {
                interpolation_refresh_hz = 0;
                std::fprintf(stderr,
                    "[sdl] frame interpolation: display refresh unavailable "
                    "(%s)\n", SDL_GetError());
            }
            interpolation_active =
                interpolation_refresh_hz >= interpolation_min_refresh_hz;
            std::fprintf(stderr,
                "[sdl] frame interpolation (experimental): mode=blend "
                "active=%s refresh=%dHz min_refresh=%dHz\n",
                interpolation_active ? "yes" : "no",
                interpolation_refresh_hz, interpolation_min_refresh_hz);
        }
    }

    const bool latency_trace = nds_frontend_latency::begin();
    AudioQueue audio_queue{};
    if (const char* mute = std::getenv("NDS_FRONTEND_AUDIO_MUTE"))
        audio_queue.output_muted = std::strcmp(mute, "1") == 0;
    SDL_AudioSpec want{};
    // The mixer runs once per 1024 DS system cycles. Request its integer host
    // rate directly; the sub-sample remainder is absorbed by the bounded queue
    // instead of producing a roughly once-per-second underrun at 32768 Hz.
    want.freq = kAudioFrequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
#if !defined(NDS_HAVE_SDL3)
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &audio_queue;
#endif
    NdsAudioDevice audio = open_audio_device(audio_queue, want);
    if (!audio)
        std::fprintf(stderr, "[sdl] audio unavailable: %s\n", SDL_GetError());
    else
        std::fprintf(stderr, "[sdl] audio driver: %s, output muted: %s\n",
            SDL_GetCurrentAudioDriver(), audio_queue.output_muted ? "yes" : "no");
    apply_audio_output_level(audio, audio_queue, options.audio_volume,
                             options.audio_muted);

    std::fprintf(stderr,
        "[sdl] controls: gamepad=Player 1 | bottom mouse=touch | "
        "Esc=settings | hold Shift=fast-forward%s\n",
        options.tab_turbo ? " | hold Tab=turbo" : "");
    std::fputs("[sdl] keyboard:", stderr);
    for (size_t row = 0; row < options.keyboard_bindings.size(); ++row)
        std::fprintf(stderr, " %s=%s", kNdsButtonNames[row],
            options.keyboard_bindings[row] ? SDL_GetScancodeName(
                static_cast<SDL_Scancode>(options.keyboard_bindings[row])) : "None");
    std::fputc('\n', stderr);
    if (options.relative_mouse_touch) {
        std::fprintf(stderr,
            "[sdl] relative mouse: click game screen to capture; "
            "Esc or focus loss releases; sensitivity=%u%% invert-y=%s "
            "aim=%s\n",
            static_cast<unsigned>(options.relative_mouse_sensitivity),
            options.relative_mouse_invert_y ? "on" : "off",
            options.relative_mouse_direct_aim ? "direct-unbounded"
                                              : "virtual-touch");
    }
    const bool mph_prime_controls_available =
        options.mph_prime_controls && options.relative_mouse_direct_aim;
    const bool mph_prime_unified_window_focus =
        mph_prime_controls_available &&
        options.mph_prime_unified_window_focus;
    const bool virtual_stylus_available =
        options.virtual_stylus.enabled && !mph_prime_controls_available;
    MphPrimeBindingSet mph_prime_bindings{};
    MphPadBindingSet mph_pad_bindings{};
    if (mph_prime_controls_available) {
        mph_prime_bindings = make_mph_prime_bindings(options.mph_bindings);
        mph_pad_bindings = make_mph_pad_bindings(options.mph_pad_bindings);
        if (!mph_prime_bindings.valid || !mph_pad_bindings.valid) {
            destroy_presentation(presentation);
            SDL_Quit();
            return 1;
        }
        std::fprintf(stderr,
            "[sdl] MPH Prime Controls: melonPrimeDS bindings enabled; "
            "virtual stylus sensitivity=%u%%\n",
            static_cast<unsigned>(
                options.mph_virtual_stylus_sensitivity));
    }
    MphPrimeBinding virtual_stylus_binding =
        parse_mph_prime_binding(options.virtual_stylus.binding);
    const MphPrimeBinding screen_toggle_binding =
        parse_mph_prime_binding(options.screen_toggle_binding);
    if (presentation.single &&
        screen_toggle_binding.kind != MphPrimeInputKind::Key &&
        binding_name_lower(options.screen_toggle_binding) != "none") {
        std::fprintf(stderr,
                     "[display] screen toggle binding \"%s\" is not a key; "
                     "toggle disabled\n",
                     options.screen_toggle_binding.c_str());
    }
    // Single-screen layout state: the debounced main LCD and the toggle.
    NdsMainScreenDebounce main_screen{};
    bool screen_toggled = false;
    MphPrimeBinding virtual_stylus_tap_binding =
        parse_mph_prime_binding(options.virtual_stylus.tap_binding);
    bool virtual_stylus_hold_recognized = true;
    bool virtual_stylus_tap_recognized = true;
    MphPadBinding virtual_stylus_pad_hold =
        parse_mph_pad_binding(options.virtual_stylus.pad_hold_binding,
                              &virtual_stylus_hold_recognized);
    MphPadBinding virtual_stylus_pad_tap =
        parse_mph_pad_binding(options.virtual_stylus.pad_tap_binding,
                              &virtual_stylus_tap_recognized);
    if (virtual_stylus_available &&
        ((virtual_stylus_binding.kind == MphPrimeInputKind::None &&
          binding_name_lower(options.virtual_stylus.binding) != "none" &&
          binding_name_lower(options.virtual_stylus.binding) != "unbound") ||
         (virtual_stylus_tap_binding.kind == MphPrimeInputKind::None &&
          binding_name_lower(options.virtual_stylus.tap_binding) != "none" &&
          binding_name_lower(options.virtual_stylus.tap_binding) != "unbound") ||
         !virtual_stylus_hold_recognized ||
         !virtual_stylus_tap_recognized)) {
        std::fprintf(stderr,
            "[sdl] invalid Virtual Stylus binding "
            "(key=%s tap=%s pad_hold=%s pad_tap=%s)\n",
            options.virtual_stylus.binding.c_str(),
            options.virtual_stylus.tap_binding.c_str(),
            options.virtual_stylus.pad_hold_binding.c_str(),
            options.virtual_stylus.pad_tap_binding.c_str());
        destroy_presentation(presentation);
        SDL_Quit();
        return 1;
    }
    if (virtual_stylus_available) {
        std::fprintf(stderr,
            "[sdl] Virtual Stylus: hold %s or %s; tap %s/%s or mouse left; "
            "sensitivity=%u%% pad=%u%%\n",
            options.virtual_stylus.binding.c_str(),
            options.virtual_stylus.pad_hold_binding.c_str(),
            options.virtual_stylus.tap_binding.c_str(),
            options.virtual_stylus.pad_tap_binding.c_str(),
            static_cast<unsigned>(options.virtual_stylus.sensitivity),
            static_cast<unsigned>(options.virtual_stylus.pad_sensitivity));
    }

    SDL_GameController* controller = open_first_controller();
    SDL_JoystickID controller_id = controller
        ? sdl_controller_id(controller)
        : -1;
    uint16_t keyboard_pressed = 0;
    uint16_t controller_pressed = 0;
    uint16_t mouse_pressed = 0;
    uint16_t mph_prime_pressed = 0;
    // Left analog stick mapped to the D-pad (movement in MPH, menus
    // everywhere), kept separate from button-event state so a wobbling
    // stick never fights explicit D-pad presses.
    uint16_t stick_pressed = 0;
    int zoom_stick_frames = 0;
    float zoom_stick_rem = 0.0f;
    bool zoom_save_pending = false;
    auto publish_keys = [&]() {
        nds_set_key_mask(static_cast<uint16_t>(
            0x0FFFu &
            ~(keyboard_pressed | controller_pressed | mouse_pressed |
              mph_prime_pressed | stick_pressed)));
    };
    publish_keys();
    nds_set_touch(0, 0, false);
    bool running = true;
    RuntimeMenuState runtime_menu_state{&options, &running, false};
    runtime_menu_state.audio = &audio;
    runtime_menu_state.audio_queue = &audio_queue;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    runtime_menu_state.resolution_scale_available =
        presentation.gl_top && nds_compute_host_active();
#endif
    static const char* const scaling_choices[] = {
        "Integer (pixel-perfect)", "Bilinear", "Nearest neighbor"};
    static const char* const resolution_scale_choices[] = {
        "Off (fixed)", "25%", "50%", "75%", "100%", "150%", "200%"};
    static const char* const gamepad_layout_choices[] = {
        "By label", "By position"};
    static const RecompRuntimeUiItem runtime_menu_items[] = {
        {kRuntimeScalingKey, "Display", "Scaling",
         "Integer keeps whole-pixel multiples; bilinear smooths to any size; "
         "nearest stays pixelated at any size.",
         RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, scaling_choices, 3, nullptr},
        {kRuntimeResolutionScaleKey, "Display", "Resolution scale",
         "3D detail relative to the window: 100% matches the screen, "
         "less is coarser, more supersamples.",
         RECOMP_RUNTIME_UI_CHOICE, 0, 200, 1, resolution_scale_choices, 7,
         kResolutionScaleChoiceValues},
        {kRuntimeSecondScreenKey, "Display", "Second screen size",
         "The other screen beside the main one, as a percent of its size. "
         "0 hides it.",
         RECOMP_RUNTIME_UI_INT, 0, 60, 5, nullptr, 0, nullptr},
        {kRuntimeShowFpsKey, "Display", "Show FPS",
         "New 3D images per second in the corner of the screen, updated "
         "once a second. Reads 0 when nothing 3D is moving.",
         RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
        {kRuntimeSmooth3dKey, "Display", "Smooth 3D",
         "Blends in-between 3D images for games that draw below 60. Shows 3D "
         "one frame later; may glitch.",
         RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
        {kRuntimeVolumeKey, "Audio", "Volume",
         "Host output level in percent; the game keeps running at full mix.",
         RECOMP_RUNTIME_UI_INT, 0, 100, 5, nullptr, 0, nullptr},
        {kRuntimeMuteKey, "Audio", "Mute",
         "Silence all audio output.",
         RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr},
        {kRuntimeGamepadLayoutKey, "Input", "Gamepad layout",
         "By label, pad A is DS A. By position, the pad's right button is "
         "DS A, as on a DS.",
         RECOMP_RUNTIME_UI_CHOICE, 0, 1, 1, gamepad_layout_choices, 2,
         nullptr},
        {kRuntimeMouseSensitivityKey, "Input", "Mouse sensitivity",
         "Prime Controls relative aim speed.",
         RECOMP_RUNTIME_UI_INT, 10, 400, 1, nullptr, 0, nullptr},
    };
    static const RecompRuntimeUiItem runtime_menu_system_items[] = {
        {kRuntimeResumeKey, "System", "Resume game",
         "Close settings and return to the game.",
         RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr},
        {kRuntimeQuitKey, "System", "Quit",
         "Close the runner.",
         RECOMP_RUNTIME_UI_ACTION, 0, 0, 0, nullptr, 0, nullptr},
    };
    // The title's own settings, then Netplay when the lobby thread runs,
    // go before System.
    std::vector<RecompRuntimeUiItem> runtime_menu_list(
        std::begin(runtime_menu_items), std::end(runtime_menu_items));
    for (const auto& [key, value] : options.title_settings) {
        const int id = nds_title_setting_find(key.c_str());
        if (id < 0)
            std::fprintf(stderr, "[title] ignoring unknown setting %s\n",
                         key.c_str());
        else if (!nds_title_setting_set(id, value))
            std::fprintf(stderr, "[title] ignoring %s = %d (out of range)\n",
                         key.c_str(), value);
    }
    static std::vector<std::string> title_menu_keys;
    title_menu_keys.clear();
    for (size_t i = 0; i < nds_title_setting_count(); ++i)
        title_menu_keys.push_back(
            std::string(kRuntimeTitlePrefix) +
            nds_title_setting_at(static_cast<int>(i))->key);
    for (size_t i = 0; i < nds_title_setting_count(); ++i) {
        const NdsTitleSetting& setting =
            *nds_title_setting_at(static_cast<int>(i));
        if (setting.hidden) continue;
        const bool choice = setting.choices && setting.choice_count;
        runtime_menu_list.push_back(
            {title_menu_keys[i].c_str(), "Game", setting.label,
             setting.description,
             choice ? RECOMP_RUNTIME_UI_CHOICE : RECOMP_RUNTIME_UI_INT,
             setting.minimum, setting.maximum, setting.step, setting.choices,
             setting.choice_count, setting.choice_values});
    }
#if NDS_ONLINE
    const bool netplay_menu = nds_netplay_menu_available();
    if (netplay_menu) nds_netplay_menu_append_items(&runtime_menu_list);
#endif
    runtime_menu_list.insert(runtime_menu_list.end(),
                             std::begin(runtime_menu_system_items),
                             std::end(runtime_menu_system_items));
    RecompRuntimeUiConfig runtime_menu_config{};
    // Title from the game config's [game].name; this menu used to say
    // "Metroid Prime Hunters" for every title.
    runtime_menu_config.title = options.game_name.empty()
        ? "Nintendo DS" : options.game_name.c_str();
    runtime_menu_config.subtitle = "Runtime Settings";
    runtime_menu_config.items = runtime_menu_list.data();
    runtime_menu_config.item_count = runtime_menu_list.size();
    runtime_menu_config.callbacks.context = &runtime_menu_state;
    runtime_menu_config.callbacks.get_value = runtime_menu_get_value;
    runtime_menu_config.callbacks.set_value = runtime_menu_set_value;
    runtime_menu_config.callbacks.run_action = runtime_menu_run_action;
    runtime_menu_config.callbacks.is_enabled = runtime_menu_is_enabled;
    runtime_menu_config.callbacks.save = runtime_menu_save;
#if NDS_ONLINE
    if (netplay_menu)
        runtime_menu_config.callbacks.visibility_changed =
            [](void*, int open) { nds_netplay_menu_visibility(open != 0); };
#endif
    runtime_menu_config.theme = "nds";
    runtime_menu_config.accept_label = "A / Enter";
    runtime_menu_config.back_label = "B / Backspace";
    RecompRuntimeUi* runtime_ui =
        recomp_runtime_ui_create(&runtime_menu_config);
    if (!runtime_ui) {
        std::fprintf(stderr, "[sdl] runtime menu unavailable\n");
    }
    runtime_menu_state.ui = runtime_ui;
#if NDS_ONLINE
    bool netplay_text_input = false;
#endif
    bool compute_failed = false;
    bool mouse_down = false;
    bool touch_release_pending = false;
    uint32_t touch_frames_held = 0;
    int64_t relative_delta_x = 0;
    int64_t relative_delta_y = 0;
    uint64_t relative_direct_writes = 0;
    uint64_t mph_prime_key_downs = 0;
    uint64_t mph_prime_mouse_downs = 0;
    NdsRelativeMouseTouch relative_mouse;
    std::array<bool, static_cast<size_t>(MphPrimeAction::Count)>
        mph_prime_held{};
    MphTouchSequence mph_touch_sequence{};
    float mph_virtual_x = 128.0f;
    float mph_virtual_y = 96.0f;
    // ── Gamepad dual-stick state ──────────────────────────────────────────
    // Right stick + triggers drive Prime Controls camera aim and fire,
    // engaging while used and idling back out so the touchscreen and menus
    // keep working when the sticks rest.
    bool mph_prime_pad_engaged = false;
    bool virtual_stylus_pointer_held = false;
    bool virtual_stylus_pad_held = false;
    bool virtual_stylus_tap_held = false;
    bool virtual_stylus_capture_owned = false;
    float virtual_stylus_pad_rem_x = 0.0f;
    float virtual_stylus_pad_rem_y = 0.0f;
    bool virtual_stylus_pad_trigger_left_held = false;
    bool virtual_stylus_pad_trigger_right_held = false;
    int mph_pad_idle_frames = 0;
    float mph_pad_aim_rem_x = 0.0f;
    float mph_pad_aim_rem_y = 0.0f;
    int32_t mph_pad_frame_x = 0;
    int32_t mph_pad_frame_y = 0;
    bool mph_pad_trigger_left_held = false;
    bool mph_pad_trigger_right_held = false;
    uint64_t mph_pad_aim_writes = 0;
    bool runtime_stick_held = false;
    // Back + Start opens the menu; Start alone belongs to the game.
    bool pad_back_held = false;
    bool pad_start_held = false;
    RecompRuntimeUiInput runtime_stick_input =
        RECOMP_RUNTIME_UI_INPUT_DOWN;
    uint32_t runtime_stick_frames = 0;
    auto mph_prime_active = [&]() {
        return mph_prime_controls_available &&
               (relative_mouse.captured() || mph_prime_pad_engaged);
    };
    auto virtual_stylus_active = [&]() {
        return virtual_stylus_available &&
               (virtual_stylus_pointer_held || virtual_stylus_pad_held);
    };
    auto update_mph_prime_pressed = [&]() {
        uint16_t mask = 0;
        const bool virtual_stylus =
            mph_prime_held[static_cast<size_t>(
                MphPrimeAction::VirtualStylus)];
        for (size_t i = 0; i < mph_prime_held.size(); ++i) {
            if (!mph_prime_held[i]) continue;
            const auto action = static_cast<MphPrimeAction>(i);
            if (virtual_stylus &&
                (action == MphPrimeAction::Shoot ||
                 action == MphPrimeAction::ScanShoot)) {
                continue;
            }
            mask |= mph_prime_hold_mask(action);
        }
        mph_prime_pressed = mask;
        publish_keys();
    };
    auto clear_mph_prime_controls = [&]() {
        mph_prime_held.fill(false);
        mph_prime_pressed = 0;
        mph_touch_sequence = {};
    };
    auto is_presentation_window = [&](uint32_t window_id) {
        return window_id == presentation.window_ids[0] ||
               window_id == presentation.window_ids[1];
    };
    auto presentation_window_focus_index = [&](uint32_t window_id) {
        if (window_id == presentation.window_ids[0]) return 0;
        if (presentation.separate &&
            window_id == presentation.window_ids[1]) {
            return 1;
        }
        return -1;
    };
    auto set_mph_prime_action = [&](MphPrimeAction action, bool down,
                                    bool repeat) {
        const size_t index = static_cast<size_t>(action);
        const uint16_t mask = mph_prime_hold_mask(action);
        if (mask != 0 ||
            action == MphPrimeAction::VirtualStylus) {
            mph_prime_held[index] = down;
            update_mph_prime_pressed();
            return true;
        }
        if (down && !repeat)
            start_mph_touch_action(action, mph_touch_sequence);
        return true;
    };
    auto process_mph_prime_key = [&](SDL_Scancode key, bool down,
                                     bool repeat) {
        if (!mph_prime_controls_available) return false;
        bool consumed = false;
        for (size_t i = 0; i < mph_prime_bindings.bindings.size(); ++i) {
            if (!binding_matches_key(mph_prime_bindings.bindings[i], key))
                continue;
            consumed = true;
            set_mph_prime_action(static_cast<MphPrimeAction>(i), down,
                                 repeat);
        }
        if (consumed && down && !repeat)
            ++mph_prime_key_downs;
        return consumed;
    };
    // Pad buttons bound to Prime actions are consumed here (never also sent
    // as raw DS buttons). Unlike the keyboard path this gates on
    // availability, not on active capture: pressing a bound button is
    // itself the pad's engage gesture.
    auto process_mph_prime_pad = [&](MphPadInputKind kind,
                                     SDL_GameControllerButton button,
                                     bool down) {
        if (!mph_prime_controls_available) return false;
        bool consumed = false;
        for (size_t i = 0; i < mph_pad_bindings.bindings.size(); ++i) {
            const MphPadBinding& binding = mph_pad_bindings.bindings[i];
            if (binding.kind != kind) continue;
            if (kind == MphPadInputKind::Button &&
                binding.button != button) {
                continue;
            }
            consumed = true;
            if (down) {
                mph_prime_pad_engaged = true;
                mph_pad_idle_frames = 0;
            }
            set_mph_prime_action(static_cast<MphPrimeAction>(i), down,
                                 false);
        }
        return consumed;
    };
    auto process_mph_prime_mouse = [&](uint8_t button, bool down,
                                       bool repeat) {
        if (!mph_prime_active()) return false;
        bool consumed = false;
        for (size_t i = 0; i < mph_prime_bindings.bindings.size(); ++i) {
            if (!binding_matches_mouse(mph_prime_bindings.bindings[i],
                                       button)) {
                continue;
            }
            consumed = true;
            set_mph_prime_action(static_cast<MphPrimeAction>(i), down,
                                 repeat);
        }
        if (consumed && down && !repeat)
            ++mph_prime_mouse_downs;
        return consumed;
    };
    auto release_virtual_stylus_capture = [&]() {
        if (!virtual_stylus_capture_owned) return;
        if (!relative_mouse.captured()) {
            sdl_set_relative_mouse_mode(presentation.windows[0], false);
            SDL_CaptureMouse(SDL_FALSE);
        }
        virtual_stylus_capture_owned = false;
    };
    auto clear_virtual_stylus = [&]() {
        virtual_stylus_pointer_held = false;
        virtual_stylus_pad_held = false;
        virtual_stylus_tap_held = false;
        virtual_stylus_pad_rem_x = 0.0f;
        virtual_stylus_pad_rem_y = 0.0f;
        nds_set_touch(0, 0, false);
        release_virtual_stylus_capture();
    };
    auto capture_virtual_stylus_mouse = [&]() {
        if (!virtual_stylus_available || virtual_stylus_capture_owned ||
            relative_mouse.captured()) {
            return;
        }
        if (!sdl_set_relative_mouse_mode(presentation.windows[0], true)) {
            std::fprintf(stderr,
                         "[sdl] Virtual Stylus capture failed: %s\n",
                         SDL_GetError());
            return;
        }
        SDL_CaptureMouse(SDL_TRUE);
        SDL_GetRelativeMouseState(nullptr, nullptr);
        virtual_stylus_capture_owned = true;
    };
    auto set_virtual_stylus_pointer_hold = [&](bool down) {
        if (!virtual_stylus_available) return false;
        virtual_stylus_pointer_held = down;
        if (down)
            capture_virtual_stylus_mouse();
        else if (!virtual_stylus_pad_held) {
            virtual_stylus_tap_held = false;
            release_virtual_stylus_capture();
            nds_set_touch(0, 0, false);
        }
        return true;
    };
    auto set_virtual_stylus_pad_hold = [&](bool down) {
        if (!virtual_stylus_available) return false;
        virtual_stylus_pad_held = down;
        if (!down && !virtual_stylus_pointer_held) {
            virtual_stylus_tap_held = false;
            nds_set_touch(0, 0, false);
        }
        return true;
    };
    auto process_virtual_stylus_key = [&](SDL_Scancode key, bool down) {
        if (binding_matches_key(virtual_stylus_binding, key))
            return set_virtual_stylus_pointer_hold(down);
        if (binding_matches_key(virtual_stylus_tap_binding, key)) {
            if (down) {
                if (!virtual_stylus_active()) return true;
                virtual_stylus_tap_held = true;
            } else {
                virtual_stylus_tap_held = false;
            }
            return true;
        }
        return false;
    };
    auto process_virtual_stylus_mouse_hold = [&](uint8_t button, bool down) {
        return binding_matches_mouse(virtual_stylus_binding, button) &&
               set_virtual_stylus_pointer_hold(down);
    };
    auto process_virtual_stylus_mouse_tap = [&](uint8_t button, bool down) {
        if (!binding_matches_mouse(virtual_stylus_tap_binding, button))
            return false;
        if (down) {
            if (!virtual_stylus_active()) return true;
            virtual_stylus_tap_held = true;
        } else {
            virtual_stylus_tap_held = false;
        }
        return true;
    };
    auto process_virtual_stylus_pad = [&](MphPadInputKind kind,
                                          SDL_GameControllerButton button,
                                          bool down) {
        if (pad_binding_matches(virtual_stylus_pad_hold, kind, button))
            return set_virtual_stylus_pad_hold(down);
        if (virtual_stylus_active() &&
            pad_binding_matches(virtual_stylus_pad_tap, kind, button)) {
            virtual_stylus_tap_held = down;
            return true;
        }
        return false;
    };
    auto release_relative_mouse = [&]() {
        if (relative_mouse.captured()) {
            sdl_set_relative_mouse_mode(presentation.windows[0], false);
            SDL_CaptureMouse(SDL_FALSE);
            relative_mouse.release();
            nds_set_touch(0, 0, false);
            std::fprintf(stderr, "[sdl] relative mouse released\n");
        }
        if (mouse_pressed != 0) {
            mouse_pressed = 0;
            publish_keys();
        }
        clear_mph_prime_controls();
        clear_virtual_stylus();
        publish_keys();
        relative_delta_x = 0;
        relative_delta_y = 0;
    };
    auto capture_relative_mouse = [&]() {
        if (!options.relative_mouse_touch || relative_mouse.captured())
            return;
        if (!sdl_set_relative_mouse_mode(presentation.windows[0], true)) {
            std::fprintf(stderr,
                         "[sdl] relative mouse capture failed: %s\n",
                         SDL_GetError());
            return;
        }
        SDL_CaptureMouse(SDL_TRUE);
        SDL_GetRelativeMouseState(nullptr, nullptr);
        relative_mouse.capture(options.relative_mouse_sensitivity,
                               options.relative_mouse_invert_y);
        if (mph_prime_controls_available && keyboard_pressed != 0) {
            keyboard_pressed = 0;
            publish_keys();
        }
        nds_set_touch(relative_mouse.x(), relative_mouse.y(), true);
        std::fprintf(stderr, "[sdl] relative mouse captured\n");
    };
    uint64_t shown_frames = 0;
    uint64_t synthetic_presents = 0;
    FrameBlendCache blend_cache{};
    uint64_t fps_frames = 0;
    uint64_t fps_start = SDL_GetPerformanceCounter();
    uint64_t fps_images = 0;
    std::string fps_counter;  // empty until the first second has passed
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    const uint64_t soak_frames = environment_u64("NDS_FRONTEND_MAX_FRAMES");
    const bool print_stats = std::getenv("NDS_FRONTEND_STATS") != nullptr;
    const bool require_audio =
        std::getenv("NDS_FRONTEND_REQUIRE_AUDIO") != nullptr;
    const bool selftest_menu =
        std::getenv("NDS_FRONTEND_SELFTEST_MENU") != nullptr;
    const bool selftest_relative_mouse =
        std::getenv("NDS_FRONTEND_SELFTEST_RELATIVE_MOUSE") != nullptr;
    const uint64_t relative_mouse_selftest_start_vblank =
        environment_u64("NDS_FRONTEND_SELFTEST_RELATIVE_MOUSE_VBLANK");
    bool audio_started = false;
    uint32_t audio_start_threshold = kAudioStartFrames;
    bool audio_queue_error = false;
    uint64_t audio_presentation_epoch = nds_spu_presentation_epoch();
    bool turbo_pressed = false;
    bool turbo_active = false;
    bool focus_release_pending = false;
    bool presentation_window_focused[2] = {
        (SDL_GetWindowFlags(presentation.windows[0]) &
         SDL_WINDOW_INPUT_FOCUS) != 0,
        presentation.separate &&
            (SDL_GetWindowFlags(presentation.windows[1]) &
             SDL_WINDOW_INPUT_FOCUS) != 0,
    };
    auto any_presentation_window_focused = [&]() {
        return presentation_window_focused[0] ||
               (presentation.separate && presentation_window_focused[1]);
    };
    // Both turbo sources (Shift, and the developer Tab) drop on focus loss so
    // a key released in another window cannot leave fast-forward latched.
    auto clear_tab_turbo = [&]() { turbo_pressed = false; };
    auto is_shift_key = [](SDL_Scancode key) {
        return key == SDL_SCANCODE_LSHIFT || key == SDL_SCANCODE_RSHIFT;
    };
    auto runtime_menu_open = [&]() {
        return runtime_ui && recomp_runtime_ui_is_open(runtime_ui);
    };
    auto clear_gameplay_input_for_runtime_menu = [&]() {
        release_relative_mouse();
        keyboard_pressed = 0;
        controller_pressed = 0;
        stick_pressed = 0;
        mouse_pressed = 0;
        mph_prime_pressed = 0;
        mph_prime_pad_engaged = false;
        mph_pad_trigger_left_held = false;
        mph_pad_trigger_right_held = false;
        virtual_stylus_pad_trigger_left_held = false;
        virtual_stylus_pad_trigger_right_held = false;
        turbo_pressed = false;
        clear_mph_prime_controls();
        clear_virtual_stylus();
        nds_set_touch(0, 0, false);
        publish_keys();
    };
    auto finish_runtime_menu_input = [&](bool was_open) {
        if (runtime_menu_state.close_requested) {
            runtime_menu_state.close_requested = false;
            if (runtime_ui) recomp_runtime_ui_close(runtime_ui);
        }
        if (!was_open && runtime_menu_open())
            clear_gameplay_input_for_runtime_menu();
    };
    uint32_t audio_pace_floor = kAudioQueueFrames;
    // The steady-state queue target. kAudioQueueFrames (~63 ms) leaves about
    // one late frame of runway once a frame's emulation and the device's own
    // pull are taken out, so a host that drops a frame now and then hears a
    // gap each time. Every frame that saw an underrun raises the target one
    // step (~16 ms, a 60 Hz frame), up to ~156 ms; it comes back down a step
    // for every two minutes without one. A host that never underruns keeps
    // the 63 ms latency.
    constexpr uint32_t kAudioSteadyStepFrames = 512;
    constexpr uint32_t kAudioSteadyMaxFrames = 5120;
    constexpr uint64_t kAudioSteadyDecayFrames = 2u * 60u * 60u;
    uint32_t audio_steady_floor = kAudioQueueFrames;
    uint64_t audio_steady_underruns = 0;
    uint64_t audio_steady_quiet_frames = 0;
    uint32_t audio_min_queue = std::numeric_limits<uint32_t>::max();
    uint32_t audio_max_queue = 0;
    uint64_t host_key_presses = 0;
    uint64_t host_touch_presses = 0;
    int last_touch_event_x = -1;
    int last_touch_event_y = -1;
    bool selftest_key_down = false;
    bool selftest_key_up = false;
    bool selftest_touch_down = false;
    bool selftest_touch_up = false;
    bool selftest_event_error = false;
    unsigned relative_mouse_selftest_stage = 0;
    bool relative_mouse_selftest_error = false;
    uint64_t phase_emu_ticks = 0;
    uint64_t phase_present_ticks = 0;
    uint64_t phase_adaptive_ticks = 0;
    uint64_t phase_upload_ticks = 0;
    uint64_t phase_draw_ticks = 0;
    uint64_t phase_swap_ticks = 0;
    uint64_t phase_drain_ticks = 0;
    uint64_t governor_last_underruns = 0;
    NdsGpu3dProfile governor_gpu3d_profile{};
    nds_gpu3d_profile(&governor_gpu3d_profile);
    uint64_t governor_last_compute_map_ns =
        governor_gpu3d_profile.compute_readback_ns;
    g_live_stats = {};
    g_live_stats.active = 1;
    g_live_stats.freq = frequency;
    g_black_band = {};
    g_input_debug = {};
    nds_diagnostics_start_performance_log(options);
    nds_perf_governor_history_reset();
    if (perf_governor.stage != 0u) {
        nds_perf_governor_record_transition(
            0, perf_governor.stage, NdsPerfGovernorReason::Initial, 0,
            false, false);
        nds_diagnostics_note_perf_governor_transition(
            0, perf_governor.stage, "initial");
    }
    auto publish_input_debug = [&]() {
        g_input_debug.active = 1;
        g_input_debug.mph_prime_controls_available =
            mph_prime_controls_available ? 1 : 0;
        g_input_debug.mph_prime_controls_active =
            mph_prime_active() ? 1 : 0;
        g_input_debug.virtual_stylus_available =
            virtual_stylus_available ? 1 : 0;
        g_input_debug.virtual_stylus_active =
            virtual_stylus_active() ? 1 : 0;
        g_input_debug.virtual_stylus_tap_held =
            virtual_stylus_tap_held ? 1 : 0;
        g_input_debug.relative_mouse_captured =
            relative_mouse.captured() ? 1 : 0;
        g_input_debug.keyboard_pressed = keyboard_pressed;
        g_input_debug.mouse_pressed = mouse_pressed;
        g_input_debug.mph_prime_pressed = mph_prime_pressed;
        g_input_debug.stick_pressed = stick_pressed;
        g_input_debug.pad_engaged = mph_prime_pad_engaged ? 1 : 0;
        g_input_debug.pad_aim_writes = mph_pad_aim_writes;
        g_input_debug.published_key_mask = static_cast<uint16_t>(
            0x0FFFu & ~(keyboard_pressed | controller_pressed |
                        mouse_pressed | mph_prime_pressed |
                        stick_pressed));
        g_input_debug.relative_direct_writes = relative_direct_writes;
        g_input_debug.mph_prime_key_downs = mph_prime_key_downs;
        g_input_debug.mph_prime_mouse_downs = mph_prime_mouse_downs;
        g_input_debug.virtual_stylus_x =
            static_cast<int>(std::lround(mph_virtual_x));
        g_input_debug.virtual_stylus_y =
            static_cast<int>(std::lround(mph_virtual_y));
        g_input_debug.top_window_id = presentation.window_ids[0];
        g_input_debug.bottom_window_id = presentation.window_ids[1];
        g_input_debug.bottom_content_left = bottom_content_left;
        g_input_debug.separate = presentation.separate ? 1 : 0;
    };
    publish_input_debug();
    uint64_t max_emu_ticks = 0;
    uint64_t max_emu_frame = 0;
    uint64_t slow_frames_32ms = 0;
    uint64_t last_underruns_seen = 0;
    uint64_t first_underrun_frame = 0;
    uint64_t last_underrun_frame = 0;
    const uint64_t soak_start = SDL_GetPerformanceCounter();
    std::string savestate_notice;
    uint64_t savestate_notice_until = 0;
    // A resize is saved once it has been still for half a second, so dragging
    // a corner writes the settings file once, not per event.
    uint64_t window_resize_save_at = 0;
    // What Alt+Enter enters: the configured fullscreen mode, or borderless
    // when the setting is off.
    const NdsFullscreenMode last_fullscreen_mode =
        options.fullscreen != NdsFullscreenMode::Off
            ? options.fullscreen : NdsFullscreenMode::Borderless;

    auto show_savestate_result = [&](const NdsSavestateSlotResult& result) {
        savestate_notice = result.message;
        savestate_notice_until = SDL_GetPerformanceCounter() + frequency * 4u;
        std::fprintf(stderr, "[savestate] %s\n", result.message.c_str());
        // A queued capture is not reported as a successfully committed save.
        if (!result.pending) nds_diagnostics_note_savestate(
            result.command.action == NdsSavestateSlotAction::Save ? "save" : "load",
            result.command.slot, result.success);
    };
    auto finish_pending_save = [&](bool wait) {
        if (wait) nds_savestate_wait_for_pending_save();
        NdsSavestateSlotResult completed{};
        if (nds_savestate_slot_poll_save(&completed)) show_savestate_result(completed);
    };

#if defined(NDS_HAVE_COMPUTE_RENDERER)
    // Captured producer data must outlive the handoff's exception-path drain.
    std::array<OwnedPresentationFrame,2> owned_frames;
    bool producer_priority_initialized=false;
#endif
    std::unique_ptr<NdsCpuRenderHandoff> cpu_handoff;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    int completed_slot=-1;
    uint64_t produced_jobs=0;
    const char* handoff_setting=std::getenv("NDS_CPU_RENDER_HANDOFF");
    const bool handoff_requested=handoff_setting && std::strcmp(handoff_setting,"1")==0;
    bool handoff_title_supported=false;
#if defined(NDS_MKDS_PERFORMANCE)
    handoff_title_supported=options.savestate_rom_sha1=="691e00d9a5dd80b04f80cc7559503e8b06848785";
#endif
    const std::string backend=nds_gpu3d_compiled_accelerated_backend();
    if (handoff_requested && handoff_title_supported && presentation.gl_top &&
        !presentation.single && backend=="compute" &&
        !interpolation_active && options.perf_governor_mode==NdsPerfGovernorMode::Off &&
        !selftest_menu && !selftest_relative_mouse &&
        !std::getenv("NDS_GUEST_JOB_PROFILE_OUTPUT")) {
        nds_hostprof_unregister_current_thread();
        nds_hostprof_register_current_thread(NDS_HOSTPROF_ROLE_RENDER);
        cpu_handoff=std::make_unique<NdsCpuRenderHandoff>(SDL_GetPerformanceCounter,[] {
            if (!nds_compute_host_make_current())
                throw std::runtime_error("main GL context unavailable during native borrow");
        });
        std::fprintf(stderr,"[sdl] CPU/render handoff: elective MKDS profile; worker simulation+PCM, main SDL/GL; one completed-frame presentation/PCM latency\n");
    } else if (handoff_requested) {
        std::fprintf(stderr,"[sdl] CPU/render handoff unavailable for selected title/renderer/features; serial fallback\n");
    }
#endif
    while (running) {
        finish_pending_save(false);
        const uint64_t latency_loop = latency_trace ? SDL_GetPerformanceCounter() : 0;
        const uint64_t latency_cycles_loop = latency_trace ? nds_frontend_latency::cycles() : 0;
        const uint32_t latency_queue_begin = latency_trace ? audio_queue_count(audio, audio_queue) : 0;
        const uint64_t latency_adaptive_before = phase_adaptive_ticks;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (!nds_compute_host_make_current()) {
            std::fprintf(stderr, "[gpu3d] lost compute GL context: %s\n",
                         SDL_GetError());
            compute_failed = true;
            running = false;
            break;
        }
#endif
        // Play-mode debug surface: execute any pending TCP command at this
        // between-frames safe point (no-op when no pump was started or no
        // client is connected). See debug_server.h.
        publish_input_debug();
        debug_pump();
        const uint64_t current_audio_epoch = nds_spu_presentation_epoch();
        if (current_audio_epoch != audio_presentation_epoch) {
            // A state load abandoned the samples already buffered in SDL.
            // Pause the callback/stream before clearing it, then use the normal
            // short steady-state prebuffer to restart from restored guest time.
            pause_audio(audio, true);
            clear_audio_queue(audio, audio_queue);
            audio_queue.started.store(false, std::memory_order_relaxed);
            audio_started = false;
            audio_start_threshold = kAudioQueueFrames;
            audio_pace_floor = kAudioQueueFrames;
            audio_presentation_epoch = current_audio_epoch;
        }
        if (selftest_menu) {
            const NdsEventCounts& counts = nds_event_counts();
            SDL_Event injected{};
            if (!selftest_key_down && counts.vblank9 >= 10) {
                injected.type = SDL_KEYDOWN;
                sdl_set_event_scancode(injected, SDL_SCANCODE_Q);
                injected.key.repeat = 0;
                selftest_event_error |= !sdl_push_event(injected);
                selftest_key_down = true;
            } else if (selftest_key_down && !selftest_key_up &&
                       counts.vblank9 >= 12) {
                injected.type = SDL_KEYUP;
                sdl_set_event_scancode(injected, SDL_SCANCODE_Q);
                injected.key.repeat = 0;
                selftest_event_error |= !sdl_push_event(injected);
                selftest_key_up = true;
            }
            if (!selftest_touch_down && g_insn_count[0] >= 42300000) {
                injected = {};
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[1];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, true);
                // Synthetic events use window pixels; the SDL3 frontend path
                // converts absolute mouse positions into logical DS-space.
                injected.button.x =
                    (bottom_content_left + 127) * kWindowScale;
                injected.button.y = (presentation.separate
                    ? 180 : 192 + 180) * kWindowScale;
                selftest_event_error |= !sdl_push_event(injected);
                selftest_touch_down = true;
            } else if (selftest_touch_down && !selftest_touch_up &&
                       counts.vblank9 >= 116) {
                injected = {};
                injected.type = SDL_MOUSEBUTTONUP;
                injected.button.windowID = presentation.window_ids[1];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, false);
                injected.button.x =
                    (bottom_content_left + 127) * kWindowScale;
                injected.button.y = (presentation.separate
                    ? 180 : 192 + 180) * kWindowScale;
                selftest_event_error |= !sdl_push_event(injected);
                selftest_touch_up = true;
            }
        }
        if (selftest_relative_mouse) {
            SDL_Event injected{};
            if (relative_mouse_selftest_stage == 0 && shown_frames >= 2 &&
                (relative_mouse_selftest_start_vblank == 0 ||
                 nds_event_counts().vblank9 >=
                     relative_mouse_selftest_start_vblank)) {
                std::fprintf(stderr,
                    "[sdl] relative mouse self-test starting at VBlank %llu\n",
                    static_cast<unsigned long long>(
                        nds_event_counts().vblank9));
                relative_mouse_selftest_error |=
                    !options.relative_mouse_touch ||
                    options.relative_mouse_fire_mask == 0;
                SDL_RaiseWindow(presentation.windows[0]);
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, true);
                // Stacked first verifies that the bottom logical screen
                // remains touch-only. Separate verifies the traditional
                // top-window capture path.
                injected.button.x = presentation.separate
                    ? 127 * kWindowScale
                    : (bottom_content_left + 127) * kWindowScale;
                injected.button.y = presentation.separate
                    ? 96 * kWindowScale
                    : (kScreenHeight + 96) * kWindowScale;
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 1;
            } else if (relative_mouse_selftest_stage == 1 &&
                       shown_frames >= 3) {
                if (presentation.separate) {
                    relative_mouse_selftest_error |= !relative_mouse.captured();
                    injected.type = SDL_MOUSEMOTION;
                    injected.motion.windowID = presentation.window_ids[0];
                    injected.motion.xrel = 20;
                    injected.motion.yrel = -10;
                    relative_mouse_selftest_stage = 3;
                } else {
                    relative_mouse_selftest_error |= relative_mouse.captured() ||
                        !mouse_down;
                    injected.type = SDL_MOUSEBUTTONUP;
                    injected.button.windowID = presentation.window_ids[0];
                    injected.button.button = SDL_BUTTON_LEFT;
                    sdl_set_mouse_button_state(injected, false);
                    injected.button.x =
                        (bottom_content_left + 127) * kWindowScale;
                    injected.button.y =
                        (kScreenHeight + 96) * kWindowScale;
                    relative_mouse_selftest_stage = 2;
                }
                relative_mouse_selftest_error |= !sdl_push_event(injected);
            } else if (relative_mouse_selftest_stage == 2 &&
                       shown_frames >= 4) {
                relative_mouse_selftest_error |= mouse_down;
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, true);
                injected.button.x =
                    (top_content_left + 127) * kWindowScale;
                injected.button.y = 96 * kWindowScale;
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 3;
            } else if (relative_mouse_selftest_stage == 3 &&
                       shown_frames >= 5) {
                relative_mouse_selftest_error |= !relative_mouse.captured();
                injected.type = SDL_MOUSEMOTION;
                injected.motion.windowID = presentation.window_ids[0];
                injected.motion.xrel = 20;
                injected.motion.yrel = -10;
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 4;
            } else if (relative_mouse_selftest_stage == 4 &&
                       shown_frames >= 6) {
                relative_mouse_selftest_error |=
                    options.relative_mouse_direct_aim
                        ? relative_direct_writes == 0
                        : (relative_mouse.x() == 128 &&
                           relative_mouse.y() == 96);
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, true);
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 5;
            } else if (relative_mouse_selftest_stage == 5 &&
                       shown_frames >= 7) {
                relative_mouse_selftest_error |=
                    mouse_pressed != options.relative_mouse_fire_mask;
                std::fprintf(stderr,
                    "[sdl] relative mouse fire asserted at VBlank %llu\n",
                    static_cast<unsigned long long>(
                        nds_event_counts().vblank9));
                injected.type = SDL_MOUSEBUTTONUP;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, false);
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 6;
            } else if (relative_mouse_selftest_stage == 6 &&
                       shown_frames >= 8) {
                relative_mouse_selftest_error |= mouse_pressed != 0;
                injected.type = SDL_KEYDOWN;
                sdl_set_event_scancode(injected, SDL_SCANCODE_ESCAPE);
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 7;
            } else if (relative_mouse_selftest_stage == 7 &&
                       shown_frames >= 9) {
                relative_mouse_selftest_error |=
                    relative_mouse.captured() || mouse_pressed != 0;
                injected.type = SDL_MOUSEBUTTONDOWN;
                injected.button.windowID = presentation.window_ids[0];
                injected.button.button = SDL_BUTTON_LEFT;
                sdl_set_mouse_button_state(injected, true);
                injected.button.x =
                    (top_content_left + 127) * kWindowScale;
                injected.button.y = 96 * kWindowScale;
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 8;
            } else if (relative_mouse_selftest_stage == 8 &&
                       shown_frames >= 10) {
                relative_mouse_selftest_error |= !relative_mouse.captured();
                sdl_make_window_event(
                    injected, SDL_EVENT_WINDOW_FOCUS_LOST,
                    presentation.window_ids[0]);
                relative_mouse_selftest_error |= !sdl_push_event(injected);
                relative_mouse_selftest_stage = 9;
            } else if (relative_mouse_selftest_stage == 9 &&
                       shown_frames >= 11) {
                relative_mouse_selftest_error |=
                    relative_mouse.captured() || mouse_pressed != 0;
                relative_mouse_selftest_stage = 10;
            }
        }
#if NDS_ONLINE
        if (netplay_menu) {
            nds_netplay_menu_tick(runtime_ui);
        }
#endif
        runtime_menu_serve_title(&runtime_menu_state, runtime_menu_list);
#if NDS_ONLINE
        if (netplay_menu) {
            const bool editing = nds_netplay_menu_editing();
            if (editing != netplay_text_input) {
                netplay_text_input = editing;
                for (SDL_Window* window : presentation.windows) {
                    if (!window) continue;
#if defined(NDS_HAVE_SDL3)
                    if (editing) SDL_StartTextInput(window);
                    else SDL_StopTextInput(window);
#endif
                }
#if !defined(NDS_HAVE_SDL3)
                if (editing) SDL_StartTextInput();
                else SDL_StopTextInput();
#endif
            }
        }
#endif
        if (window_resize_save_at &&
            SDL_GetPerformanceCounter() >= window_resize_save_at) {
            window_resize_save_at = 0;
            persist_window_size(options, presentation.windows[0]);
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            convert_mouse_event_to_logical_coordinates(event, presentation);
            if (event.type == SDL_QUIT) {
                release_relative_mouse();
                running = false;
            }
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
                release_relative_mouse();
                running = false;
            }
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_RESIZED) &&
                !presentation.separate && presentation.windows[0] &&
                event.window.windowID ==
                    SDL_GetWindowID(presentation.windows[0]))
                window_resize_save_at =
                    SDL_GetPerformanceCounter() + frequency / 2u;
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_FOCUS_LOST) &&
                is_presentation_window(event.window.windowID)) {
                const int index =
                    presentation_window_focus_index(event.window.windowID);
                if (index >= 0)
                    presentation_window_focused[index] = false;
                if (mph_prime_unified_window_focus)
                    focus_release_pending = true;
                else {
                    release_relative_mouse();
                    clear_tab_turbo();
                }
            }
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_FOCUS_GAINED) &&
                is_presentation_window(event.window.windowID)) {
                const int index =
                    presentation_window_focus_index(event.window.windowID);
                if (index >= 0)
                    presentation_window_focused[index] = true;
                focus_release_pending = false;
            }
#if NDS_ONLINE
            // The lobby name editor has the keyboard (and A / B) to itself.
            if (nds_netplay_menu_editing()) {
#if defined(NDS_HAVE_SDL3)
                const bool text_event = event.type == SDL_EVENT_TEXT_INPUT;
#else
                const bool text_event = event.type == SDL_TEXTINPUT;
#endif
                bool consumed = true;
                if (text_event) {
                    nds_netplay_menu_edit_text(event.text.text);
                } else if (event.type == SDL_KEYDOWN) {
                    const SDL_Scancode key = sdl_event_scancode(event);
                    if (key == SDL_SCANCODE_RETURN ||
                        key == SDL_SCANCODE_KP_ENTER) {
                        if (nds_netplay_menu_edit_key(NdsNetplayEditKey::Commit))
                            runtime_menu_save(&runtime_menu_state);
                    } else if (key == SDL_SCANCODE_ESCAPE) {
                        nds_netplay_menu_edit_key(NdsNetplayEditKey::Cancel);
                    } else if (key == SDL_SCANCODE_BACKSPACE) {
                        nds_netplay_menu_edit_key(NdsNetplayEditKey::Erase);
                    }
                } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                    const auto button = static_cast<SDL_GameControllerButton>(
                        sdl_controller_button(event));
                    if (button == SDL_CONTROLLER_BUTTON_A) {
                        if (nds_netplay_menu_edit_key(NdsNetplayEditKey::Commit))
                            runtime_menu_save(&runtime_menu_state);
                    } else if (button == SDL_CONTROLLER_BUTTON_B) {
                        nds_netplay_menu_edit_key(NdsNetplayEditKey::Cancel);
                    }
                } else {
                    consumed = event.type == SDL_KEYUP ||
                               event.type == SDL_CONTROLLERBUTTONUP;
                }
                if (consumed) continue;
            }
#endif
            // NDS_DEBUG_KEYEV=1: trace every polled keyboard event to stderr.
            // Exists because an injected key that vanishes between
            // SDL_PushEvent (debug pump, counted) and this loop is otherwise
            // undiagnosable -- the 2026-08-31 A/B session lost an evening leg
            // to exactly that.
            static const bool debug_keyev = [] {
                const char* v = std::getenv("NDS_DEBUG_KEYEV");
                return v && v[0] == '1';
            }();
            if (debug_keyev &&
                (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
                std::fprintf(stderr, "[keyev] type=%u scancode=%d repeat=%d\n",
                             (unsigned)event.type,
                             (int)sdl_event_scancode(event),
                             (int)event.key.repeat);
            }
            // Alt+Enter toggles fullscreen on the primary window, whatever
            // the menu or the game is doing; the key never reaches either.
            if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
                sdl_event_alt(event)) {
                const SDL_Scancode alt_key = sdl_event_scancode(event);
                if (alt_key == SDL_SCANCODE_RETURN ||
                    alt_key == SDL_SCANCODE_KP_ENTER) {
                    if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                        presentation.windows[0]) {
                        SDL_Window* window = presentation.windows[0];
                        const auto window_flags = SDL_GetWindowFlags(window);
#if defined(NDS_HAVE_SDL3)
                        const bool is_fullscreen =
                            (window_flags & SDL_WINDOW_FULLSCREEN) != 0;
#else
                        const bool is_fullscreen =
                            (window_flags & (SDL_WINDOW_FULLSCREEN |
                                             SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
#endif
                        const NdsFullscreenMode mode =
                            is_fullscreen ? NdsFullscreenMode::Off
                                          : last_fullscreen_mode;
                        if (set_window_fullscreen(window, mode)) {
                            // Remembered across launches, like the size.
                            options.fullscreen = mode;
                            persist_fullscreen(options);
                        }
                    }
                    continue;
                }
            }
            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                RecompRuntimeUiInput menu_input{};
                const SDL_Scancode scancode = sdl_event_scancode(event);
                const bool was_open = runtime_menu_open();
                const bool known = runtime_menu_key_input(
                    scancode, &menu_input);
                if (known && runtime_ui &&
                    recomp_runtime_ui_handle_input(
                        runtime_ui, menu_input, event.type == SDL_KEYDOWN,
                        event.key.repeat)) {
                    finish_runtime_menu_input(was_open);
                    continue;
                }
                if (was_open) {
                    finish_runtime_menu_input(was_open);
                    continue;
                }
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const SDL_Scancode scancode = sdl_event_scancode(event);
                NdsSavestateSlotCommand state_command{};
                const unsigned function_key = savestate_function_key(scancode);
                const bool state_shortcut = nds_savestate_slot_shortcut(
                    function_key, sdl_event_shift(event), false,
                    &state_command);
                if (state_shortcut) {
                    if (state_command.action == NdsSavestateSlotAction::Load)
                        finish_pending_save(true);
                    const NdsSavestateSlotResult result =
                        state_command.action == NdsSavestateSlotAction::Save ?
                        nds_savestate_slot_request_save(
                            options.savestate_directory,
                            {options.savestate_build_id, options.savestate_rom_sha1},
                            state_command) :
                        nds_savestate_slot_execute(
                            options.savestate_directory,
                            {options.savestate_build_id, options.savestate_rom_sha1},
                            state_command);
                    show_savestate_result(result);
                    if (result.success &&
                        state_command.action ==
                            NdsSavestateSlotAction::Load) {
                        // gpu3d_savestate_import() clears the readback-latency
                        // flag as part of installing a fresh renderer, so the
                        // live stage has to be re-established. Skipped once the
                        // governor is terminally disabled: the installed state
                        // is then whatever survived the failure.
                        if (!perf_governor.apply_failed &&
                            options.perf_governor_mode !=
                                NdsPerfGovernorMode::Off &&
                            !apply_performance_governor_stage(
                                presentation, perf_governor.stage,
                                requested_internal_scale)) {
                            nds_perf_governor_mark_apply_failed(
                                &perf_governor);
                            nds_perf_governor_record_transition(
                                perf_governor.stage, perf_governor.stage,
                                NdsPerfGovernorReason::ApplyFailed,
                                shown_frames, perf_governor.stage2_held,
                                true);
                            nds_diagnostics_note_perf_governor_transition(
                                perf_governor.stage, perf_governor.stage,
                                "apply_failed");
                        }
                        blend_cache.valid = false;
                        // A load re-primes the machine, so it also opens a new
                        // digest epoch: it is the only alignment point two
                        // independently launched processes share, since
                        // shown_frames at load time is wall-clock dependent.
                        ++g_frame_digest_epoch;
                        // Guest input registers came from historical state;
                        // the live host controls become authoritative again
                        // before the restored machine executes a round.
                        publish_keys();
                        if (mouse_down) {
                            set_touch_from_mouse(
                                static_cast<float>(last_touch_event_x),
                                static_cast<float>(last_touch_event_y),
                                true, options.screen_layout,
                                bottom_logical_width);
                        } else {
                            nds_set_touch(0, 0, false);
                        }
                    }
                } else if (scancode == SDL_SCANCODE_ESCAPE) {
                    if (relative_mouse.captured() || virtual_stylus_active())
                        release_relative_mouse();
                    else
                        running = false;
                } else if (process_virtual_stylus_key(scancode, true)) {
                    // Consumed by the generic lower-screen pointer helper.
                } else if (presentation.single &&
                           binding_matches_key(screen_toggle_binding,
                                               scancode)) {
                    screen_toggled = !screen_toggled;
                    std::fprintf(stderr, "[display] showing the %s screen\n",
                                 screen_toggled ? "other" : "main");
                } else if (process_mph_prime_key(
                               scancode, true, false)) {
                    // Consumed by the MPH-specific keyboard/mouse layer.
                } else if (options.tab_turbo &&
                           scancode == SDL_SCANCODE_TAB) {
                    turbo_pressed = true;
                } else if (mph_prime_active()) {
                    // Prime Controls replaces the normal keyboard keypad map;
                    // unbound keys must not leak through as DS buttons.
                } else if (const uint16_t bit = nds_binding_mask(options.keyboard_bindings, scancode)) {
                    ++host_key_presses;
                    keyboard_pressed |= bit;
                    publish_keys();
                } else if (is_shift_key(scancode)) {
                    // Hold Shift to fast-forward, unless the user bound Shift
                    // to a DS button above.
                    turbo_pressed = true;
                } else if (scancode == SDL_SCANCODE_M) {
                    // Marks the moment in an NDS_GX_CMD_TRACE (smooth_3d
                    // debugging); nothing without one.
                    nds_gpu3d_mark_interp();
                }
            }
            if (event.type == SDL_KEYUP && !event.key.repeat) {
                const SDL_Scancode scancode = sdl_event_scancode(event);
                if (process_virtual_stylus_key(scancode, false)) {
                    // Consumed by the generic lower-screen pointer helper.
                } else if (process_mph_prime_key(
                        scancode, false, false)) {
                    // Consumed by the MPH-specific keyboard/mouse layer.
                } else if (options.tab_turbo &&
                           scancode == SDL_SCANCODE_TAB) {
                    turbo_pressed = false;
                } else if (mph_prime_active()) {
                    // See keydown path: ignore generic keyboard bindings
                    // while the Prime Controls capture owns the keyboard.
                } else if (const uint16_t bit = nds_binding_mask(options.keyboard_bindings, scancode)) {
                    keyboard_pressed &= static_cast<uint16_t>(~bit);
                    publish_keys();
                } else if (is_shift_key(scancode)) {
                    turbo_pressed = false;
                }
            }
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                controller =
                    SDL_GameControllerOpen(sdl_controller_device_id(event));
                if (controller) {
                    controller_id = sdl_controller_id(controller);
                    std::fprintf(stderr,
                                 "[sdl] Player 1 controller: %s\n",
                                 SDL_GameControllerName(controller));
                }
            }
            if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
                controller && sdl_controller_device_id(event) == controller_id) {
                pad_back_held = false;
                pad_start_held = false;
                SDL_GameControllerClose(controller);
                controller = nullptr;
                controller_id = -1;
                controller_pressed = 0;
                // Pad-held Prime actions must not survive the device; the
                // keyboard/mouse can re-press theirs on the next event.
                clear_mph_prime_controls();
                clear_virtual_stylus();
                publish_keys();
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN ||
                event.type == SDL_CONTROLLERBUTTONUP) {
                RecompRuntimeUiInput menu_input{};
                const bool was_open = runtime_menu_open();
                const auto button = static_cast<SDL_GameControllerButton>(
                    sdl_controller_button(event));
                const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
                if (button == SDL_CONTROLLER_BUTTON_BACK)
                    pad_back_held = down;
                if (button == SDL_CONTROLLER_BUTTON_START)
                    pad_start_held = down;
                if (!was_open) {
                    if (down && pad_back_held && pad_start_held && runtime_ui &&
                        recomp_runtime_ui_handle_input(
                            runtime_ui, RECOMP_RUNTIME_UI_INPUT_TOGGLE, 1, 0)) {
                        finish_runtime_menu_input(was_open);
                        continue;
                    }
                } else if (runtime_menu_controller_input(button, &menu_input) &&
                           runtime_ui &&
                           recomp_runtime_ui_handle_input(
                               runtime_ui, menu_input, down, false)) {
                    finish_runtime_menu_input(was_open);
                    continue;
                }
                if (was_open) {
                    finish_runtime_menu_input(was_open);
                    continue;
                }
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                const auto button = static_cast<SDL_GameControllerButton>(
                    sdl_controller_button(event));
                if (process_virtual_stylus_pad(MphPadInputKind::Button,
                                               button, true)) {
                    // Consumed by the generic lower-screen pointer helper.
                } else if (process_mph_prime_pad(MphPadInputKind::Button, button,
                                          true)) {
                    // Consumed by Prime Controls.
                } else if (const uint16_t bit =
                        gamepad_button_mask(options, button)) {
                    controller_pressed |= bit;
                    publish_keys();
                }
            }
            if (event.type == SDL_CONTROLLERBUTTONUP) {
                const auto button = static_cast<SDL_GameControllerButton>(
                    sdl_controller_button(event));
                if (process_virtual_stylus_pad(MphPadInputKind::Button,
                                               button, false)) {
                    // Consumed by the generic lower-screen pointer helper.
                } else if (process_mph_prime_pad(MphPadInputKind::Button, button,
                                          false)) {
                    // Consumed by Prime Controls.
                } else if (const uint16_t bit =
                        gamepad_button_mask(options, button)) {
                    controller_pressed &= static_cast<uint16_t>(~bit);
                    publish_keys();
                }
            }
            if (runtime_menu_open() &&
                (event.type == SDL_MOUSEBUTTONDOWN ||
                 event.type == SDL_MOUSEBUTTONUP ||
                 event.type == SDL_MOUSEMOTION)) {
                continue;
            }
            // Wheel up/down raises/lowers a title's zoom value (title_settings.h).
            if (event.type == SDL_MOUSEWHEEL && !runtime_menu_open()) {
                // Scroll up lowers the value (zooms in), down raises it.
                const float wheel = static_cast<float>(event.wheel.y) *
                    (event.wheel.direction == 1 ? -1.0f : 1.0f);
                // 50 units (5%) per notch, smaller for a hi-res wheel's fractions.
                int delta = static_cast<int>(std::lround(-wheel * 50.0f));
                if (delta == 0 && wheel != 0.0f) delta = wheel > 0.0f ? -1 : 1;
                if (nds_title_zoom_add(delta))
                    runtime_menu_save(&runtime_menu_state);
            }
            if ((event.type == SDL_MOUSEBUTTONDOWN ||
                 event.type == SDL_MOUSEBUTTONUP) &&
                event.button.button == SDL_BUTTON_RIGHT) {
                nds_title_set_right_mouse_held(
                    event.type == SDL_MOUSEBUTTONDOWN);
            }
            const bool primary_left_down =
                event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[0];
            const bool bottom_left_down =
                event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                presentation.separate &&
                event.button.windowID == presentation.window_ids[1];
            const bool relative_left_down =
                primary_left_down ||
                (bottom_left_down && mph_prime_unified_window_focus);
            const NdsStackedRelativeMouseRoute stacked_left_route =
                primary_left_down && !presentation.separate &&
                        !presentation.single
                    ? nds_route_stacked_relative_mouse_button(
                        options.relative_mouse_touch,
                        relative_mouse.captured(), stacked_top_screen,
                        stacked_bottom_touch, event.button.x, event.button.y)
                    : NdsStackedRelativeMouseRoute::None;
            bool virtual_stylus_mouse_consumed = false;
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                is_presentation_window(event.button.windowID)) {
                if (process_virtual_stylus_mouse_hold(
                        event.button.button, true)) {
                    virtual_stylus_mouse_consumed = true;
                } else if (process_virtual_stylus_mouse_tap(
                               event.button.button, true)) {
                    virtual_stylus_mouse_consumed = true;
                } else if (event.button.button == SDL_BUTTON_LEFT &&
                           virtual_stylus_active()) {
                    virtual_stylus_tap_held = true;
                    virtual_stylus_mouse_consumed = true;
                }
            }
            if (!virtual_stylus_mouse_consumed &&
                relative_left_down && options.relative_mouse_touch &&
                (presentation.separate ||
                 stacked_left_route ==
                     NdsStackedRelativeMouseRoute::AcquireRelative ||
                 stacked_left_route ==
                     NdsStackedRelativeMouseRoute::CapturedButton)) {
                if (!relative_mouse.captured()) {
                    // The acquisition click only captures; the next click is
                    // the first guest fire press, avoiding an accidental shot.
                    capture_relative_mouse();
                } else if (process_mph_prime_mouse(
                               event.button.button, true, false)) {
                    // Consumed by Prime Controls.
                } else if (options.relative_mouse_fire_mask != 0) {
                    mouse_pressed |= options.relative_mouse_fire_mask;
                    publish_keys();
                }
            }
            if (!virtual_stylus_mouse_consumed &&
                event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button != SDL_BUTTON_LEFT &&
                (event.button.windowID == presentation.window_ids[0] ||
                 (mph_prime_unified_window_focus &&
                  is_presentation_window(event.button.windowID))) &&
                process_mph_prime_mouse(event.button.button, true, false)) {
                // Consumed by Prime Controls.
            }
            if (event.type == SDL_MOUSEBUTTONUP &&
                is_presentation_window(event.button.windowID)) {
                if (process_virtual_stylus_mouse_hold(
                        event.button.button, false)) {
                    virtual_stylus_mouse_consumed = true;
                } else if (process_virtual_stylus_mouse_tap(
                               event.button.button, false)) {
                    virtual_stylus_mouse_consumed = true;
                } else if (event.button.button == SDL_BUTTON_LEFT &&
                           virtual_stylus_tap_held) {
                    virtual_stylus_tap_held = false;
                    virtual_stylus_mouse_consumed = true;
                }
            }
            if (!virtual_stylus_mouse_consumed &&
                event.type == SDL_MOUSEBUTTONUP &&
                (event.button.windowID == presentation.window_ids[0] ||
                 (mph_prime_unified_window_focus &&
                  is_presentation_window(event.button.windowID))) &&
                relative_mouse.captured()) {
                if (process_mph_prime_mouse(
                        event.button.button, false, false)) {
                    // Consumed by Prime Controls.
                } else if (event.button.button == SDL_BUTTON_LEFT &&
                           mouse_pressed != 0) {
                    mouse_pressed &= static_cast<uint16_t>(
                        ~options.relative_mouse_fire_mask);
                    publish_keys();
                }
            }
            if (!virtual_stylus_mouse_consumed &&
                event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[1] &&
                !(mph_prime_unified_window_focus &&
                  relative_mouse.captured()) &&
                (presentation.separate || presentation.single
                    ? (!presentation.single ||
                       presentation.shown_screen == 1 ||
                       presentation.inset_percent > 0) &&
                          event.button.x >= bottom_content_left &&
                          event.button.x < bottom_content_left + kScreenWidth &&
                          event.button.y >= 0 &&
                          event.button.y < kScreenHeight
                    : stacked_left_route ==
                          NdsStackedRelativeMouseRoute::Touchscreen)) {
                mouse_down = true;
                ++host_touch_presses;
                last_touch_event_x = event.button.x;
                last_touch_event_y = event.button.y;
                touch_release_pending = false;
                touch_frames_held = 0;
                set_touch_from_mouse(event.button.x, event.button.y, true,
                                     options.screen_layout,
                                     bottom_logical_width);
            }
            if (!virtual_stylus_mouse_consumed &&
                event.type == SDL_MOUSEBUTTONUP &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.windowID == presentation.window_ids[1] &&
                mouse_down) {
                mouse_down = false;
                // A host click can begin and end while the emulation thread is
                // rendering one slow frame. Keep such a click asserted long
                // enough for the ARM7 touchscreen polling path to observe it.
                if (touch_frames_held < 2)
                    touch_release_pending = true;
                else
                    set_touch_from_mouse(event.button.x, event.button.y, false,
                                         options.screen_layout,
                                         bottom_logical_width);
            }
            if (event.type == SDL_MOUSEMOTION && mouse_down &&
                event.motion.windowID == presentation.window_ids[1])
                set_touch_from_mouse(event.motion.x, event.motion.y, true,
                                     options.screen_layout,
                                     bottom_logical_width);
            // The pointer over the touchscreen with no button held, for
            // titles that highlight the menu entry under it.
            if (event.type == SDL_MOUSEMOTION ||
                event.type == SDL_MOUSEBUTTONDOWN ||
                event.type == SDL_MOUSEBUTTONUP) {
                const bool motion = event.type == SDL_MOUSEMOTION;
                const uint32_t window = motion ? event.motion.windowID
                                               : event.button.windowID;
                const bool touchscreen_shown =
                    !presentation.single || presentation.shown_screen == 1 ||
                    presentation.inset_percent > 0;
                const bool hovering =
                    window == presentation.window_ids[1] &&
                    touchscreen_shown && !mouse_down &&
                    !touch_release_pending && !relative_mouse.captured() &&
                    !virtual_stylus_active() &&
                    (motion ? event.motion.state == 0
                            : event.type == SDL_MOUSEBUTTONUP &&
                                  SDL_GetMouseState(nullptr, nullptr) == 0);
                set_hover_from_mouse(
                    motion ? event.motion.x : event.button.x,
                    motion ? event.motion.y : event.button.y, hovering,
                    options.screen_layout, bottom_logical_width);
            }
            // The pointer over the top screen, which the guest never polls
            // as a touchscreen: titles read it to make the top screen
            // hoverable and clickable. Single layout shows it only when it
            // is the shown screen and not forced stacked with the other.
            if (event.type == SDL_MOUSEMOTION ||
                event.type == SDL_MOUSEBUTTONDOWN ||
                event.type == SDL_MOUSEBUTTONUP) {
                const bool motion = event.type == SDL_MOUSEMOTION;
                const uint32_t window = motion ? event.motion.windowID
                                               : event.button.windowID;
                const bool top_shown =
                    window == presentation.window_ids[0] &&
                    g_top_logical_hit.valid &&
                    !relative_mouse.captured() && !virtual_stylus_active() &&
                    (!presentation.single ||
                     (!presentation.stacked &&
                      (presentation.shown_screen == 0 ||
                       presentation.inset_percent > 0)));
                int top_x = 0, top_y = 0;
                const int top_left =
                    (presentation.screen_widths[0] - kScreenWidth) / 2 +
                    (presentation.separate || presentation.single
                         ? 0 : top_content_left);
                const bool over = top_shown &&
                    top_screen_point(g_top_logical_hit.x,
                                     g_top_logical_hit.y, top_left,
                                     &top_x, &top_y);
                nds_title_set_top_pointer(top_x, top_y, over);
                if (over && event.type == SDL_MOUSEBUTTONDOWN &&
                    event.button.button == SDL_BUTTON_LEFT)
                    nds_title_note_top_click(top_x, top_y);
            }
            if (event.type == SDL_MOUSEMOTION &&
                virtual_stylus_active() &&
                (event.motion.windowID == presentation.window_ids[0] ||
                 is_presentation_window(event.motion.windowID) ||
                 event.motion.windowID == 0)) {
                mph_virtual_x +=
                    static_cast<float>(event.motion.xrel) *
                    options.virtual_stylus.sensitivity * 0.01f;
                mph_virtual_y +=
                    static_cast<float>(event.motion.yrel) *
                    (256.0f / 192.0f) *
                    options.virtual_stylus.sensitivity * 0.01f;
                mph_virtual_x = std::clamp(mph_virtual_x, 0.0f, 255.0f);
                mph_virtual_y = std::clamp(mph_virtual_y, 0.0f, 191.0f);
            } else if (event.type == SDL_MOUSEMOTION &&
                relative_mouse.captured() &&
                (event.motion.windowID == presentation.window_ids[0] ||
                 (mph_prime_unified_window_focus &&
                  is_presentation_window(event.motion.windowID)) ||
                 event.motion.windowID == 0)) {
                const bool prime_virtual_stylus = mph_prime_active() &&
                    mph_prime_held[static_cast<size_t>(
                        MphPrimeAction::VirtualStylus)];
                if (prime_virtual_stylus) {
                    mph_virtual_x +=
                        static_cast<float>(event.motion.xrel) *
                        options.mph_virtual_stylus_sensitivity * 0.01f;
                    mph_virtual_y +=
                        static_cast<float>(event.motion.yrel) *
                        (256.0f / 192.0f) *
                        options.mph_virtual_stylus_sensitivity * 0.01f;
                    mph_virtual_x = std::clamp(mph_virtual_x, 0.0f, 255.0f);
                    mph_virtual_y = std::clamp(mph_virtual_y, 0.0f, 191.0f);
                } else if (options.relative_mouse_direct_aim) {
                    relative_delta_x += event.motion.xrel;
                    relative_delta_y += event.motion.yrel;
                } else if (relative_mouse.move(event.motion.xrel,
                                               event.motion.yrel)) {
                    nds_set_touch(relative_mouse.x(), relative_mouse.y(), true);
                }
            }
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_MOUSE_LEAVE) &&
                mouse_down &&
                event.window.windowID == presentation.window_ids[1]) {
                mouse_down = false;
                if (touch_frames_held < 2)
                    touch_release_pending = true;
                else
                    nds_set_touch(0, 0, false);
            }
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_MOUSE_LEAVE) &&
                event.window.windowID == presentation.window_ids[1])
                nds_title_set_pointer_hover(0, 0, false);
            if (sdl_window_event_is(event, SDL_EVENT_WINDOW_MOUSE_LEAVE) &&
                event.window.windowID == presentation.window_ids[0])
                nds_title_set_top_pointer(0, 0, false);
        }
        if (focus_release_pending) {
            if (!any_presentation_window_focused()) {
                release_relative_mouse();
                clear_tab_turbo();
            }
            focus_release_pending = false;
        }

        // ── Gamepad dual-stick poll (once per shown frame) ───────────────
        mph_pad_frame_x = 0;
        mph_pad_frame_y = 0;
        // The raw left stick for title hooks, while it drives the D-pad.
        if (!controller || runtime_menu_open()) {
            nds_title_set_left_stick(0, 0, false);
        } else {
            nds_title_set_left_stick(
                SDL_GameControllerGetAxis(controller,
                                          SDL_CONTROLLER_AXIS_LEFTX),
                SDL_GameControllerGetAxis(controller,
                                          SDL_CONTROLLER_AXIS_LEFTY),
                true);
        }
        if (controller) {
            // Left stick -> D-pad, with hysteresis so a stick resting near
            // a threshold never flickers a direction.
            const float lx = SDL_GameControllerGetAxis(
                controller, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            const float ly = SDL_GameControllerGetAxis(
                controller, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            const bool menu_open = runtime_menu_open();
            if (menu_open) {
                if (stick_pressed != 0) {
                    stick_pressed = 0;
                    publish_keys();
                }
                bool have_stick_input = false;
                RecompRuntimeUiInput stick_input =
                    RECOMP_RUNTIME_UI_INPUT_DOWN;
                const float ax = std::fabs(lx);
                const float ay = std::fabs(ly);
                if (std::max(ax, ay) > 0.5f) {
                    have_stick_input = true;
                    if (ax > ay)
                        stick_input = lx > 0.0f
                            ? RECOMP_RUNTIME_UI_INPUT_RIGHT
                            : RECOMP_RUNTIME_UI_INPUT_LEFT;
                    else
                        stick_input = ly > 0.0f
                            ? RECOMP_RUNTIME_UI_INPUT_DOWN
                            : RECOMP_RUNTIME_UI_INPUT_UP;
                }
                if (have_stick_input) {
                    const bool same = runtime_stick_held &&
                                      runtime_stick_input == stick_input;
                    if (!same) {
                        runtime_stick_held = true;
                        runtime_stick_input = stick_input;
                        runtime_stick_frames = 0;
                        recomp_runtime_ui_handle_input(
                            runtime_ui, stick_input, 1, 0);
                    } else if (++runtime_stick_frames >= 18 &&
                               ((runtime_stick_frames - 18) % 5) == 0) {
                        recomp_runtime_ui_handle_input(
                            runtime_ui, stick_input, 1, 1);
                    }
                } else {
                    runtime_stick_held = false;
                    runtime_stick_frames = 0;
                }
            } else {
                runtime_stick_held = false;
                runtime_stick_frames = 0;
                auto stick_dir = [&](float value, uint16_t bit, bool positive) {
                    const float v = positive ? value : -value;
                    const bool held = (stick_pressed & bit) != 0;
                    const bool next = v > (held ? 0.35f : 0.5f);
                    if (next != held) {
                        if (next) stick_pressed |= bit;
                        else stick_pressed &= static_cast<uint16_t>(~bit);
                        publish_keys();
                    }
                };
                stick_dir(lx, 1u << 4, true);    // Right
                stick_dir(lx, 1u << 5, false);   // Left
                stick_dir(ly, 1u << 6, false);   // Up (SDL Y axis points down)
                stick_dir(ly, 1u << 7, true);    // Down
            }

            // Right stick up/down -> a title's zoom (title_settings.h): up
            // raises the value, down lowers it, at a speed that follows the
            // deflection (up to 15 units, 1.5%, a frame). The stick belongs to the
            // virtual stylus while it is held (LT), to Prime's aim, and to
            // the runtime menu while that is open. The settings are saved
            // when the stick is released, and every half second meanwhile.
            {
                float ry = 0.0f;
                if (!menu_open && !virtual_stylus_active() &&
                    !mph_prime_controls_available) {
                    ry = SDL_GameControllerGetAxis(
                        controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
                }
                constexpr float kZoomDeadzone = 0.25f;
                const float mag = std::fabs(ry);
                if (mag > kZoomDeadzone) {
                    const float curved =
                        (mag - kZoomDeadzone) / (1.0f - kZoomDeadzone);
                    // Stick up (negative Y) raises the value.
                    zoom_stick_rem += (ry < 0.0f ? 1.0f : -1.0f) * curved *
                                      15.0f;
                    const int delta = static_cast<int>(zoom_stick_rem);
                    zoom_stick_rem -= static_cast<float>(delta);
                    if (nds_title_zoom_add(delta)) zoom_save_pending = true;
                    if (zoom_save_pending && ++zoom_stick_frames >= 30) {
                        zoom_stick_frames = 0;
                        zoom_save_pending = false;
                        runtime_menu_save(&runtime_menu_state);
                    }
                } else {
                    zoom_stick_rem = 0.0f;
                    zoom_stick_frames = 0;
                    if (zoom_save_pending) {
                        zoom_save_pending = false;
                        runtime_menu_save(&runtime_menu_state);
                    }
                }
            }

            if (!menu_open && virtual_stylus_available) {
                const float rx = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
                const float ry = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
                const bool trigger_right = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 9830;
                const bool trigger_left = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 9830;
                if (trigger_right != virtual_stylus_pad_trigger_right_held) {
                    virtual_stylus_pad_trigger_right_held = trigger_right;
                    process_virtual_stylus_pad(MphPadInputKind::TriggerRight,
                                              SDL_CONTROLLER_BUTTON_INVALID,
                                              trigger_right);
                }
                if (trigger_left != virtual_stylus_pad_trigger_left_held) {
                    virtual_stylus_pad_trigger_left_held = trigger_left;
                    process_virtual_stylus_pad(MphPadInputKind::TriggerLeft,
                                              SDL_CONTROLLER_BUTTON_INVALID,
                                              trigger_left);
                }
                const float mag = std::sqrt(rx * rx + ry * ry);
                constexpr float kVirtualStylusDeadzone = 0.25f;
                if (virtual_stylus_active() && mag > kVirtualStylusDeadzone) {
                    const float curved =
                        (mag - kVirtualStylusDeadzone) /
                        (1.0f - kVirtualStylusDeadzone);
                    const float rate = curved * curved * 5.0f *
                        (options.virtual_stylus.pad_sensitivity / 100.0f) /
                        mag;
                    virtual_stylus_pad_rem_x += rx * rate;
                    virtual_stylus_pad_rem_y += ry * rate *
                        (256.0f / 192.0f);
                    const int32_t pad_x =
                        static_cast<int32_t>(virtual_stylus_pad_rem_x);
                    const int32_t pad_y =
                        static_cast<int32_t>(virtual_stylus_pad_rem_y);
                    virtual_stylus_pad_rem_x -= static_cast<float>(pad_x);
                    virtual_stylus_pad_rem_y -= static_cast<float>(pad_y);
                    mph_virtual_x = std::clamp(
                        mph_virtual_x + static_cast<float>(pad_x),
                        0.0f, 255.0f);
                    mph_virtual_y = std::clamp(
                        mph_virtual_y + static_cast<float>(pad_y),
                        0.0f, 191.0f);
                }
            }

            if (!menu_open && mph_prime_controls_available) {
                // Right stick -> camera aim; triggers act as bindable
                // pseudo-buttons (defaults: RT shoot, LT scan-fire).
                const float rx = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
                const float ry = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
                const bool trigger_right = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 9830;
                const bool trigger_left = SDL_GameControllerGetAxis(
                    controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 9830;
                if (trigger_right != mph_pad_trigger_right_held) {
                    mph_pad_trigger_right_held = trigger_right;
                    process_mph_prime_pad(MphPadInputKind::TriggerRight,
                                          SDL_CONTROLLER_BUTTON_INVALID,
                                          trigger_right);
                }
                if (trigger_left != mph_pad_trigger_left_held) {
                    mph_pad_trigger_left_held = trigger_left;
                    process_mph_prime_pad(MphPadInputKind::TriggerLeft,
                                          SDL_CONTROLLER_BUTTON_INVALID,
                                          trigger_left);
                }
                const float mag = std::sqrt(rx * rx + ry * ry);
                constexpr float kDeadzone = 0.25f;
                const bool aiming = mag > kDeadzone;
                if (aiming || trigger_right || trigger_left) {
                    mph_pad_idle_frames = 0;
                    if (!mph_prime_pad_engaged) {
                        mph_prime_pad_engaged = true;
                        std::fprintf(stderr,
                            "[sdl] MPH pad aim engaged (right stick / "
                            "triggers); idles out when released\n");
                    }
                } else if (mph_prime_pad_engaged &&
                           !relative_mouse.captured() &&
                           ++mph_pad_idle_frames > 45) {
                    mph_prime_pad_engaged = false;
                    nds_set_touch(0, 0, false);
                }
                if (aiming && mph_prime_pad_engaged) {
                    // Square-law response: fine aim near center, a full
                    // deflection turns at the built-in rate scaled by the
                    // pad sensitivity. Y keeps the mouse path's 150% scale
                    // and follows the same invert option.
                    const float curved =
                        (mag - kDeadzone) / (1.0f - kDeadzone);
                    const float rate = curved * curved * 5.0f *
                        (options.mph_pad_aim_sensitivity / 100.0f) / mag;
                    mph_pad_aim_rem_x += rx * rate;
                    mph_pad_aim_rem_y += ry * rate * 1.5f *
                        (options.relative_mouse_invert_y ? -1.0f : 1.0f);
                    mph_pad_frame_x =
                        static_cast<int32_t>(mph_pad_aim_rem_x);
                    mph_pad_frame_y =
                        static_cast<int32_t>(mph_pad_aim_rem_y);
                    mph_pad_aim_rem_x -= static_cast<float>(mph_pad_frame_x);
                    mph_pad_aim_rem_y -= static_cast<float>(mph_pad_frame_y);
                }
            }
        } else {
            runtime_stick_held = false;
            runtime_stick_frames = 0;
            if (stick_pressed != 0) {
                stick_pressed = 0;
                publish_keys();
            }
            if (mph_pad_trigger_right_held) {
                mph_pad_trigger_right_held = false;
                process_mph_prime_pad(MphPadInputKind::TriggerRight,
                                      SDL_CONTROLLER_BUTTON_INVALID, false);
            }
            if (mph_pad_trigger_left_held) {
                mph_pad_trigger_left_held = false;
                process_mph_prime_pad(MphPadInputKind::TriggerLeft,
                                      SDL_CONTROLLER_BUTTON_INVALID, false);
            }
            if (virtual_stylus_pad_trigger_right_held) {
                virtual_stylus_pad_trigger_right_held = false;
                process_virtual_stylus_pad(MphPadInputKind::TriggerRight,
                                          SDL_CONTROLLER_BUTTON_INVALID,
                                          false);
            }
            if (virtual_stylus_pad_trigger_left_held) {
                virtual_stylus_pad_trigger_left_held = false;
                process_virtual_stylus_pad(MphPadInputKind::TriggerLeft,
                                          SDL_CONTROLLER_BUTTON_INVALID,
                                          false);
            }
            if (mph_prime_pad_engaged) {
                mph_prime_pad_engaged = false;
                if (!relative_mouse.captured()) nds_set_touch(0, 0, false);
            }
            if (virtual_stylus_pad_held) clear_virtual_stylus();
        }

        const bool mph_prime_is_active = mph_prime_active();
        const bool mph_prime_virtual_stylus = mph_prime_is_active &&
            mph_prime_held[static_cast<size_t>(
                MphPrimeAction::VirtualStylus)];
        const bool generic_virtual_stylus = virtual_stylus_active();

        const bool turbo_want = turbo_pressed || nds_debug_turbo();
        if (turbo_want != turbo_active) {
            turbo_active = turbo_want;
            if (audio) {
                pause_audio(audio, true);
                clear_audio_queue(audio, audio_queue);
                audio_queue.started.store(false, std::memory_order_relaxed);
            }
            audio_started = false;
            audio_start_threshold = kAudioQueueFrames;
            audio_pace_floor = audio_start_threshold;
            audio_min_queue = std::numeric_limits<uint32_t>::max();
            audio_max_queue = 0;
            std::fprintf(stderr, "[sdl] turbo %s\n",
                         turbo_active ? "on" : "off");
        }

        if ((relative_mouse.captured() || mph_prime_pad_engaged) &&
            options.relative_mouse_direct_aim &&
            !mph_prime_virtual_stylus &&
            (relative_delta_x != 0 || relative_delta_y != 0 ||
             mph_pad_frame_x != 0 || mph_pad_frame_y != 0)) {
            // AMHE0 consumes signed per-frame aim deltas. Keep the native
            // stylus held at center, but feed motion through those title-owned
            // fields so turning never stops at a virtual touchscreen edge.
            // The pad's right-stick contribution is pre-scaled and merges
            // with the mouse counts here because the title fields are
            // OVERWRITTEN per frame, not accumulated.
            const NdsRelativeMouseDelta delta =
                nds_scale_relative_mouse_delta(
                    relative_delta_x, relative_delta_y,
                    options.relative_mouse_sensitivity,
                    options.relative_mouse_invert_y, 150);
            const int32_t final_x = delta.x + mph_pad_frame_x;
            const int32_t final_y = delta.y + mph_pad_frame_y;
            if (nds_title_patches_apply_mph_mouse_delta(final_x, final_y)) {
                ++relative_direct_writes;
                if (mph_pad_frame_x != 0 || mph_pad_frame_y != 0)
                    ++mph_pad_aim_writes;
            }
            relative_delta_x = 0;
            relative_delta_y = 0;
        }
        if (generic_virtual_stylus) {
            // A single-screen peek at the top LCD hides the touchscreen, so
            // taps do not reach it.
            const bool stylus_on_touchscreen =
                !presentation.single || presentation.shown_screen == 1;
            nds_set_touch(static_cast<uint16_t>(std::lround(mph_virtual_x)),
                          static_cast<uint16_t>(std::lround(mph_virtual_y)),
                          virtual_stylus_tap_held && stylus_on_touchscreen);
        } else if (mph_prime_is_active) {
            if (mph_touch_sequence.active()) {
                mph_touch_sequence.tick();
            } else if (mph_prime_virtual_stylus) {
                const bool touch_down =
                    mph_prime_held[static_cast<size_t>(
                        MphPrimeAction::Shoot)] ||
                    mph_prime_held[static_cast<size_t>(
                        MphPrimeAction::ScanShoot)];
                nds_set_touch(
                    static_cast<uint16_t>(std::lround(mph_virtual_x)),
                    static_cast<uint16_t>(std::lround(mph_virtual_y)),
                    touch_down);
            } else {
                if (nds_title_patches_mph_should_release_touch_for_morph_boost(
                        mph_prime_held[static_cast<size_t>(
                            MphPrimeAction::BoostZoom)]))
                    nds_set_touch(0, 0, false);
                else
                    nds_set_touch(128, 96, true);
            }
        }
        if (options.custom_gamepad_bindings) {
            // Button events handle digital bindings; poll signed axes here.
            // Each row has one source, so clearing an axis row cannot release
            // a different row's button. Keep host pointer/menu ownership.
            uint16_t axis_rows = 0, axis_pressed = 0;
            for (size_t row = 0; row < options.gamepad_bindings.size(); ++row) {
                const int binding = options.gamepad_bindings[row];
                if (binding < 100 || binding > 111) continue;
                axis_rows |= kNdsButtonMasks[row];
                if (!controller || runtime_menu_open() || virtual_stylus_active() ||
                    mph_prime_controls_available) continue;
                const int value = SDL_GameControllerGetAxis(controller,
                    static_cast<decltype(SDL_CONTROLLER_AXIS_LEFTX)>((binding - 100) / 2));
                if ((binding & 1) ? value > 16384 : value < -16384)
                    axis_pressed |= kNdsButtonMasks[row];
            }
            const uint16_t next = (controller_pressed & ~axis_rows) | axis_pressed;
            if (next != controller_pressed) {
                controller_pressed = next;
                publish_keys();
            }
        }
        publish_input_debug();

        // Quit/control requests are resolved while the producer is joined.
        // Never start another guest transaction after SDL_QUIT/window close.
        if (!running) break;
        const uint64_t phase0 = SDL_GetPerformanceCounter();
        uint64_t latency_cycles_emu = latency_trace ? nds_frontend_latency::cycles() : 0;
        uint64_t worker_emu_begin=phase0, worker_emu_end=phase0;
        uint64_t frame_work_ticks=0;
        bool producer_pending=false;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        OwnedPresentationFrame* presented=nullptr;
        int building_slot=-1;
        if (cpu_handoff) {
            if (completed_slot>=0 &&
                (owned_frames[completed_slot].audio_epoch!=nds_spu_presentation_epoch() ||
                 owned_frames[completed_slot].digest_epoch!=g_frame_digest_epoch ||
                 owned_frames[completed_slot].gl.scale!=nds_gpu3d_internal_scale()))
                completed_slot=-1;
            building_slot=completed_slot<0 ? 0 : 1-completed_slot;
            auto& packet=owned_frames[building_slot];
            packet.job=++produced_jobs; packet.digest_epoch=g_frame_digest_epoch;
            cpu_handoff->Start([&,building_slot] {
                // SDL changes the CALLING thread's priority. Do not assume the
                // main thread's existing HIGH setting is inherited by pthreads.
                if (!producer_priority_initialized) {
                    if (!sdl_set_thread_priority_high())
                        std::fprintf(stderr,"[sdl] CPU producer priority unchanged: %s\n",SDL_GetError());
                    producer_priority_initialized=true;
                }
                auto& output=owned_frames[building_slot];
                output.emu_begin=SDL_GetPerformanceCounter();
                output.cycles_begin=latency_trace ? nds_frontend_latency::cycles() : 0;
                const uint64_t now=scheduler_system_timestamp();
                const uint64_t end=(now/kSystemCyclesPerFrame+1u)*kSystemCyclesPerFrame;
                while (scheduler_system_timestamp()<end &&
                       !(scheduler_cpu_terminal_halted(0) && scheduler_cpu_terminal_halted(1)))
                    scheduler_run_round();
                output.cycles_end=latency_trace ? nds_frontend_latency::cycles() : 0;
                output.emu_end=SDL_GetPerformanceCounter();
                output.Capture(options,presentation.gl_top);
            });
            producer_pending=true;
            if (completed_slot<0) {
                try { cpu_handoff->Join(); }
                catch (const std::exception& e) {
                    std::fprintf(stderr,"[sdl] CPU producer failed: %s\n",e.what());
                    compute_failed=true; running=false; break;
                }
                producer_pending=false; completed_slot=building_slot;
                // Prime one completed packet without displaying it twice. The
                // next loop samples controls for the next CPU frame, then
                // presents this completed job while that producer runs.
                if (!packet.valid) {
                    compute_failed=!packet.powered_off; running=false; break;
                }
                if (mouse_down || touch_release_pending) ++touch_frames_held;
                if (touch_release_pending && touch_frames_held>=2) {
                    nds_set_touch(0,0,false); touch_release_pending=false;
                }
                continue;
            }
            presented=&owned_frames[completed_slot];
            if (!presented->valid) {
                compute_failed=!presented->powered_off; running=false; break;
            }
        } else
#endif
        {
            const uint64_t now = scheduler_system_timestamp();
            const uint64_t next_frame =
                (now / kSystemCyclesPerFrame + 1u) * kSystemCyclesPerFrame;
            while (running && scheduler_system_timestamp() < next_frame &&
                   !(scheduler_cpu_terminal_halted(0) && scheduler_cpu_terminal_halted(1)))
                scheduler_run_round();
        }
        // A real DS power-off is an application lifecycle request, not a
        // debuggable terminal halt. Leave before presenting the powered-down
        // black framebuffer; main() performs durable state flushes.
        if (!cpu_handoff && nds_powered_off()) {
            std::fprintf(stderr,
                         "[sdl] guest requested power-off; closing\n");
            running = false;
            break;
        }
        uint64_t frame_emu_ticks = 0;
        {
            frame_emu_ticks = cpu_handoff ? 0 : SDL_GetPerformanceCounter() - phase0;
            phase_emu_ticks += frame_emu_ticks;
            if (frame_emu_ticks > max_emu_ticks) {
                max_emu_ticks = frame_emu_ticks;
                max_emu_frame = shown_frames;
            }
            if (frame_emu_ticks * 1000u > frequency * 32u)
                ++slow_frames_32ms;
        }
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (!cpu_handoff && nds_gpu3d_compute_runtime_failed()) {
            compute_failed = true;
            running = false;
            break;
        }
#endif
        {
            const uint64_t seen =
                audio_queue.underruns.load(std::memory_order_relaxed);
            if (seen != last_underruns_seen) {
                if (!last_underruns_seen) first_underrun_frame = shown_frames;
                last_underrun_frame = shown_frames;
                last_underruns_seen = seen;
            }
        }

        if (mouse_down || touch_release_pending)
            ++touch_frames_held;
        if (!cpu_handoff && touch_release_pending && touch_frames_held >= 2) {
            nds_set_touch(0, 0, false);
            touch_release_pending = false;
        }

        const uint64_t phase1 = SDL_GetPerformanceCounter();
        uint64_t latency_cycles_present = latency_trace ? nds_frontend_latency::cycles() : 0;
        const uint32_t* native_top=nullptr;
        const uint32_t* top_pixels=nullptr;
        const uint32_t* bottom_pixels=nullptr;
        uint16_t top_width=256,bottom_width=256;
        const uint64_t adaptive_start=SDL_GetPerformanceCounter();
        bool direct_present=false;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (cpu_handoff) {
            native_top=presented->native_top.data();
            top_pixels=presented->pixels[0].data(); bottom_pixels=presented->pixels[1].data();
            top_width=presented->widths[0]; bottom_width=presented->widths[1];
            direct_present=presented->direct;
            phase_adaptive_ticks+=presented->adaptive_ticks;
            observe_top_black_bands(native_top,shown_frames,presented->system_timestamp);
        } else
#endif
        {
            native_top=nds_gpu2d_framebuffer(0);
            top_pixels=native_top; bottom_pixels=nds_gpu2d_framebuffer(1);
            nds_gpu2d_invalidate_hd_frame();
            // The window that consumes HD surfaces: the top one, or in the
            // single-screen layout whichever LCD it shows. gpu2d emits them
            // only while engine A drew that LCD.
            int hd_screen = 0;
            if (presentation.single) {
                if (nds_main_screen_update(
                        main_screen, nds_gpu2d_presented_main_screen(),
                        nds_gpu2d_presented_both_blank())) {
                    std::fprintf(stderr,
                                 "[display] main screen -> %s (frame %llu)\n",
                                 main_screen.main ? "bottom" : "top",
                                 static_cast<unsigned long long>(shown_frames));
                    screen_toggled = false;
                }
                presentation.stacked = nds_title_stacked_layout_wanted();
                nds_stack_vertically_flag().store(
                    presentation.stacked, std::memory_order_relaxed);
                const int shown = presentation.stacked
                    ? 0
                    : nds_single_shown_screen(
                          main_screen.main, screen_toggled,
                          mph_prime_virtual_stylus ||
                              generic_virtual_stylus);
                if (shown != presentation.shown_screen) {
                    // A held mouse touch belongs to the LCD it started on.
                    if (mouse_down) {
                        mouse_down = false;
                        touch_release_pending = false;
                        nds_set_touch(0, 0, false);
                    }
                    presentation.shown_screen = shown;
                }
                hd_screen = shown;
            }
            nds_gpu2d_set_hd_screen(hd_screen);
            const bool native_hd=presentation.gl_top && nds_gpu3d_internal_scale()>1u &&
                                 nds_gpu3d_output_width()==256u;
            direct_present=nds_gpu2d_direct_present_frame_active();
            // Fixed-size layers are split only for a presenter that draws
            // them: the GPU host's window, for engine A's LCD (hd_screen,
            // also at 1x, without HD).
#if defined(NDS_HAVE_COMPUTE_RENDERER)
            nds_gpu2d_set_fixed_layers_consumer(
                presentation.gl_top && nds_compute_host_supports_layer());
#endif
            const bool fixed_layers=nds_gpu2d_fixed_layers_requested();
            if (((options.adaptive_screens & NDS_ADAPTIVE_TOP) ||
                 ((native_hd || fixed_layers) && hd_screen == 0)) && !direct_present)
                top_pixels=nds_gpu2d_adaptive_framebuffer(0,&top_width);
            else if (direct_present) top_width=nds_gpu3d_output_width();
            if ((options.adaptive_screens & NDS_ADAPTIVE_BOTTOM) ||
                ((native_hd || fixed_layers) && hd_screen == 1))
                bottom_pixels=nds_gpu2d_adaptive_framebuffer(1,&bottom_width);
            phase_adaptive_ticks+=SDL_GetPerformanceCounter()-adaptive_start;
            observe_top_black_bands(native_top,shown_frames);
        }
        // At most ONE synthetic frame, presented ahead of the real one, so
        // the visible order stays real(N-1), blend(N-1,N), real(N). No
        // scheduler round, no input sampling, no audio production happens
        // here: this only re-presents pixels that already exist. The direct
        // presenter is excluded above; direct-present frames are excluded
        // again per frame because their top surface lives on the GPU.
        if (interpolation_active && blend_cache.valid && !turbo_active &&
            !direct_present &&
            blend_cache.widths[0] == top_width &&
            blend_cache.widths[1] == bottom_width && audio_started &&
            audio_queue_count(audio, audio_queue) >
                kInterpolationAudioFloorFrames) {
            blend_half(blend_cache.previous[0].data(), top_pixels,
                       blend_cache.blended[0].data(),
                       blend_cache.previous[0].size());
            blend_half(blend_cache.previous[1].data(), bottom_pixels,
                       blend_cache.blended[1].data(),
                       blend_cache.previous[1].size());
            presentation.window_mode = options.window_mode;
            presentation.scaling = options.scaling;
            presentation.resolution_scale = options.resolution_scale;
            presentation.internal_resolution = options.internal_resolution;
            presentation.inset_percent =
                !presentation.single ? 0 : presentation.stacked ? 100
                : options.second_screen_size;
            const PresentationTicks synthetic_ticks = present_screens(
                presentation, blend_cache.blended[0].data(), top_width,
                blend_cache.blended[1].data(), bottom_width,
                runtime_ui,
                mph_prime_virtual_stylus || generic_virtual_stylus,
                mph_virtual_x, mph_virtual_y,
                savestate_notice.empty() ? nullptr : savestate_notice.c_str(),
                options.show_fps ? fps_counter.c_str() : nullptr);
            if (!synthetic_ticks.ok) {
                compute_failed = true;
                running = false;
                break;
            }
            phase_upload_ticks += synthetic_ticks.upload;
            phase_draw_ticks += synthetic_ticks.draw;
            phase_swap_ticks += synthetic_ticks.swap;
            // phase_present_ticks is measured from phase1, which already
            // encloses this block; only the sub-counters need folding in.
            ++synthetic_presents;
            // Events are still consumed at exactly one point, the top of the
            // loop, so input keeps its DS-frame sampling cadence. Pumping
            // here only keeps the OS message queue from backing up across the
            // extra present.
            SDL_PumpEvents();
        }
        // Fast-forward shows the game's own 3D images only: building and
        // drawing the halfway image costs host time that turbo would rather
        // spend running more DS frames, and at turbo speed it is not seen.
        nds_gpu3d_set_interpolation(options.smooth_3d && !turbo_active);
        presentation.window_mode = options.window_mode;
        presentation.scaling = options.scaling;
        presentation.resolution_scale = options.resolution_scale;
        presentation.internal_resolution = options.internal_resolution;
        presentation.inset_percent =
            !presentation.single ? 0 : presentation.stacked ? 100
            : options.second_screen_size;
        const PresentationTicks presentation_ticks = present_screens(
            presentation, top_pixels, top_width,
            bottom_pixels, bottom_width,
            runtime_ui,
            mph_prime_virtual_stylus || generic_virtual_stylus,
            mph_virtual_x, mph_virtual_y,
            savestate_notice.empty() ? nullptr : savestate_notice.c_str(),
            options.show_fps ? fps_counter.c_str() : nullptr
#if defined(NDS_HAVE_COMPUTE_RENDERER)
            ,cpu_handoff ? &presented->gl : nullptr
#endif
            );
        const uint64_t presentation_end=SDL_GetPerformanceCounter();
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        if (cpu_handoff) {
            if (producer_pending) {
                try { cpu_handoff->Join(); }
                catch (const std::exception& e) {
                    std::fprintf(stderr,"[sdl] CPU producer failed: %s\n",e.what());
                    compute_failed=true; running=false; break;
                }
                producer_pending=false; completed_slot=building_slot;
            }
            const auto& produced=owned_frames[building_slot];
            const auto& handoff_timing=cpu_handoff->LastTiming();
            frame_work_ticks=std::max(presentation_end,handoff_timing.end)-phase0;
            const auto overlap_begin=std::max(phase1,handoff_timing.begin);
            const auto prefix_end=handoff_timing.first_request
                ? handoff_timing.first_request : handoff_timing.end;
            const auto overlap_end=std::min(presentation_end,prefix_end);
            const auto overlap=overlap_end>overlap_begin ? overlap_end-overlap_begin : 0;
            g_live_stats.cpu_render_handoff=1;
            g_live_stats.cpu_frames_produced=produced.job;
            g_live_stats.presented_job=presented->job;
            g_live_stats.presented_system_timestamp=presented->system_timestamp;
            g_live_stats.cpu_worker_cycles+=handoff_timing.cycles_end-handoff_timing.cycles_begin;
            g_live_stats.cpu_main_requests+=handoff_timing.main_requests;
            g_live_stats.cpu_overlap_ticks+=overlap;
            if (overlap) ++g_live_stats.cpu_overlap_frames;
            g_live_stats.cpu_prefix_cycles+=(handoff_timing.first_request
                ? handoff_timing.cycles_first_request : handoff_timing.cycles_end)
                -handoff_timing.cycles_begin;
            worker_emu_begin=produced.emu_begin; worker_emu_end=produced.emu_end;
            latency_cycles_emu=produced.cycles_begin; latency_cycles_present=produced.cycles_end;
            frame_emu_ticks=worker_emu_end-worker_emu_begin;
            phase_emu_ticks+=frame_emu_ticks;
            if (frame_emu_ticks>max_emu_ticks) { max_emu_ticks=frame_emu_ticks; max_emu_frame=shown_frames; }
            if (frame_emu_ticks*1000u>frequency*32u) ++slow_frames_32ms;
            if (!produced.valid || nds_gpu3d_compute_runtime_failed()) {
                compute_failed=!produced.powered_off; running=false; break;
            }
            if (touch_release_pending && touch_frames_held>=2) {
                nds_set_touch(0,0,false); touch_release_pending=false;
            }
        }
#endif
        if (!presentation_ticks.ok) {
            compute_failed = true;
            running = false;
            break;
        }
        phase_upload_ticks += presentation_ticks.upload;
        phase_draw_ticks += presentation_ticks.draw;
        phase_swap_ticks += presentation_ticks.swap;
        // Digest the exact surfaces the presenter was handed, after the
        // present so the hash cost is never inside a measured present phase.
        record_frame_digest(shown_frames, top_pixels, top_width,
                            bottom_pixels, bottom_width,
                            direct_present
#if defined(NDS_HAVE_COMPUTE_RENDERER)
                            ,cpu_handoff && presented->gl.has_hd ? &presented->gl.hd : nullptr,
                            bool(cpu_handoff)
#endif
                            );
        if (interpolation_active &&
            !direct_present) {
            cache_presented_frame(blend_cache, 0, top_pixels, top_width);
            cache_presented_frame(blend_cache, 1, bottom_pixels, bottom_width);
            blend_cache.valid = true;
        } else {
            blend_cache.valid = false;
        }
        phase_present_ticks += presentation_end - phase1;
        if (audio && audio_started) {
            const uint32_t queued = audio_queue_count(audio, audio_queue);
            audio_min_queue = std::min(audio_min_queue, queued);
        }
        const uint64_t phase2 = SDL_GetPerformanceCounter();
        const uint64_t latency_cycles_drain = latency_trace ? nds_frontend_latency::cycles() : 0;
        // Glide: after the prebuffered start, the pacing allowance decays a
        // fixed step per frame from the prebuffer level down to the steady
        // 63 ms target, shedding the extra startup latency over a couple of
        // seconds without ever forcing the queue down during a slow stretch.
        constexpr uint32_t kGlideStepFrames = 300;
        if (audio_started && !turbo_active) {
            const uint64_t underruns =
                audio_queue.underruns.load(std::memory_order_relaxed);
            if (underruns != audio_steady_underruns) {
                audio_steady_quiet_frames = 0;
                if (audio_steady_floor < kAudioSteadyMaxFrames) {
                    audio_steady_floor += kAudioSteadyStepFrames;
                    std::fprintf(stderr,
                                 "[sdl] audio underrun: queue target raised "
                                 "to %u frames (%u ms)\n",
                                 audio_steady_floor,
                                 audio_steady_floor * 1000u / kAudioFrequency);
                }
            } else if (++audio_steady_quiet_frames >= kAudioSteadyDecayFrames) {
                audio_steady_quiet_frames = 0;
                if (audio_steady_floor > kAudioQueueFrames)
                    audio_steady_floor -= kAudioSteadyStepFrames;
            }
            audio_steady_underruns = underruns;
        } else {
            audio_steady_underruns =
                audio_queue.underruns.load(std::memory_order_relaxed);
        }
        if (audio_pace_floor > audio_steady_floor + kGlideStepFrames)
            audio_pace_floor -= kGlideStepFrames;
        else
            audio_pace_floor = audio_steady_floor;
        uint32_t queued = audio_queue_count(audio, audio_queue);
        const uint32_t latency_queue_before_drain = queued;
        if (turbo_active) {
            if (!cpu_handoff) discard_spu_output();
        } else {
            queued = drain_audio(
                audio, audio_queue, audio_started, audio_pace_floor,
                audio_queue_error
#if defined(NDS_HAVE_COMPUTE_RENDERER)
                ,cpu_handoff ? &presented->pcm : nullptr
#endif
                );
        }
        const uint64_t frame_drain_ticks =
            SDL_GetPerformanceCounter() - phase2;
        phase_drain_ticks += frame_drain_ticks;
        audio_max_queue = std::max(audio_max_queue, queued);
        if (audio && !audio_started && !turbo_active &&
            queued >= audio_start_threshold) {
            // Opening paused and prebuffering avoids the guaranteed startup
            // underrun produced by unpausing an empty SDL queue.
            audio_queue.started.store(true, std::memory_order_relaxed);
            pause_audio(audio, false);
            audio_started = true;
            audio_min_queue = queued;
            audio_pace_floor = audio_start_threshold;
        }

        const uint64_t current_underruns =
            audio_queue.underruns.load(std::memory_order_relaxed);
        NdsGpu3dProfile current_gpu3d_profile{};
        nds_gpu3d_profile(&current_gpu3d_profile);
        // compute_readback_ns, not compute_map_ns: the latter only accumulates
        // under NDS_PROFILE_GPU, so it read zero in every normal run.
        const uint64_t frame_compute_map_ns =
            current_gpu3d_profile.compute_readback_ns >=
                    governor_last_compute_map_ns
                ? current_gpu3d_profile.compute_readback_ns -
                      governor_last_compute_map_ns
                : 0u;
        governor_last_compute_map_ns =
            current_gpu3d_profile.compute_readback_ns;
        NdsPerfGovernorSample governor_sample{};
        governor_sample.emu_ms =
            static_cast<double>(frame_emu_ticks) * 1000.0 /
            static_cast<double>(frequency);
        governor_sample.drain_ms =
            static_cast<double>(frame_drain_ticks) * 1000.0 /
            static_cast<double>(frequency);
        governor_sample.budget_ms = 1000.0 / 60.0;
        governor_sample.underruns_delta =
            current_underruns >= governor_last_underruns
                ? current_underruns - governor_last_underruns
                : 0u;
        governor_sample.compute_map_ms =
            static_cast<double>(frame_compute_map_ns) / 1000000.0;
        governor_sample.gpu_wait_ms =
            governor_sample.compute_map_ms +
            static_cast<double>(presentation_ticks.swap) * 1000.0 /
                static_cast<double>(frequency);
        // The drain is only a headroom signal when it is actually the pacing
        // sleep this frame: no device, pre-playback and turbo frames all read
        // ~0 for reasons that have nothing to do with load.
        governor_sample.headroom_valid =
            static_cast<bool>(audio) && audio_started && !turbo_active;
        governor_last_underruns = current_underruns;
        // For title settings that follow the window size; a minimized
        // window keeps the last size.
        if (const int points = presented_screen_points(presentation);
            points > 0)
            nds_title_set_display_height(points);
#if defined(NDS_HAVE_COMPUTE_RENDERER)
        // Auto scale. Runs here, with the producer joined, like the governor:
        // a renderer rebuild must not land between a frame's capture and its
        // present. The target must hold for 250 ms (a drag-resize sends many
        // sizes) before the costly rebuild, which happens only on an integer
        // multiplier change.
        if (runtime_menu_state.resolution_scale_available &&
            auto_scale_enabled && !perf_governor.apply_failed) {
            NdsPresentRect rect{};
            const uint8_t target =
                requested_internal_scale_for(presentation, options, &rect);
            const uint64_t now = SDL_GetPerformanceCounter();
            if (target == requested_internal_scale) {
                auto_scale_candidate = 0u;
            } else if (target != auto_scale_candidate) {
                auto_scale_candidate = target;
                auto_scale_since = now;
            } else if (now - auto_scale_since >= frequency / 4u) {
                auto_scale_candidate = 0u;
                const uint8_t effective =
                    perf_governor.stage >= 2u ? 1u : target;
                if (nds_gpu3d_set_runtime_internal_scale(effective)) {
                    nds_gpu2d_set_hd_emit(
                        effective > 1u && presenter_composes_3d(presentation));
                    std::fprintf(stderr,
                                 "[display] internal %ux -> %ux "
                                 "(presented %dx%d, resolution scale "
                                 "%u%%%s) frame %llu\n",
                                 static_cast<unsigned>(
                                     requested_internal_scale),
                                 static_cast<unsigned>(target), rect.w,
                                 rect.h,
                                 static_cast<unsigned>(
                                     options.resolution_scale),
                                 effective != target
                                     ? ", governor holds 1x" : "",
                                 static_cast<unsigned long long>(
                                     shown_frames));
                    requested_internal_scale = target;
                    blend_cache.valid = false;
                } else {
                    // Same terminal rule as the governor: a failing rebuild
                    // is not retried every frame.
                    auto_scale_enabled = false;
                    std::fprintf(stderr,
                                 "[display] internal %ux could not be "
                                 "applied; auto scale disabled at %ux\n",
                                 static_cast<unsigned>(target),
                                 static_cast<unsigned>(
                                     nds_gpu3d_internal_scale()));
                }
            }
        }
#endif
        const uint8_t governor_stage_before = perf_governor.stage;
        if (nds_perf_governor_update(&perf_governor, governor_sample)) {
            const uint8_t requested_stage = perf_governor.stage;
            NdsPerfGovernorReason reason = perf_governor.last_reason;
            bool applied = apply_performance_governor_stage(
                presentation, requested_stage, requested_internal_scale);
            if (!applied) {
                // Nothing was mutated by the failed apply, so the previously
                // installed stage is still the live one. Report that stage and
                // stop deciding: retrying a failing rebuild every
                // engage_frames forever is a hitch generator, not a recovery.
                perf_governor.stage = governor_stage_before;
                nds_perf_governor_mark_apply_failed(&perf_governor);
                reason = NdsPerfGovernorReason::ApplyFailed;
                std::fprintf(stderr,
                             "[governor] stage %u apply failed; "
                             "governor disabled at stage %u\n",
                             static_cast<unsigned>(requested_stage),
                             static_cast<unsigned>(perf_governor.stage));
            }
            // Never a no-op N->N transition. A failed apply is reported even
            // when the stage did not move, because the terminal state is the
            // information.
            if (perf_governor.stage != governor_stage_before || !applied) {
                nds_perf_governor_record_transition(
                    governor_stage_before, perf_governor.stage, reason,
                    shown_frames, perf_governor.stage2_held,
                    perf_governor.apply_failed);
                nds_diagnostics_note_perf_governor_transition(
                    governor_stage_before, perf_governor.stage,
                    nds_perf_governor_reason_name(reason));
            }
        }

        ++shown_frames;
        ++fps_frames;
        g_live_stats.frames = shown_frames;
        g_live_stats.emu_ticks = phase_emu_ticks;
        g_live_stats.present_ticks = phase_present_ticks;
        g_live_stats.adaptive_ticks = phase_adaptive_ticks;
        g_live_stats.upload_ticks = phase_upload_ticks;
        g_live_stats.draw_ticks = phase_draw_ticks;
        g_live_stats.swap_ticks = phase_swap_ticks;
        g_live_stats.drain_ticks = phase_drain_ticks;
        g_live_stats.underruns = current_underruns;
        g_live_stats.real_presents = shown_frames;
        g_live_stats.synthetic_presents = synthetic_presents;
        g_live_stats.perf_governor_stage = perf_governor.stage;
        g_live_stats.perf_governor_over_frames = perf_governor.over_frames;
        g_live_stats.perf_governor_under_frames =
            perf_governor.under_frames;
        g_live_stats.perf_governor_held = perf_governor.stage2_held ? 1u : 0u;
        g_live_stats.perf_governor_apply_failed =
            perf_governor.apply_failed ? 1u : 0u;
        g_live_stats.perf_governor_transitions =
            nds_perf_governor_history_total();
        const uint64_t counter = SDL_GetPerformanceCounter();
        if (latency_trace)
            nds_frontend_latency::frame({shown_frames, latency_loop,
                cpu_handoff ? worker_emu_begin : phase0,
                cpu_handoff ? worker_emu_end : phase0 + frame_emu_ticks, phase1, presentation_end,
                phase2 + frame_drain_ticks, counter, latency_cycles_loop,
                latency_cycles_emu, latency_cycles_present, latency_cycles_drain,
                nds_frontend_latency::cycles(), latency_queue_begin,
                latency_queue_before_drain, queued, current_underruns,
                phase_adaptive_ticks - latency_adaptive_before,
                presentation_ticks.upload, presentation_ticks.draw,
                presentation_ticks.swap, frame_compute_map_ns,
                audio_started ? 1u : 0u, presentation_ticks.pool_wait,
                presentation_ticks.gl_setup, presentation_ticks.gl_error,
                presentation_ticks.overlay, adaptive_start - phase1,
                keyboard_pressed, g_input_debug.published_key_mask,
                scheduler_system_timestamp(),g_live_stats.cpu_render_handoff,
                g_live_stats.cpu_frames_produced,g_live_stats.presented_job,
                g_live_stats.presented_system_timestamp,g_live_stats.cpu_overlap_ticks,
                g_live_stats.cpu_worker_cycles});
        record_frontend_pacing(counter, frame_emu_ticks, presentation_end - phase1,
                               frequency,frame_work_ticks);
        if (!savestate_notice.empty() && counter >= savestate_notice_until)
            savestate_notice.clear();
        g_live_stats.now_ticks = counter;
        g_live_stats.freq = frequency;
        nds_diagnostics_maybe_write_performance_sample(g_live_stats);
        nds_wifi_frame_tick();
        if (counter - fps_start >= frequency) {
            const double seconds = static_cast<double>(counter - fps_start) /
                                   static_cast<double>(frequency);
            const double fps = static_cast<double>(fps_frames) / seconds;
            const std::string fps_text =
                std::to_string(fps).substr(0, 4) + " FPS";
            NdsGpu3dProfile gpu3d_profile{};
            nds_gpu3d_profile(&gpu3d_profile);
            // Loading a savestate resets the profile.
            const uint64_t images = gpu3d_profile.gx_new_images +
                                    gpu3d_profile.gx_interp_images;
            const uint64_t new_images =
                images >= fps_images ? images - fps_images : 0;
            char fps_counter_text[24];
            std::snprintf(fps_counter_text, sizeof(fps_counter_text),
                          "3D %.1f FPS",
                          static_cast<double>(new_images) / seconds);
            fps_counter = fps_counter_text;
            fps_images = images;
            const std::string governor_text =
                perf_governor.apply_failed
                    ? " - Gov disabled (apply failed, S" +
                          std::to_string(perf_governor.stage) + ")"
                    : perf_governor.stage
                    ? " - Gov S" + std::to_string(perf_governor.stage) +
                          (perf_governor.stage2_held ? " held" : "")
                    : "";
            const std::string top_title = !savestate_notice.empty()
                ? "ndsrecomp - " + savestate_notice
                : presentation.single
                ? (options.game_name.empty() ? std::string("ndsrecomp")
                                             : options.game_name) +
                      " - " + fps_text + governor_text
                : presentation.separate
                ? "ndsrecomp - Top Screen - " + fps_text + governor_text
                : "ndsrecomp firmware preview - " + fps_text +
                      governor_text;
            SDL_SetWindowTitle(presentation.windows[0],
                               top_title.c_str());
            if (presentation.separate) {
                const std::string bottom_title =
                    "ndsrecomp - Bottom Screen - " + fps_text +
                    governor_text;
                SDL_SetWindowTitle(presentation.windows[1],
                                   bottom_title.c_str());
            }
            fps_frames = 0;
            fps_start = counter;
        }

        if (soak_frames && shown_frames >= soak_frames)
            running = false;
        if (selftest_menu && selftest_touch_up &&
            nds_event_counts().vblank9 >= 600)
            running = false;
        if (selftest_relative_mouse && relative_mouse_selftest_stage == 10)
            running = false;

        if (scheduler_cpu_terminal_halted(0) &&
            scheduler_cpu_terminal_halted(1)) {
            SDL_Delay(8);
        }
    }

    if (cpu_handoff) {
        cpu_handoff.reset(); // drain/pump before SDL/GL teardown or durable flush
        nds_hostprof_unregister_current_thread();
        nds_hostprof_register_current_thread(NDS_HOSTPROF_ROLE_EMU);
    }
    if (audio) pause_audio(audio, true);
    finish_pending_save(true);
    release_relative_mouse();
    nds_set_touch(0, 0, false);
    nds_set_key_mask(0x0FFFu);
    g_live_stats.active = 0;
    g_input_debug.active = 0;
    nds_diagnostics_stop_performance_log();
    const double soak_seconds = static_cast<double>(
        SDL_GetPerformanceCounter() - soak_start) /
        static_cast<double>(frequency);
    const uint64_t top_hash = framebuffer_rgb_fnv(0);
    const uint64_t bottom_hash = framebuffer_rgb_fnv(1);
    if (audio) pause_audio(audio, true);
    const uint64_t audio_underruns =
        audio_queue.underruns.load(std::memory_order_relaxed);
    // close_audio() clears the handle (SDL2 zeroes the device id, SDL3 nulls
    // the stream), so whether a device was ever opened has to be latched
    // BEFORE the close. The audio_failed verdict below reads this, not
    // `audio` -- reading the closed handle made NDS_FRONTEND_REQUIRE_AUDIO=1
    // report failure on every clean run.
    const bool audio_opened = static_cast<bool>(audio);
    close_audio(audio);
    nds_frontend_latency::finish();
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    nds_gpu2d_set_direct_present(false);
    nds_compute_host_stop();
#endif
    if (controller) SDL_GameControllerClose(controller);
    if (runtime_ui) {
        recomp_runtime_ui_destroy(runtime_ui);
        runtime_ui = nullptr;
    }
    // A resize still inside its settle window when the user quit.
    if (window_resize_save_at && !presentation.separate)
        persist_window_size(options, presentation.windows[0]);
    destroy_presentation(presentation);
    SDL_Quit();
    std::fprintf(stderr, "[sdl] closed after %llu presented frames\n",
                 static_cast<unsigned long long>(shown_frames));
    if (print_stats || soak_frames || selftest_menu ||
        selftest_relative_mouse) {
        std::fprintf(stderr,
            "[sdl] soak: frames=%llu seconds=%.3f fps=%.3f "
            "audio_started=%u queue_errors=%u underruns=%llu "
            "min_queue_frames=%u max_queue_frames=%u "
            "key_presses=%llu touch_presses=%llu last_touch=(%d,%d) "
            "frame_fnv=(%016llx,%016llx)\n",
            static_cast<unsigned long long>(shown_frames), soak_seconds,
            soak_seconds > 0.0 ? shown_frames / soak_seconds : 0.0,
            audio_started ? 1u : 0u, audio_queue_error ? 1u : 0u,
            static_cast<unsigned long long>(audio_underruns),
            audio_min_queue == std::numeric_limits<uint32_t>::max()
                ? 0u : audio_min_queue,
            audio_max_queue,
            static_cast<unsigned long long>(host_key_presses),
            static_cast<unsigned long long>(host_touch_presses),
            last_touch_event_x, last_touch_event_y,
            static_cast<unsigned long long>(top_hash),
            static_cast<unsigned long long>(bottom_hash));
        const double tick_seconds = 1.0 / static_cast<double>(frequency);
        std::fprintf(stderr,
            "[sdl] phases: emu=%.3fs present=%.3fs drain=%.3fs other=%.3fs "
            "max_emu_ms=%.1f@f%llu slow32ms=%llu underrun_frames=[%llu,%llu]\n",
            phase_emu_ticks * tick_seconds,
            phase_present_ticks * tick_seconds,
            phase_drain_ticks * tick_seconds,
            soak_seconds - (phase_emu_ticks + phase_present_ticks +
                            phase_drain_ticks) * tick_seconds,
            max_emu_ticks * tick_seconds * 1000.0,
            static_cast<unsigned long long>(max_emu_frame),
            static_cast<unsigned long long>(slow_frames_32ms),
            static_cast<unsigned long long>(first_underrun_frame),
            static_cast<unsigned long long>(last_underrun_frame));
        std::fprintf(stderr,
            "[sdl] present detail: adaptive=%.3fs upload=%.3fs "
            "draw=%.3fs swap=%.3fs\n",
            phase_adaptive_ticks * tick_seconds,
            phase_upload_ticks * tick_seconds,
            phase_draw_ticks * tick_seconds,
            phase_swap_ticks * tick_seconds);
        std::fprintf(stderr,
            "[sdl] frame interpolation: mode=%s active=%u refresh=%dHz "
            "min_refresh=%dHz real_presents=%llu synthetic_presents=%llu\n",
            nds_frame_interpolation_name(options.frame_interpolation),
            interpolation_active ? 1u : 0u,
            interpolation_refresh_hz, interpolation_min_refresh_hz,
            static_cast<unsigned long long>(shown_frames),
            static_cast<unsigned long long>(synthetic_presents));
        nds_profile_report(stderr);
    }
    const bool audio_failed = audio_queue_error ||
        (require_audio &&
         (audio_underruns != 0 || !audio_opened || !audio_started));
    const bool menu_selftest_failed = selftest_menu &&
        (selftest_event_error || !selftest_key_up || !selftest_touch_up ||
         host_key_presses != 1 || host_touch_presses != 1 ||
         last_touch_event_x != bottom_content_left + 127 ||
         last_touch_event_y != (presentation.separate ? 180 : 372) ||
         top_hash != 0xa0f41b93e4eefa55ull ||
         bottom_hash != 0x6c43b370e9cda730ull);
    const bool relative_mouse_selftest_failed = selftest_relative_mouse &&
        (relative_mouse_selftest_error || relative_mouse_selftest_stage != 10);
    if (selftest_menu)
        std::fprintf(stderr, "[sdl] menu self-test: %s\n",
                     menu_selftest_failed ? "FAIL" : "PASS");
    if (selftest_relative_mouse)
        std::fprintf(stderr, "[sdl] relative mouse self-test: %s\n",
                     relative_mouse_selftest_failed ? "FAIL" : "PASS");
    return (audio_failed || menu_selftest_failed ||
            relative_mouse_selftest_failed || compute_failed) ? 1 : 0;
}

#else

int nds_run_interactive_frontend(const NdsFrontendOptions&) {
    std::fprintf(stderr,
        "[sdl] this runner was built without SDL; configure NDS_SDL_BACKEND=SDL3 "
        "or SDL2 for interactive presentation\n");
    return 1;
}

#endif

bool nds_frontend_request_exit() {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_live_stats.active) return false;
    SDL_Event event{};
    event.type = SDL_QUIT;
    return sdl_push_event(event);
#else
    return false;
#endif
}

bool nds_frontend_debug_key(const char* key_name, bool down, bool shift) {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active || !key_name || key_name[0] == '\0')
        return false;
    const SDL_Scancode key = scancode_from_binding_name(key_name);
    g_input_debug.debug_last_key_scancode = static_cast<uint32_t>(key);
    if (key == SDL_SCANCODE_UNKNOWN)
        return false;
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = g_input_debug.top_window_id;
#if !defined(NDS_HAVE_SDL3)
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
#else
    event.key.down = down;
#endif
    event.key.repeat = 0;
    // Modifier state: savestate SAVE is Shift+F<slot> while LOAD is the bare
    // F-key, so an injected key that cannot carry Shift can only ever load.
#if defined(NDS_HAVE_SDL3)
    event.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
#else
    event.key.keysym.mod = shift ? KMOD_LSHIFT : KMOD_NONE;
#endif
    sdl_set_event_scancode(event, key);
    const bool pushed = sdl_push_event(event);
    if (pushed) ++g_input_debug.debug_key_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)key_name;
    (void)down;
    (void)shift;
    return false;
#endif
}

bool nds_frontend_debug_mouse_button(uint8_t button, bool down) {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active || button == 0)
        return false;
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = g_input_debug.top_window_id;
    event.button.button = button;
#if !defined(NDS_HAVE_SDL3)
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
#else
    event.button.down = down;
#endif
    event.button.clicks = 1;
    event.button.x = 128 * kWindowScale;
    event.button.y = 96 * kWindowScale;
    const bool pushed = sdl_push_event(event);
    if (pushed) ++g_input_debug.debug_mouse_button_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)button;
    (void)down;
    return false;
#endif
}

bool nds_frontend_debug_mouse_motion(int dx, int dy) {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active)
        return false;
    SDL_Event event{};
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = g_input_debug.top_window_id;
    event.motion.x = 128 * kWindowScale;
    event.motion.y = 96 * kWindowScale;
    event.motion.xrel = dx;
    event.motion.yrel = dy;
    const bool pushed = sdl_push_event(event);
    if (pushed) ++g_input_debug.debug_mouse_motion_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)dx;
    (void)dy;
    return false;
#endif
}

bool nds_frontend_debug_touch(uint16_t x, uint16_t y, bool down) {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active || x >= kScreenWidth || y >= kScreenHeight)
        return false;
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.windowID = g_input_debug.bottom_window_id;
    event.button.button = SDL_BUTTON_LEFT;
#if !defined(NDS_HAVE_SDL3)
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
#else
    event.button.down = down;
#endif
    event.button.clicks = 1;
    const int logical_x = g_input_debug.bottom_content_left + static_cast<int>(x);
    const int logical_y = (g_input_debug.separate ? 0 : kScreenHeight) + static_cast<int>(y);
    SDL_Window* window = SDL_GetWindowFromID(event.button.windowID);
    SDL_Renderer* renderer = window ? SDL_GetRenderer(window) : nullptr;
    if (!renderer) return false;
#if defined(NDS_HAVE_SDL3)
    if (!SDL_RenderCoordinatesToWindow(renderer, static_cast<float>(logical_x),
            static_cast<float>(logical_y), &event.button.x, &event.button.y)) return false;
#elif SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_RenderLogicalToWindow(renderer, static_cast<float>(logical_x),
        static_cast<float>(logical_y), &event.button.x, &event.button.y);
#else
    event.button.x = logical_x * kWindowScale;
    event.button.y = logical_y * kWindowScale;
#endif
    const bool pushed = sdl_push_event(event);
    if (pushed) ++g_input_debug.debug_touch_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    (void)x;
    (void)y;
    (void)down;
    return false;
#endif
}

bool nds_frontend_debug_capture_mouse() {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active)
        return false;
    if (g_input_debug.relative_mouse_captured)
        return true;
    const bool pushed = nds_frontend_debug_mouse_button(SDL_BUTTON_LEFT, true);
    if (pushed) ++g_input_debug.debug_capture_events;
    return pushed;
#else
    return false;
#endif
}

bool nds_frontend_debug_release_mouse() {
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (!g_input_debug.active)
        return false;
    if (!g_input_debug.relative_mouse_captured)
        return true;
    SDL_Event event{};
    sdl_make_window_event(event, SDL_EVENT_WINDOW_FOCUS_LOST,
                          g_input_debug.top_window_id);
    const bool pushed = sdl_push_event(event);
    if (pushed) ++g_input_debug.debug_release_events;
    else ++g_input_debug.debug_event_errors;
    return pushed;
#else
    return false;
#endif
}

void nds_frontend_live_stats(NdsFrontendLiveStats* out) {
    if (!out) return;
    *out = g_live_stats;
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    if (g_live_stats.active)
        out->now_ticks = SDL_GetPerformanceCounter();
#endif
}

void nds_frontend_pacing_stats(NdsFrontendPacingStats* out, bool reset) {
    if (reset) {
        g_pacing_stats = {};
        g_pacing_stats.armed = true;
        g_pacing_stats.frequency = g_live_stats.freq;
        g_pacing_previous_counter = 0;
    }
    if (out) *out = g_pacing_stats;
}

void nds_frontend_input_debug_state(NdsFrontendInputDebugState* out) {
    if (out) *out = g_input_debug;
}

void nds_frontend_black_band_scan(bool enabled, bool reset) {
    if (reset) g_black_band = {};
    g_black_band.enabled = enabled ? 1 : 0;
#if defined(NDS_HAVE_COMPUTE_RENDERER)
    nds_gpu2d_set_direct_present(
        !enabled && nds_compute_host_has_visible_context() &&
        nds_compute_host_composes_3d() && nds_compute_host_presents_direct());
#endif
}

void nds_frontend_black_band_capture(NdsFrontendBlackBandCapture* out) {
    if (out) *out = g_black_band;
}

bool nds_frontend_frame_digest_enabled() { return frame_digest_enabled(); }

uint64_t nds_frontend_frame_digest_count() {
    return g_frame_digest_count.load(std::memory_order_acquire);
}

uint32_t nds_frontend_frame_digests(uint64_t from, uint32_t max,
                                    NdsFrontendFrameDigest* out) {
    if (!out || !max) return 0;
    const uint64_t count =
        g_frame_digest_count.load(std::memory_order_acquire);
    if (from >= count) return 0;
    // The oldest sequence index still resident. A reader that asks for an
    // evicted window gets nothing rather than a torn mixture of generations.
    const uint64_t oldest =
        count > kFrameDigestRing ? count - kFrameDigestRing : 0;
    if (from < oldest) from = oldest;
    uint32_t copied = 0;
    for (uint64_t i = from; i < count && copied < max; ++i, ++copied)
        out[copied] = g_frame_digests[i % kFrameDigestRing];
    // Re-check: a wrap during the copy would have overwritten the head of the
    // window. Only possible if the caller asked for ~2 minutes of history.
    const uint64_t after =
        g_frame_digest_count.load(std::memory_order_acquire);
    if (after - from > kFrameDigestRing) return 0;
    return copied;
}
