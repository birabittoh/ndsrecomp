#include "player_launch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#if defined(NDS_HAVE_SDL3)
#include <SDL3/SDL.h>
#elif defined(NDS_HAVE_SDL2)
#include <SDL.h>
#endif

#include "sha1.h"

namespace fs = std::filesystem;

namespace {

const unsigned char kTitleConfig[] = {
#include "nds_title_config.inc"
};

constexpr const char* kBiosFiles[] = {
    "biosnds7.rom", "biosnds9.rom", "firmware.bin"};

std::string title_config() {
    return std::string(reinterpret_cast<const char*>(kTitleConfig),
                       sizeof(kTitleConfig) - 1u);
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

fs::path executable_dir() {
#if defined(__linux__)
    std::error_code error;
    const fs::path exe = fs::read_symlink("/proc/self/exe", error);
    if (!error) return exe.parent_path();
#elif defined(NDS_HAVE_SDL3)
    if (const char* base = SDL_GetBasePath()) return fs::path(base);
#elif defined(NDS_HAVE_SDL2)
    if (char* base = SDL_GetBasePath()) {
        const fs::path path(base);
        SDL_free(base);
        return path;
    }
#endif
    return fs::current_path();
}

fs::path data_directory(const std::string& app) {
    const fs::path exe_dir = executable_dir();
    if (fs::exists(exe_dir / "portable.txt")) return exe_dir;
#if defined(_WIN32)
#if defined(NDS_HAVE_SDL3)
    if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS))
        return fs::path(documents) / app;
#endif
    return fs::path(env_or_empty("USERPROFILE")) / "Documents" / app;
#elif defined(__APPLE__)
    return fs::path(env_or_empty("HOME")) / "Library" /
           "Application Support" / app;
#else
    const std::string xdg = env_or_empty("XDG_DATA_HOME");
    if (!xdg.empty()) return fs::path(xdg) / app;
    return fs::path(env_or_empty("HOME")) / ".local" / "share" / app;
#endif
}

// Stored paths are relative when they sit inside the data directory, so a
// portable folder keeps working after it is moved.
fs::path resolve_stored(const fs::path& root, const std::string& stored) {
    const fs::path path(stored);
    return path.is_absolute() ? path : root / path;
}

std::string to_stored(const fs::path& root, const fs::path& path) {
    const fs::path relative = path.lexically_relative(root);
    const bool inside = !relative.empty() && *relative.begin() != "..";
    return (inside ? relative : path).generic_string();
}

// The DS firmware languages a title can read, from the desktop's locale.
const char* locale_language() {
    std::string locale = env_or_empty("LC_ALL");
    if (locale.empty()) locale = env_or_empty("LC_MESSAGES");
    if (locale.empty()) locale = env_or_empty("LANG");
#if defined(NDS_HAVE_SDL3)
    if (locale.empty()) {
        int count = 0;
        if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
            if (count > 0 && locales[0]->language)
                locale = locales[0]->language;
            SDL_free(locales);
        }
    }
#endif
    const std::string code = locale.substr(0, 2);
    if (code == "en") return "english";
    if (code == "fr") return "french";
    if (code == "de") return "german";
    if (code == "it") return "italian";
    if (code == "es") return "spanish";
    return nullptr;
}

bool write_default_settings(const fs::path& path,
                            const NdsFrontendOptions& options) {
    const char* language = locale_language();
    const std::string language_name =
        language ? language
                 : nds_firmware_language_name(options.firmware_language);
    std::ofstream out(path);
    out << "# " << options.game_name << " settings.\n"
        << "# Edit this file with the game closed. The in-game menu (Esc)\n"
        << "# updates the values it covers and keeps everything else,\n"
        << "# comments included.\n"
        << "\n[paths]\n"
        << "# Empty means the default. Relative paths start from this "
           "folder.\n"
        << "rom = \"\"    # asked for on first launch, then remembered\n"
        << "bios = \"\"   # folder with biosnds7.rom, biosnds9.rom, "
           "firmware.bin\n"
        << "save = \"\"   # default: saves/" << options.app_name << ".sav\n"
        << "\n[display]\n"
        << "screen_layout = "
        << nds_toml_string(nds_screen_layout_name(options.screen_layout))
        << "   # single | stacked | separate\n"
        << "window_mode = "
        << nds_toml_string(nds_window_mode_name(options.window_mode))
        << "   # letterbox | stretch | original\n"
        << "scaling = "
        << nds_toml_string(nds_scaling_filter_name(options.scaling))
        << "   # nearest | bilinear | integer\n"
        << "fullscreen = "
        << nds_toml_string(nds_fullscreen_mode_name(options.fullscreen))
        << "   # off | borderless | exclusive\n"
        << "# 3D detail as a % of the window's resolution, 10..1000. 0 uses "
           "a fixed\n# multiplier: add internal_resolution = 1..16.\n"
        << "resolution_scale = " << options.resolution_scale << "\n"
        << "# The other screen beside the main one, as a % of its size "
           "(0..60; 0 hides it).\n"
        << "second_screen_size = "
        << static_cast<unsigned>(options.second_screen_size) << "\n"
        << "show_fps = " << (options.show_fps ? "true" : "false")
        << "   # 3D frame rate in the corner of the screen\n"
        << "# Hybrid-graphics laptops: run on the discrete GPU.\n"
        << "discrete_gpu = " << (options.discrete_gpu ? "true" : "false")
        << "\n"
        << "\n[system]\n"
        << "# Game text: english, french, german, italian or spanish.\n"
        << "language = " << nds_toml_string(language_name) << "\n"
        << "\n[audio]\n"
        << "volume = " << static_cast<unsigned>(options.audio_volume)
        << "   # 0..100\n"
        << "mute = " << (options.audio_muted ? "true" : "false") << "\n"
        << "\n[controls]\n"
        << "# positional: the pad's right button is DS A, as on a DS.\n"
        << "# labels: pad A is DS A.\n"
        << "gamepad_layout = "
        << nds_toml_string(options.gamepad_positional ? "positional"
                                                      : "labels")
        << "\n";
    out.flush();
    return static_cast<bool>(out);
}

