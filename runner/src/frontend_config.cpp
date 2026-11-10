#include "frontend.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "toml.hpp"

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

}  // namespace

static bool nds_set_virtual_stylus_binding(std::string* target,
                                           const std::string& value) {
    if (!target || value.size() >= 64u) return false;
    *target = value;
    return true;
}

bool nds_parse_screen_layout(const std::string& value,
                             NdsScreenLayout* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "stacked" || normalized == "combined") {
        *out = NdsScreenLayout::Stacked;
        return true;
    }
    if (normalized == "single" || normalized == "main") {
        *out = NdsScreenLayout::Single;
        return true;
    }
    if (normalized == "separate" || normalized == "two-window" ||
        normalized == "two_windows") {
        *out = NdsScreenLayout::Separate;
        return true;
    }
    return false;
}

bool nds_parse_window_mode(const std::string& value, NdsWindowMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "letterbox" || normalized == "fit") {
        *out = NdsWindowMode::Letterbox;
        return true;
    }
    if (normalized == "stretch") {
        *out = NdsWindowMode::Stretch;
        return true;
    }
    if (normalized == "original" || normalized == "1:1" ||
        normalized == "native") {
        *out = NdsWindowMode::Original;
        return true;
    }
    return false;
}

bool nds_parse_scaling_filter(const std::string& value,
                              NdsScalingFilter* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "integer" || normalized == "pixel-perfect" ||
        normalized == "pixel_perfect") {
        *out = NdsScalingFilter::Integer;
        return true;
    }
    if (normalized == "bilinear" || normalized == "linear") {
        *out = NdsScalingFilter::Bilinear;
        return true;
    }
    if (normalized == "nearest" || normalized == "nearest-neighbor" ||
        normalized == "nearest_neighbor" || normalized == "nearest-neighbour" ||
        normalized == "nearest_neighbour" || normalized == "point") {
        *out = NdsScalingFilter::Nearest;
        return true;
    }
    return false;
}

bool nds_parse_fullscreen_mode(const std::string& value,
                               NdsFullscreenMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "off") {
        *out = NdsFullscreenMode::Off;
        return true;
    }
    if (normalized == "borderless") {
        *out = NdsFullscreenMode::Borderless;
        return true;
    }
    if (normalized == "exclusive") {
        *out = NdsFullscreenMode::Exclusive;
        return true;
    }
    return false;
}

bool nds_apply_fullscreen_overrides(
    NdsFrontendOptions* options, const std::string& cli_fullscreen,
    NdsFullscreenOverrideError* error) {
    if (error) *error = NdsFullscreenOverrideError::None;
    if (!options) return false;
    if (const char* value = std::getenv("NDS_FULLSCREEN")) {
        if (!nds_parse_fullscreen_mode(value, &options->fullscreen)) {
            if (error) *error = NdsFullscreenOverrideError::Environment;
            return false;
        }
    }
    if (!cli_fullscreen.empty() &&
        !nds_parse_fullscreen_mode(cli_fullscreen, &options->fullscreen)) {
        if (error) *error = NdsFullscreenOverrideError::CommandLine;
        return false;
    }
    return true;
}

bool nds_parse_adaptive_screens(const std::string& value, uint8_t* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "none" || normalized == "off") {
        *out = NDS_ADAPTIVE_NONE;
        return true;
    }
    if (normalized == "top") {
        *out = NDS_ADAPTIVE_TOP;
        return true;
    }
    if (normalized == "bottom") {
        *out = NDS_ADAPTIVE_BOTTOM;
        return true;
    }
    if (normalized == "both") {
        *out = NDS_ADAPTIVE_BOTH;
        return true;
    }
    return false;
}

bool nds_parse_widescreen_width(const std::string& value, uint16_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' ||
        (parsed != 256 && parsed != 320 &&
         parsed != 384 && parsed != 448)) {
        return false;
    }
    *out = static_cast<uint16_t>(parsed);
    return true;
}

bool nds_parse_supersampling(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > 4) return false;
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_internal_resolution(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1 || parsed > kNdsMaxInternalScale) return false;
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_second_screen_size(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 60) return false;
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_gamepad_layout(const std::string& value, bool* positional) {
    if (!positional) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "labels") {
        *positional = false;
    } else if (normalized == "positional") {
        *positional = true;
    } else {
        return false;
    }
    return true;
}

bool nds_parse_resolution_scale(const std::string& value, uint16_t* out) {
    if (!out || value.empty()) return false;
    std::string digits = value;
    if (digits.back() == '%') digits.pop_back();
    if (digits.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(digits.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 10 || parsed > 1000) return false;
    *out = static_cast<uint16_t>(parsed);
    return true;
}

bool nds_parse_texture_upscale(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' ||
        (parsed != 1 && parsed != 2 && parsed != 4)) {
        return false;
    }
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_frame_interpolation(const std::string& value,
                                   NdsFrameInterpolation* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "off" || normalized == "none" || normalized == "0") {
        *out = NdsFrameInterpolation::Off;
        return true;
    }
    if (normalized == "blend") {
        *out = NdsFrameInterpolation::Blend;
        return true;
    }
    return false;
}

bool nds_parse_antialiasing(const std::string& value, uint8_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' ||
        (parsed != 0 && parsed != 2 && parsed != 4 && parsed != 8)) {
        return false;
    }
    *out = static_cast<uint8_t>(parsed);
    return true;
}

bool nds_parse_on_off(const std::string& value, bool* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "on" || normalized == "true" || normalized == "1" ||
        normalized == "yes") {
        *out = true;
        return true;
    }
    if (normalized == "off" || normalized == "false" || normalized == "0" ||
        normalized == "no") {
        *out = false;
        return true;
    }
    return false;
}

bool nds_parse_mouse_sensitivity(const std::string& value, uint16_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 10 || parsed > 400) return false;
    *out = static_cast<uint16_t>(parsed);
    return true;
}

bool nds_parse_mouse_fire_key(const std::string& value, uint16_t* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "none" || normalized == "off") {
        *out = 0;
        return true;
    }
    if (normalized == "a") *out = 1u << 0;
    else if (normalized == "b") *out = 1u << 1;
    else if (normalized == "r") *out = 1u << 8;
    else if (normalized == "l") *out = 1u << 9;
    else if (normalized == "x") *out = 1u << 10;
    else if (normalized == "y") *out = 1u << 11;
    else return false;
    return true;
}

namespace {

std::string NdsMphPrimeControlBindings::* mph_binding_member(
    const std::string& action) {
    using B = NdsMphPrimeControlBindings;
    if (action == "move-forward") return &B::move_forward;
    if (action == "move-back") return &B::move_back;
    if (action == "move-left") return &B::move_left;
    if (action == "move-right") return &B::move_right;
    if (action == "jump") return &B::jump;
    if (action == "morph-ball") return &B::morph_ball;
    if (action == "boost-zoom") return &B::boost_zoom;
    if (action == "scan-visor") return &B::scan_visor;
    if (action == "ui-left") return &B::ui_left;
    if (action == "ui-right") return &B::ui_right;
    if (action == "ui-ok") return &B::ui_ok;
    if (action == "shoot") return &B::shoot;
    if (action == "scan-shoot") return &B::scan_shoot;
    if (action == "beam") return &B::beam;
    if (action == "missile") return &B::missile;
    if (action == "weapon1") return &B::weapon1;
    if (action == "weapon2") return &B::weapon2;
    if (action == "weapon3") return &B::weapon3;
    if (action == "weapon4") return &B::weapon4;
    if (action == "weapon5") return &B::weapon5;
    if (action == "weapon6") return &B::weapon6;
    if (action == "virtual-stylus") return &B::virtual_stylus;
    if (action == "menu") return &B::menu;
    return nullptr;
}

}  // namespace

NdsMphPrimeControlBindings nds_default_mph_pad_bindings() {
    NdsMphPrimeControlBindings pad;
    // Movement comes from the left stick / D-pad natively; keep the four
    // move actions unbound so the D-pad stays a plain D-pad.
    pad.move_forward = "None";
    pad.move_back = "None";
    pad.move_left = "None";
    pad.move_right = "None";
    pad.jump = "Pad A";
    pad.morph_ball = "Pad B";
    pad.boost_zoom = "Pad RB";
    pad.scan_visor = "Pad R3";
    pad.ui_left = "None";
    pad.ui_right = "None";
    pad.ui_ok = "Pad Y";
    pad.shoot = "Pad RT";
    pad.scan_shoot = "Pad LT";
    pad.beam = "Pad LB";
    pad.missile = "Pad X";
    pad.weapon1 = "None";
    pad.weapon2 = "None";
    pad.weapon3 = "None";
    pad.weapon4 = "None";
    pad.weapon5 = "None";
    pad.weapon6 = "None";
    // Keep the virtual stylus on a keyboard binding by default; reserving a
    // controller button for it would reduce the already limited pad bindings.
    pad.virtual_stylus = "None";
    pad.menu = "Pad Start";
    return pad;
}

bool nds_set_mph_prime_binding(NdsFrontendOptions* options,
                               const std::string& action,
                               const std::string& value) {
    if (!options || value.size() >= 64u) return false;
    const auto member = mph_binding_member(action);
    if (!member) return false;
    options->mph_bindings.*member = value;
    return true;
}

bool nds_set_mph_prime_pad_binding(NdsFrontendOptions* options,
                                   const std::string& action,
                                   const std::string& value) {
    if (!options || value.size() >= 64u) return false;
    const auto member = mph_binding_member(action);
    if (!member) return false;
    options->mph_pad_bindings.*member = value;
    return true;
}

bool nds_parse_cartridge_save_type(const std::string& value,
                                   NdsCartridgeSaveType* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "none") {
        *out = NdsCartridgeSaveType::None;
        return true;
    }
    if (normalized == "eeprom-tiny" || normalized == "eeprom_tiny") {
        *out = NdsCartridgeSaveType::EepromTiny;
        return true;
    }
    if (normalized == "eeprom") {
        *out = NdsCartridgeSaveType::Eeprom;
        return true;
    }
    if (normalized == "flash") {
        *out = NdsCartridgeSaveType::Flash;
        return true;
    }
    return false;
}

bool nds_parse_ipv4(const std::string& value, uint32_t* out) {
    if (!out || value.empty()) return false;
    uint32_t octets[4] = {0, 0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= value.size() || !std::isdigit(static_cast<unsigned char>(value[pos])))
            return false;
        size_t start = pos;
        while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])))
            ++pos;
        const std::string digits = value.substr(start, pos - start);
        if (digits.size() > 1 && digits[0] == '0') return false;  // no leading zeros
        if (digits.size() > 3) return false;
        const long v = std::strtol(digits.c_str(), nullptr, 10);
        if (v < 0 || v > 255) return false;
        octets[i] = static_cast<uint32_t>(v);
        if (i < 3) {
            if (pos >= value.size() || value[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos != value.size()) return false;  // trailing garbage
    *out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return true;
}

bool nds_parse_network_backend(const std::string& value, std::string* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    // Wiimmfi M8 added "replay"; M7 wires "pcap" when the runner is built
    // with NDS_ENABLE_PCAP_BACKEND. Builds without that flag reject pcap at
    // main.cpp's bridge-construction validation.
    if (normalized == "slirp" || normalized == "pcap" ||
        normalized == "replay") {
        *out = normalized;
        return true;
    }
    return false;
}

bool nds_parse_startup_mode(const std::string& value,
                            NdsStartupMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "preserve" || normalized == "firmware" ||
        normalized == "default") {
        *out = NdsStartupMode::Preserve;
        return true;
    }
    if (normalized == "manual" || normalized == "menu") {
        *out = NdsStartupMode::Manual;
        return true;
    }
    if (normalized == "automatic" || normalized == "auto" ||
        normalized == "slot-1" || normalized == "slot1") {
        *out = NdsStartupMode::Automatic;
        return true;
    }
    return false;
}