// Same variables as the prime-run wrapper. They must be in place before SDL
// loads a GL driver. Anything the player set themselves stays.
void prefer_discrete_gpu() {
#if defined(__linux__)
    if (!fs::exists("/proc/driver/nvidia/version")) return;
    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 0);
    setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 0);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 0);
    std::fprintf(stderr, "[player] preferring the NVIDIA GPU\n");
#endif
}

enum class DialogResult { Picked, Cancelled, Unavailable };

#if defined(NDS_HAVE_SDL3)
struct DialogState {
    std::atomic<bool> done{false};
    DialogResult result = DialogResult::Cancelled;
    std::string path;
};

void SDLCALL dialog_finished(void* userdata, const char* const* files, int) {
    auto* state = static_cast<DialogState*>(userdata);
    if (!files) {
        state->result = DialogResult::Unavailable;
    } else if (files[0] && files[0][0]) {
        state->result = DialogResult::Picked;
        state->path = files[0];
    }
    state->done.store(true);
}

DialogResult run_dialog(SDL_FileDialogType type, const std::string& title,
                        std::string* path) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return DialogResult::Unavailable;
    const SDL_DialogFileFilter filter{"Nintendo DS ROM", "nds"};
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                          title.c_str());
    if (type == SDL_FILEDIALOG_OPENFILE) {
        SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                               const_cast<SDL_DialogFileFilter*>(&filter));
        SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                              1);
    }
    DialogState state;
    SDL_ShowFileDialogWithProperties(type, dialog_finished, &state, properties);
    while (!state.done.load()) {
        // SDL turns SIGTERM/SIGINT into a quit event. Nothing is running yet,
        // and the dialog's callback still points at `state`, so leave outright.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) std::_Exit(0);
        }
        SDL_Delay(10);
    }
    SDL_DestroyProperties(properties);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    *path = state.path;
    return state.result;
}

DialogResult pick_file(const std::string& title, std::string* path) {
    return run_dialog(SDL_FILEDIALOG_OPENFILE, title, path);
}
DialogResult pick_folder(const std::string& title, std::string* path) {
    return run_dialog(SDL_FILEDIALOG_OPENFOLDER, title, path);
}
#else
DialogResult pick_file(const std::string&, std::string*) {
    return DialogResult::Unavailable;
}
DialogResult pick_folder(const std::string&, std::string*) {
    return DialogResult::Unavailable;
}
#endif

bool rom_matches(const fs::path& path, const std::string& expected_sha1) {
    if (expected_sha1.empty()) return true;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return gba::sha1(bytes.data(), bytes.size()).hex() == expected_sha1;
}

bool bios_complete(const fs::path& dir) {
    return std::all_of(std::begin(kBiosFiles), std::end(kBiosFiles),
                       [&](const char* name) {
                           return fs::is_regular_file(dir / name);
                       });
}

bool remember(const fs::path& settings, const char* key,
              const std::string& value) {
    std::string error;
    if (nds_update_settings_file(settings.string(),
                                 {{"paths", key, nds_toml_string(value)}},
                                 &error))
        return true;
    std::fprintf(stderr, "[player] could not update %s: %s\n",
                 settings.string().c_str(), error.c_str());
    return false;
}