bool nds_parse_boot_mode(const std::string& value, NdsBootMode* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    if (normalized == "lle" || normalized == "firmware" ||
        normalized == "default") {
        *out = NdsBootMode::Lle;
        return true;
    }
    if (normalized == "direct") {
        *out = NdsBootMode::Direct;
        return true;
    }
    return false;
}

bool nds_parse_firmware_language(const std::string& value,
                                 NdsFirmwareLanguage* out) {
    if (!out) return false;
    const std::string normalized = lower_ascii(value);
    static const struct {
        const char* name;
        const char* code;
        NdsFirmwareLanguage language;
    } kLanguages[] = {
        {"preserve", "firmware", NdsFirmwareLanguage::Preserve},
        {"japanese", "ja", NdsFirmwareLanguage::Japanese},
        {"english", "en", NdsFirmwareLanguage::English},
        {"french", "fr", NdsFirmwareLanguage::French},
        {"german", "de", NdsFirmwareLanguage::German},
        {"italian", "it", NdsFirmwareLanguage::Italian},
        {"spanish", "es", NdsFirmwareLanguage::Spanish},
        {"chinese", "zh", NdsFirmwareLanguage::Chinese},
    };
    for (const auto& entry : kLanguages) {
        if (normalized == entry.name || normalized == entry.code) {
            *out = entry.language;
            return true;
        }
    }
    return false;
}

const char* nds_firmware_language_name(NdsFirmwareLanguage value) {
    switch (value) {
        case NdsFirmwareLanguage::Japanese: return "japanese";
        case NdsFirmwareLanguage::English: return "english";
        case NdsFirmwareLanguage::French: return "french";
        case NdsFirmwareLanguage::German: return "german";
        case NdsFirmwareLanguage::Italian: return "italian";
        case NdsFirmwareLanguage::Spanish: return "spanish";
        case NdsFirmwareLanguage::Chinese: return "chinese";
        default: return "preserve";
    }
}

bool nds_parse_instance_index(const std::string& value, uint32_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 255) return false;
    *out = static_cast<uint32_t>(parsed);
    return true;
}

bool nds_parse_local_wireless_base_port(const std::string& value,
                                        uint16_t* out) {
    if (!out || value.empty()) return false;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 1024 || parsed > 65520)
        return false;
    *out = static_cast<uint16_t>(parsed);
    return true;
}

namespace {

bool parse_u32_in_range(const std::string& value, uint32_t max,
                        uint32_t* out) {
    if (value.empty()) return false;
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed < 0 ||
        parsed > static_cast<long long>(max))
        return false;
    *out = static_cast<uint32_t>(parsed);
    return true;
}

}  // namespace

constexpr size_t kMaxLocalWirelessPeers = 8;

bool nds_is_local_wireless_tuning_flag(const std::string& flag) {
    return flag == "--local-wireless-delay-ms" ||
           flag == "--local-wireless-jitter-ms" ||
           flag == "--local-wireless-loss-pct" ||
           flag == "--local-wireless-late-replies" ||
           flag == "--local-wireless-reply-budget-us" ||
           flag == "--local-wireless-stay-awake" ||
           flag == "--local-wireless-link" ||
           flag == "--local-wireless-peer" ||
           flag == "--netplay" || flag == "--netplay-sign-in" ||
           flag == "--netplay-join" || flag == "--netplay-lobby-name" ||
           flag == "--netplay-lobby-url";
}

bool nds_parse_local_wireless_tuning(const std::string& flag,
                                     const std::string& value,
                                     NdsLocalWirelessOptions* out,
                                     std::string* error) {
    const auto fail = [&](const char* range) {
        if (error) *error = "invalid " + flag + " (expected " + range + ")";
        return false;
    };
    if (flag == "--local-wireless-delay-ms")
        return parse_u32_in_range(value, 2000, &out->delay_ms) ||
               fail("0..2000");
    if (flag == "--local-wireless-jitter-ms")
        return parse_u32_in_range(value, 2000, &out->jitter_ms) ||
               fail("0..2000");
    if (flag == "--local-wireless-loss-pct")
        return parse_u32_in_range(value, 100, &out->loss_pct) ||
               fail("0..100");
    if (flag == "--local-wireless-reply-budget-us")
        return parse_u32_in_range(value, 25000, &out->reply_budget_us) ||
               fail("0..25000");
    if (flag == "--local-wireless-late-replies")
        return nds_parse_on_off(value, &out->late_replies) ||
               fail("on or off");
    if (flag == "--local-wireless-stay-awake")
        return nds_parse_on_off(value, &out->stay_awake) ||
               fail("on or off");
    if (flag == "--local-wireless-link") {
        if (value == "lan") out->lan = true;
        else if (value == "loopback") out->lan = false;
        else return fail("lan or loopback");
        return true;
    }
    if (flag == "--local-wireless-peer") {
        if (value.empty() || out->peers.size() >= kMaxLocalWirelessPeers)
            return fail("HOST or HOST:PORT, at most 8 peers");
        out->peers.push_back(value);
        return true;
    }
    if (flag == "--netplay")
        return nds_parse_on_off(value, &out->online.enabled) ||
               fail("on or off");
    if (flag == "--netplay-sign-in")
        return nds_parse_on_off(value, &out->online.sign_in) ||
               fail("on or off");
    if (flag == "--netplay-join") {
        if (value.empty()) return fail("a lobby id or first");
        out->online.join = value;
        return true;
    }
    if (flag == "--netplay-lobby-name") {
        out->online.lobby_name = value;
        return true;
    }
    if (flag == "--netplay-lobby-url") {
        if (value.rfind("ws://", 0) != 0) return fail("a ws:// URL");
        out->online.lobby_url = value;
        return true;
    }
    if (error) *error = "unknown flag " + flag;
    return false;
}

const char* nds_screen_layout_name(NdsScreenLayout value) {
    switch (value) {
        case NdsScreenLayout::Separate: return "separate";
        case NdsScreenLayout::Single: return "single";
        default: return "stacked";
    }
}

const char* nds_window_mode_name(NdsWindowMode value) {
    switch (value) {
        case NdsWindowMode::Stretch: return "stretch";
        case NdsWindowMode::Original: return "original";
        default: return "letterbox";
    }
}

const char* nds_scaling_filter_name(NdsScalingFilter value) {
    switch (value) {
        case NdsScalingFilter::Bilinear: return "bilinear";
        case NdsScalingFilter::Nearest: return "nearest";
        case NdsScalingFilter::Integer:
        default: return "integer";
    }
}

const char* nds_frame_interpolation_name(NdsFrameInterpolation value) {
    return value == NdsFrameInterpolation::Blend ? "blend" : "off";
}

const char* nds_fullscreen_mode_name(NdsFullscreenMode value) {
    switch (value) {
        case NdsFullscreenMode::Borderless: return "borderless";
        case NdsFullscreenMode::Exclusive: return "exclusive";
        default: return "off";
    }
}

const char* nds_startup_mode_name(NdsStartupMode value) {
    switch (value) {
        case NdsStartupMode::Manual: return "manual";
        case NdsStartupMode::Automatic: return "automatic";
        default: return "preserve";
    }
}

const char* nds_boot_mode_name(NdsBootMode value) {
    return value == NdsBootMode::Direct ? "direct" : "lle";
}

const char* nds_adaptive_screens_name(uint8_t value) {
    switch (value & NDS_ADAPTIVE_BOTH) {
        case NDS_ADAPTIVE_TOP: return "top";
        case NDS_ADAPTIVE_BOTTOM: return "bottom";
        case NDS_ADAPTIVE_BOTH: return "both";
        default: return "none";
    }
}

static bool apply_frontend_config(toml::table& root,
                                  NdsFrontendOptions* options,
                                  std::string* error);

bool nds_load_frontend_config(const std::string& path,
                              NdsFrontendOptions* options,
                              std::string* error) {
    if (!options) return false;
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        if (error) *error = e.description();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    return apply_frontend_config(root, options, error);
}

bool nds_load_frontend_config_text(const std::string& text,
                                   NdsFrontendOptions* options,
                                   std::string* error) {
    if (!options) return false;
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        if (error) *error = e.description();
        return false;
    }
    return apply_frontend_config(root, options, error);
}

bool nds_load_settings_file(const std::string& path,
                            NdsFrontendOptions* options,
                            std::string* error) {
    if (!options) return false;
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        if (error) {
            std::ostringstream message;
            message << "line " << e.source().begin.line << ": "
                    << e.description();
            *error = message.str();
        }
        return false;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    root.erase("game");
    root.erase("cartridge");
    if (const toml::table* paths = root["paths"].as_table()) {
        options->paths.rom = (*paths)["rom"].value_or(std::string());
        options->paths.bios = (*paths)["bios"].value_or(std::string());
        options->paths.save = (*paths)["save"].value_or(std::string());
    }
    return apply_frontend_config(root, options, error);
}

std::string nds_toml_string(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

namespace {

std::string trim_ascii(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1u);
}

// "[name]" on a line of its own, optionally followed by a comment.
bool settings_section_header(const std::string& line, std::string* name) {
    const std::string text = trim_ascii(line);
    if (text.size() < 3u || text[0] != '[' || text[1] == '[') return false;
    const size_t close = text.find(']');
    if (close == std::string::npos) return false;
    *name = trim_ascii(text.substr(1u, close - 1u));
    return true;
}

// Replace the value of a `key = value  # comment` line, comment included.
std::string with_settings_value(const std::string& line, size_t equals,
                                const std::string& value) {
    size_t begin = line.find_first_not_of(" \t", equals + 1u);
    if (begin == std::string::npos) begin = line.size();
    size_t end = begin;
    if (begin < line.size() && (line[begin] == '"' || line[begin] == '\'')) {
        const char quote = line[begin];
        end = begin + 1u;
        while (end < line.size() && line[end] != quote)
            end += line[end] == '\\' && quote == '"' ? 2u : 1u;
        end = std::min(end + 1u, line.size());
    } else {
        while (end < line.size() && line[end] != '#' && line[end] != ' ' &&
               line[end] != '\t' && line[end] != '\r')
            ++end;
    }
    return line.substr(0, equals) + "= " + value + line.substr(end);
}

}  // namespace

bool nds_update_settings_file(const std::string& path,
                              const std::vector<NdsSettingUpdate>& updates,
                              std::string* error) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    std::vector<bool> done(updates.size(), false);
    std::string section;
    std::vector<std::string> edited;
    edited.reserve(lines.size());
    for (const std::string& line : lines) {
        if (settings_section_header(line, &section)) {
            edited.push_back(line);
            continue;
        }
        const size_t equals = line.find('=');
        const size_t comment = line.find('#');
        bool removed = false;
        std::string out = line;
        if (equals != std::string::npos &&
            (comment == std::string::npos || comment > equals)) {
            const std::string key = trim_ascii(line.substr(0, equals));
            for (size_t i = 0; i < updates.size(); ++i) {
                if (updates[i].section != section || updates[i].key != key)
                    continue;
                done[i] = true;
                if (updates[i].remove)
                    removed = true;
                else
                    out = with_settings_value(line, equals, updates[i].value);
            }
        }
        if (!removed) edited.push_back(out);
    }

    for (size_t i = 0; i < updates.size(); ++i) {
        if (done[i] || updates[i].remove) continue;
        const std::string entry = updates[i].key + " = " + updates[i].value;
        // After the last non-blank line of the section, if it exists.
        std::string current;
        size_t insert = edited.size() + 1u;
        for (size_t line = 0; line < edited.size(); ++line) {
            if (settings_section_header(edited[line], &current)) {
                if (current == updates[i].section) insert = line + 1u;
            } else if (current == updates[i].section &&
                       !trim_ascii(edited[line]).empty()) {
                insert = line + 1u;
            }
        }
        if (insert > edited.size()) {
            if (!edited.empty() && !trim_ascii(edited.back()).empty())
                edited.push_back("");
            edited.push_back("[" + updates[i].section + "]");
            edited.push_back(entry);
        } else {
            edited.insert(edited.begin() + static_cast<std::ptrdiff_t>(insert),
                          entry);
        }
    }

    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const std::string& line : edited) out << line << '\n';
        out.flush();
        if (!out) {
            if (error) *error = "cannot write " + temporary;
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        if (error) *error = rename_error.message();
        return false;
    }
    return true;
}