// False: the player cancelled (*cancelled), or an error was already shown.
bool resolve_rom(const fs::path& root, const fs::path& settings,
                 const NdsFrontendOptions& options, const char* argument,
                 fs::path* rom, bool* cancelled) {
    if (!argument && !options.paths.rom.empty()) {
        *rom = resolve_stored(root, options.paths.rom);
        if (fs::is_regular_file(*rom)) return true;
        std::fprintf(stderr, "[player] remembered ROM %s is gone\n",
                     rom->string().c_str());
    }
    std::string candidate = argument ? argument : "";
    for (;;) {
        if (candidate.empty()) {
            switch (pick_file("Select your " + options.game_name + " ROM",
                              &candidate)) {
                case DialogResult::Picked:
                    break;
                case DialogResult::Cancelled:
                    *cancelled = true;
                    return false;
                case DialogResult::Unavailable:
                    nds_player_error(
                        "No file picker is available. Start the game with "
                        "the ROM's path as its only argument.");
                    return false;
            }
        }
        const fs::path path = fs::absolute(candidate);
        if (rom_matches(path, options.expected_rom_sha1)) {
            *rom = path;
            remember(settings, "rom", to_stored(root, path));
            return true;
        }
        nds_player_error(path.filename().string() +
                         " is not the ROM this build runs: " +
                         options.game_name + ", SHA-1 " +
                         options.expected_rom_sha1 + ".");
        if (argument) return false;
        candidate.clear();
    }
}

bool resolve_bios(const fs::path& root, const fs::path& settings,
                  const NdsFrontendOptions& options, fs::path* bios,
                  bool* cancelled) {
    std::vector<fs::path> candidates;
    if (!options.paths.bios.empty())
        candidates.push_back(resolve_stored(root, options.paths.bios));
    candidates.push_back(root / "bios");
    candidates.push_back(executable_dir() / "bios");
#if defined(NDS_SOURCE_BIOS_DIR)
    candidates.push_back(NDS_SOURCE_BIOS_DIR);
#endif
    for (const fs::path& dir : candidates) {
        if (bios_complete(dir)) {
            *bios = dir.lexically_normal();
            return true;
        }
    }
    for (;;) {
        std::string picked;
        switch (pick_folder(
            "Select the folder with biosnds7.rom, biosnds9.rom and "
            "firmware.bin", &picked)) {
            case DialogResult::Picked:
                break;
            case DialogResult::Cancelled:
                *cancelled = true;
                return false;
            case DialogResult::Unavailable:
                nds_player_error("Put biosnds7.rom, biosnds9.rom and "
                                 "firmware.bin in " +
                                 (root / "bios").string() + ".");
                return false;
        }
        if (bios_complete(picked)) {
            *bios = picked;
            remember(settings, "bios", to_stored(root, picked));
            return true;
        }
        nds_player_error(picked + " must contain biosnds7.rom, biosnds9.rom "
                                  "and firmware.bin.");
    }
}

}  // namespace

bool nds_player_requested(int argc, char** argv) {
    if (sizeof(kTitleConfig) <= 1u) return false;
    if (argc == 1) return true;
    if (argc != 2 || argv[1][0] == '-') return false;
    std::string extension = fs::path(argv[1]).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return extension == ".nds";
}

void nds_player_error(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cannot start the game",
                             message.c_str(), nullptr);
#endif
}

bool nds_player_prepare(int argc, char** argv, NdsFrontendOptions* options,
                        NdsPlayerLaunch* out) {
    out->exit_code = 1;
    std::string error;
    if (!nds_load_frontend_config_text(title_config(), options, &error)) {
        nds_player_error("Invalid embedded game config: " + error);
        return false;
    }
    const std::string app =
        options->app_name.empty() ? "ndsrecomp" : options->app_name;
    options->app_name = app;
    const fs::path root = data_directory(app);
    const fs::path settings = root / "settings.toml";
    std::error_code fs_error;
    for (const char* sub : {"saves", "savestates", "logs"})
        fs::create_directories(root / sub, fs_error);
    if (fs_error) {
        nds_player_error("Cannot create " + root.string() + ": " +
                         fs_error.message());
        return false;
    }
    if (!fs::exists(settings) && !write_default_settings(settings, *options)) {
        nds_player_error("Cannot write " + settings.string());
        return false;
    }
    if (!nds_load_settings_file(settings.string(), options, &error)) {
        nds_player_error(settings.string() + ": " + error);
        return false;
    }
    options->settings_path = settings.string();
    std::fprintf(stderr, "[player] data directory: %s\n",
                 root.string().c_str());

    if (options->discrete_gpu) prefer_discrete_gpu();

    fs::path rom;
    fs::path bios;
    bool cancelled = false;
    if (!resolve_rom(root, settings, *options, argc == 2 ? argv[1] : nullptr,
                     &rom, &cancelled) ||
        !resolve_bios(root, settings, *options, &bios, &cancelled)) {
        if (cancelled) out->exit_code = 0;
        return false;
    }
    out->rom = rom.string();
    out->bios_dir = bios.string();
    out->firmware = (bios / "firmware.bin").string();
    out->save = options->paths.save.empty()
        ? (root / "saves" / (app + ".sav")).string()
        : resolve_stored(root, options->paths.save).string();
    out->savestate_dir = (root / "savestates").string();
    out->log_dir = (root / "logs").string();
    return true;
}