static bool apply_frontend_config(toml::table& root,
                                  NdsFrontendOptions* options,
                                  std::string* error) {
    if (const toml::table* game = root["game"].as_table()) {
        if (const auto value = (*game)["name"].value<std::string>())
            options->game_name = *value;
        if (const auto value = (*game)["app_name"].value<std::string>())
            options->app_name = *value;
        if (const auto value = (*game)["sha1"].value<std::string>()) {
            const bool invalid_length = value->size() != 40u;
            const bool invalid_character = std::any_of(
                value->begin(), value->end(), [](unsigned char c) {
                    return !(c >= static_cast<unsigned char>('0') &&
                             c <= static_cast<unsigned char>('9')) &&
                           !(c >= static_cast<unsigned char>('a') &&
                             c <= static_cast<unsigned char>('f'));
                });
            if (invalid_length || invalid_character) {
                if (error) {
                    *error =
                        "game.sha1 must be exactly 40 lowercase hex digits";
                }
                return false;
            }
            options->expected_rom_sha1 = *value;
        }
    }

    if (const toml::table* system = root["system"].as_table()) {
        if (const auto value =
                (*system)["startup_mode"].value<std::string>()) {
            if (!nds_parse_startup_mode(*value, &options->startup_mode)) {
                if (error) {
                    *error =
                        "system.startup_mode must be preserve, manual, or "
                        "automatic";
                }
                return false;
            }
        }
        if (const auto value = (*system)["boot"].value<std::string>()) {
            if (!nds_parse_boot_mode(*value, &options->boot_mode)) {
                if (error) {
                    *error = "system.boot must be lle or direct";
                }
                return false;
            }
        }
        if (const auto value = (*system)["generated_firmware"].value<bool>()) {
            options->generated_firmware = *value;
        }
        if (const auto value = (*system)["freebios"].value<bool>()) {
            options->freebios = *value;
        }
        // beads-yjp.16: the firmware console nickname. Stored raw here and
        // validated once in main.cpp, together with NDS_PLAYER_NAME and
        // --player-name, so one rule set covers all three sources and the
        // error message can name the value that actually won.
        if (const auto value = (*system)["player_name"].value<std::string>()) {
            options->player_name = *value;
        }
        if (const auto value = (*system)["language"].value<std::string>()) {
            if (!nds_parse_firmware_language(*value,
                                             &options->firmware_language)) {
                if (error) {
                    *error =
                        "system.language must be preserve, japanese, english, "
                        "french, german, italian, spanish, or chinese";
                }
                return false;
            }
        }
        if (const auto value = (*system)["instance_index"].value<int64_t>()) {
            if (!nds_parse_instance_index(std::to_string(*value),
                                          &options->instance_index)) {
                if (error) {
                    *error = "system.instance_index must be 0..255";
                }
                return false;
            }
        }
    }

    if (const toml::table* audio = root["audio"].as_table()) {
        if (const auto value = (*audio)["volume"].value<int64_t>()) {
            if (*value < 0 || *value > 100) {
                if (error) *error = "audio.volume must be 0..100";
                return false;
            }
            options->audio_volume = static_cast<uint8_t>(*value);
        }
        if (const auto value = (*audio)["mute"].value<bool>())
            options->audio_muted = *value;
    }

    if (const toml::table* display = root["display"].as_table()) {
        if (const auto value = (*display)["discrete_gpu"].value<bool>())
            options->discrete_gpu = *value;
        if (const auto value = (*display)["fullscreen"].value<std::string>()) {
            if (!nds_parse_fullscreen_mode(*value, &options->fullscreen)) {
                if (error) {
                    *error =
                        "display.fullscreen must be off, borderless, or exclusive";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["screen_layout"].value<std::string>()) {
            if (!nds_parse_screen_layout(*value,
                                         &options->screen_layout)) {
                if (error) {
                    *error =
                        "display.screen_layout must be single, stacked, or "
                        "separate";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["window_mode"].value<std::string>()) {
            if (!nds_parse_window_mode(*value, &options->window_mode)) {
                if (error) {
                    *error =
                        "display.window_mode must be letterbox, stretch, or "
                        "original";
                }
                return false;
            }
        }
        if (const auto value = (*display)["scaling"].value<std::string>()) {
            if (!nds_parse_scaling_filter(*value, &options->scaling)) {
                if (error) {
                    *error = "display.scaling must be integer, bilinear, or nearest";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["adaptive_widescreen"].value<std::string>()) {
            if (!nds_parse_adaptive_screens(*value,
                                            &options->adaptive_screens)) {
                if (error) {
                    *error =
                        "display.adaptive_widescreen must be none, top, "
                        "bottom, or both";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["adaptive_capability"].value<std::string>()) {
            if (!nds_parse_adaptive_screens(*value,
                                            &options->adaptive_supported)) {
                if (error) {
                    *error =
                        "display.adaptive_capability must be none, top, "
                        "bottom, or both";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["adaptive_width"].value<int64_t>()) {
            uint16_t width = 0;
            if (!nds_parse_widescreen_width(std::to_string(*value), &width)) {
                if (error) {
                    *error =
                        "display.adaptive_width must be 256, 320, 384, "
                        "or 448";
                }
                return false;
            }
            if (options->adaptive_supported & NDS_ADAPTIVE_TOP)
                options->adaptive_max_width[0] = width;
            if (options->adaptive_supported & NDS_ADAPTIVE_BOTTOM)
                options->adaptive_max_width[1] = width;
        } else {
            if (options->adaptive_supported & NDS_ADAPTIVE_TOP)
                options->adaptive_max_width[0] = 448;
            if (options->adaptive_supported & NDS_ADAPTIVE_BOTTOM)
                options->adaptive_max_width[1] = 448;
        }
        if (const auto value =
                (*display)["adaptive_skybox_fill"].value<bool>()) {
            options->adaptive_skybox_fill = *value;
        }
        if (const auto value =
                (*display)["adaptive_hud_anchor"].value<bool>()) {
            options->adaptive_hud_anchor = *value;
        }
        if (const auto value =
                (*display)["adaptive_center_native"].value<bool>()) {
            options->adaptive_center_native = *value;
        }
        if (const auto value =
                (*display)["adaptive_guest_culling"].value<bool>()) {
            options->adaptive_guest_culling = *value;
        }
        if (const auto value =
                (*display)["adaptive_center_max_polygons"].value<int64_t>()) {
            if (*value < 0 || *value > 2048) {
                if (error) {
                    *error =
                        "display.adaptive_center_max_polygons must be from "
                        "0 through 2048";
                }
                return false;
            }
            options->adaptive_center_max_polygons =
                static_cast<uint32_t>(*value);
        }
        if (const auto value =
                (*display)["adaptive_hud_center_width"].value<int64_t>()) {
            if (*value < 8 || *value > 256 || (*value & 7) != 0) {
                if (error) {
                    *error =
                        "display.adaptive_hud_center_width must be a "
                        "multiple of 8 from 8 through 256";
                }
                return false;
            }
            options->adaptive_hud_center_width =
                static_cast<uint16_t>(*value);
        }
        if (options->adaptive_supported != NDS_ADAPTIVE_NONE &&
            options->expected_rom_sha1.empty()) {
            if (error) {
                *error =
                    "display.adaptive_capability requires an exact game.sha1";
            }
            return false;
        }
        if (const auto value =
                (*display)["supersampling"].value<int64_t>()) {
            if (!nds_parse_supersampling(std::to_string(*value),
                                         &options->supersampling)) {
                if (error) *error = "display.supersampling must be 1..4";
                return false;
            }
        }
        if (const auto value =
                (*display)["antialiasing"].value<int64_t>()) {
            if (!nds_parse_antialiasing(std::to_string(*value),
                                        &options->antialiasing)) {
                if (error) {
                    *error = "display.antialiasing must be 0, 2, 4, or 8";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["texture_upscale"].value<int64_t>()) {
            if (!nds_parse_texture_upscale(std::to_string(*value),
                                           &options->texture_upscale)) {
                if (error) *error = "display.texture_upscale must be 1, 2, or 4";
                return false;
            }
        }
        if (const auto value =
                (*display)["frame_interpolation"].value<std::string>()) {
            if (!nds_parse_frame_interpolation(
                    *value, &options->frame_interpolation)) {
                if (error) {
                    *error = "display.frame_interpolation must be off or blend";
                }
                return false;
            }
        }
        if (const auto value =
                (*display)["performance_governor"].value<std::string>()) {
            if (!nds_parse_perf_governor_mode(
                    value->c_str(), &options->perf_governor_mode)) {
                if (error) {
                    *error = "display.performance_governor must be auto, off, "
                             "stage1 or stage2";
                }
                return false;
            }
        }
        const auto internal_resolution =
            (*display)["internal_resolution"].value<int64_t>();
        const auto resolution_scale =
            (*display)["resolution_scale"].value<int64_t>();
        // resolution_scale = 0 is the runtime menu's "Off (fixed)": it keeps
        // whatever multiplier internal_resolution gives.
        const bool scale_off = resolution_scale && *resolution_scale == 0;
        if (internal_resolution && resolution_scale && !scale_off) {
            if (error) {
                *error = "display.internal_resolution and "
                         "display.resolution_scale are exclusive";
            }
            return false;
        }
        if (internal_resolution) {
            if (!nds_parse_internal_resolution(
                    std::to_string(*internal_resolution),
                    &options->internal_resolution)) {
                if (error) {
                    *error = "display.internal_resolution must be 1..16";
                }
                return false;
            }
            options->resolution_scale = 0;
        }
        if (scale_off) {
            options->resolution_scale = 0;
        } else if (resolution_scale) {
            if (!nds_parse_resolution_scale(
                    std::to_string(*resolution_scale),
                    &options->resolution_scale)) {
                if (error) {
                    *error = "display.resolution_scale must be 0 or 10..1000";
                }
                return false;
            }
            options->internal_resolution = 1;
        }
        if (const auto value =
                (*display)["second_screen_size"].value<int64_t>()) {
            if (!nds_parse_second_screen_size(
                    std::to_string(*value), &options->second_screen_size)) {
                if (error) {
                    *error = "display.second_screen_size must be 0..60";
                }
                return false;
            }
        }
        if (const auto value = (*display)["show_fps"].value<bool>())
            options->show_fps = *value;
        if (const auto value = (*display)["smooth_3d"].value<bool>())
            options->smooth_3d = *value;
        if (const auto value = (*display)["thin_outlines"].value<bool>())
            options->thin_outlines = *value;
    }

    if (const toml::table* cartridge = root["cartridge"].as_table()) {
        if (const auto value =
                (*cartridge)["save_type"].value<std::string>()) {
            if (!nds_parse_cartridge_save_type(
                    *value, &options->cartridge_save.type)) {
                if (error) {
                    *error =
                        "cartridge.save_type must be none, eeprom-tiny, "
                        "eeprom, or flash";
                }
                return false;
            }
        }
        if (const auto value = (*cartridge)["save_size"].value<int64_t>()) {
            if (*value < 0 || *value > (64ll * 1024ll * 1024ll)) {
                if (error) {
                    *error =
                        "cartridge.save_size must be between 0 and 67108864";
                }
                return false;
            }
            options->cartridge_save.size = static_cast<uint32_t>(*value);
        }
        const uint32_t size = options->cartridge_save.size;
        const bool power_of_two = size != 0u && (size & (size - 1u)) == 0u;
        if (options->cartridge_save.type == NdsCartridgeSaveType::None) {
            if (size != 0u) {
                if (error) {
                    *error =
                        "cartridge.save_size must be 0 when save_type is none";
                }
                return false;
            }
        } else if (!power_of_two) {
            if (error) {
                *error =
                    "cartridge.save_size must be a nonzero power of two";
            }
            return false;
        } else if (options->cartridge_save.type ==
                       NdsCartridgeSaveType::EepromTiny &&
                   size != 512u) {
            if (error) {
                *error =
                    "cartridge.save_size must be 512 for eeprom-tiny";
            }
            return false;
        }
    }

    if (const toml::table* network = root["network"].as_table()) {
        if (const auto value = (*network)["enabled"].value<bool>()) {
            options->network.enabled = *value;
        }
        if (const auto value = (*network)["backend"].value<std::string>()) {
            if (!nds_parse_network_backend(*value, &options->network.backend)) {
                if (error) *error = "network.backend must be slirp, replay, or pcap";
                return false;
            }
        }
        if (const auto value = (*network)["pcap_adapter"].value<std::string>()) {
            options->network.pcap_adapter = *value;
        }
        // Wiimmfi M8: capture/replay knobs, TOML mirror of the
        // --net-capture-* CLI flags (see frontend.h's NdsNetworkOptions).
        if (const toml::table* capture = (*network)["capture"].as_table()) {
            if (const auto value = (*capture)["out"].value<std::string>()) {
                options->network.capture_out = *value;
            }
            if (const auto value = (*capture)["in"].value<std::string>()) {
                options->network.capture_in = *value;
            }
            if (const auto value = (*capture)["raw"].value<bool>()) {
                options->network.capture_raw = *value;
            }
            if (const auto value = (*capture)["no_pcap"].value<bool>()) {
                options->network.capture_no_pcap = *value;
            }
            if (const auto value = (*capture)["scenario"].value<std::string>()) {
                options->network.capture_scenario = *value;
            }
        }
        if (const toml::table* wfc = (*network)["wfc"].as_table()) {
            if (const auto value = (*wfc)["enabled"].value<bool>()) {
                options->network.wfc_enabled = *value;
            }
            if (const auto value = (*wfc)["provider"].value<std::string>()) {
                options->network.wfc_provider.name = *value;
            }
            if (const auto value =
                    (*wfc)["clear_crt_errno_addr"].value<int64_t>()) {
                if (*value < 0 || *value > 0xFFFFFFFFll) {
                    if (error) {
                        *error = "network.wfc.clear_crt_errno_addr must be "
                                 "a 32-bit guest address";
                    }
                    return false;
                }
                options->network.wfc_clear_crt_errno_addr =
                    static_cast<uint32_t>(*value);
            }
            if (const auto value = (*wfc)["dns_server"].value<std::string>()) {
                uint32_t probe = 0;
                if (!nds_parse_ipv4(*value, &probe)) {
                    if (error) {
                        *error =
                            "network.wfc.dns_server must be a dotted-quad "
                            "IPv4 address";
                    }
                    return false;
                }
                options->network.wfc_provider.dns_server = *value;
            }
        }
    }
    if (const toml::table* local =
            root["local_wireless"].as_table()) {
        if (const auto value = (*local)["enabled"].value<bool>()) {
            options->local_wireless.enabled = *value;
        }
        if (const auto value = (*local)["base_port"].value<int64_t>()) {
            if (!nds_parse_local_wireless_base_port(
                    std::to_string(*value),
                    &options->local_wireless.base_port)) {
                if (error) {
                    *error =
                        "local_wireless.base_port must be 1024..65520";
                }
                return false;
            }
        }
        if (const auto value = (*local)["link"].value<std::string>()) {
            if (*value != "lan" && *value != "loopback") {
                if (error)
                    *error = "local_wireless.link must be lan or loopback";
                return false;
            }
            options->local_wireless.lan = *value == "lan";
        }
        if (const toml::array* peers = (*local)["peers"].as_array()) {
            options->local_wireless.peers.clear();
            for (const auto& entry : *peers) {
                const auto text = entry.value<std::string>();
                if (!text || text->empty() ||
                    options->local_wireless.peers.size() >=
                        kMaxLocalWirelessPeers) {
                    if (error) {
                        *error = "local_wireless.peers must be at most 8 "
                                 "\"HOST\" or \"HOST:PORT\" strings";
                    }
                    return false;
                }
                options->local_wireless.peers.push_back(*text);
            }
        }
    }
    if (const toml::table* online = root["online"].as_table()) {
        NdsOnlineOptions& o = options->local_wireless.online;
        if (const auto value = (*online)["enabled"].value<bool>())
            o.enabled = *value;
        if (const auto value = (*online)["lobby_url"].value<std::string>()) {
            if (!value->empty() && value->rfind("ws://", 0) != 0) {
                if (error) *error = "online.lobby_url must be a ws:// URL";
                return false;
            }
            o.lobby_url = *value;
        }
        if (const auto value = (*online)["game_name"].value<std::string>())
            o.game_name = *value;
        if (const auto value = (*online)["lobby_name"].value<std::string>())
            o.lobby_name = *value;
    }

    if (const toml::table* controls = root["controls"].as_table()) {
        if (const auto value =
                (*controls)["screen_toggle"].value<std::string>()) {
            if (!nds_set_virtual_stylus_binding(
                    &options->screen_toggle_binding, *value)) {
                if (error) *error = "controls.screen_toggle is too long";
                return false;
            }
        }
        if (const auto value =
                (*controls)["gamepad_layout"].value<std::string>()) {
            if (!nds_parse_gamepad_layout(*value,
                                          &options->gamepad_positional)) {
                if (error) {
                    *error = "controls.gamepad_layout must be labels or "
                             "positional";
                }
                return false;
            }
        }
        if (const auto value = (*controls)["mouse_sensitivity"].value<int64_t>()) {
            if (!nds_parse_mouse_sensitivity(
                    std::to_string(*value),
                    &options->relative_mouse_sensitivity)) {
                if (error) *error = "controls.mouse_sensitivity must be 10..400";
                return false;
            }
        }
        if (const toml::table* virtual_stylus =
                (*controls)["virtual_stylus"].as_table()) {
            if (const auto value =
                    (*virtual_stylus)["enabled"].value<bool>()) {
                options->virtual_stylus.enabled = *value;
            }
            if (const auto value =
                    (*virtual_stylus)["sensitivity"].value<int64_t>()) {
                if (!nds_parse_mouse_sensitivity(
                        std::to_string(*value),
                        &options->virtual_stylus.sensitivity)) {
                    if (error) {
                        *error =
                            "controls.virtual_stylus.sensitivity "
                            "must be 10..400";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*virtual_stylus)["pad_sensitivity"].value<int64_t>()) {
                if (!nds_parse_mouse_sensitivity(
                        std::to_string(*value),
                        &options->virtual_stylus.pad_sensitivity)) {
                    if (error) {
                        *error =
                            "controls.virtual_stylus.pad_sensitivity "
                            "must be 10..400";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*virtual_stylus)["binding"].value<std::string>()) {
                if (!nds_set_virtual_stylus_binding(
                        &options->virtual_stylus.binding, *value)) {
                    if (error) {
                        *error =
                            "controls.virtual_stylus.binding is too long";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*virtual_stylus)["tap_binding"].value<std::string>()) {
                if (!nds_set_virtual_stylus_binding(
                        &options->virtual_stylus.tap_binding, *value)) {
                    if (error) {
                        *error =
                            "controls.virtual_stylus.tap_binding is too long";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*virtual_stylus)["pad_hold_binding"].value<std::string>()) {
                if (!nds_set_virtual_stylus_binding(
                        &options->virtual_stylus.pad_hold_binding, *value)) {
                    if (error) {
                        *error =
                            "controls.virtual_stylus.pad_hold_binding "
                            "is too long";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*virtual_stylus)["pad_tap_binding"].value<std::string>()) {
                if (!nds_set_virtual_stylus_binding(
                        &options->virtual_stylus.pad_tap_binding, *value)) {
                    if (error) {
                        *error =
                            "controls.virtual_stylus.pad_tap_binding "
                            "is too long";
                    }
                    return false;
                }
            }
        }
        if (const toml::table* prime = (*controls)["prime"].as_table()) {
            if (const auto value = (*prime)["enabled"].value<bool>()) {
                options->mph_prime_controls = *value;
            }
            if (const auto value =
                    (*prime)["unified_window_focus"].value<bool>()) {
                options->mph_prime_unified_window_focus = *value;
            }
            if (const auto value =
                    (*prime)["virtual_stylus_sensitivity"].value<int64_t>()) {
                if (!nds_parse_mouse_sensitivity(std::to_string(*value),
                                                 &options->mph_virtual_stylus_sensitivity)) {
                    if (error) {
                        *error =
                            "controls.prime.virtual_stylus_sensitivity "
                            "must be 10..400";
                    }
                    return false;
                }
            }
            if (const auto value =
                    (*prime)["pad_aim_sensitivity"].value<int64_t>()) {
                if (!nds_parse_mouse_sensitivity(std::to_string(*value),
                                                 &options->mph_pad_aim_sensitivity)) {
                    if (error) {
                        *error =
                            "controls.prime.pad_aim_sensitivity "
                            "must be 10..400";
                    }
                    return false;
                }
            }
            if (const toml::table* bindings = (*prime)["bindings"].as_table()) {
                for (const auto& item : *bindings) {
                    const auto value = item.second.value<std::string>();
                    if (!value) continue;
                    if (!nds_set_mph_prime_binding(
                            options, std::string(item.first.str()), *value)) {
                        if (error) {
                            *error =
                                "controls.prime.bindings contains an "
                                "unknown binding name or too-long value";
                        }
                        return false;
                    }
                }
            }
            if (const toml::table* pad_bindings =
                    (*prime)["pad_bindings"].as_table()) {
                for (const auto& item : *pad_bindings) {
                    const auto value = item.second.value<std::string>();
                    if (!value) continue;
                    if (!nds_set_mph_prime_pad_binding(
                            options, std::string(item.first.str()), *value)) {
                        if (error) {
                            *error =
                                "controls.prime.pad_bindings contains an "
                                "unknown binding name or too-long value";
                        }
                        return false;
                    }
                }
            }
        }
    }
    if (const toml::table* title = root["title"].as_table()) {
        for (const auto& item : *title) {
            int value = 0;
            if (const auto b = item.second.value_exact<bool>()) {
                value = *b ? 1 : 0;
            } else if (const auto n = item.second.value_exact<int64_t>()) {
                if (*n < INT32_MIN || *n > INT32_MAX) {
                    if (error) *error = "title values must be 32-bit integers";
                    return false;
                }
                value = static_cast<int>(*n);
            } else {
                if (error) *error = "title values must be integers or booleans";
                return false;
            }
            options->title_settings.emplace_back(std::string(item.first.str()),
                                                 value);
        }
    }
    return true;
}
