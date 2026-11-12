#include "player_launch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>
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

#if defined(NDS_EMBEDDED_DUMPS)
const unsigned char kEmbeddedBios7[] = {
#include "embedded_biosnds7.rom.inc"
};
const unsigned char kEmbeddedBios9[] = {
#include "embedded_biosnds9.rom.inc"
};
const unsigned char kEmbeddedFirmware[] = {
#include "embedded_firmware.bin.inc"
};
#endif

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
#elif defined(__ANDROID__) && defined(NDS_HAVE_SDL3)
    // The app's folder on shared storage (Android/data/<package>/files),
    // which a file manager can reach, else its private one.
    if (const char* external = SDL_GetAndroidExternalStoragePath())
        return fs::path(external);
    if (const char* internal = SDL_GetAndroidInternalStoragePath())
        return fs::path(internal);
    return fs::current_path();
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
        << "rom = \"\"    # asked for on first launch, copied into this folder\n"
        << "bios = \"\"   # folder with biosnds7.rom, biosnds9.rom, "
           "firmware.bin\n"
        << "save = \"\"   # default: saves/" << options.app_name << ".sav\n"
        << "\n[display]\n"
        << "screen_layout = "
        << nds_toml_string(nds_screen_layout_name(options.screen_layout))
        << "   # single | stacked | separate\n"
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
           "(10..100).\n"
        << "second_screen_size = "
        << static_cast<unsigned>(options.second_screen_size) << "\n"
        << "show_fps = " << (options.show_fps ? "true" : "false")
        << "   # 3D frame rate in the corner of the screen\n"
        << "smooth_3d = " << (options.smooth_3d ? "true" : "false")
        << "   # blend 3D frames (experimental)\n"
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

#if defined(__ANDROID__) && defined(NDS_HAVE_SDL3)
// The Android picker hands out a content:// URI, which only SDL (through the
// ContentResolver) can open: copy it to `dest` so the rest reads a file.
bool import_uri(const std::string& uri, const fs::path& dest) {
    SDL_IOStream* in = SDL_IOFromFile(uri.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "[player] could not open %s: %s\n", uri.c_str(),
                     SDL_GetError());
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1u << 20);
    bool ok = static_cast<bool>(out);
    while (ok) {
        const size_t got = SDL_ReadIO(in, buffer.data(), buffer.size());
        if (got == 0) {
            ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
            break;
        }
        ok = static_cast<bool>(out.write(buffer.data(),
                                         static_cast<std::streamsize>(got)));
    }
    SDL_CloseIO(in);
    out.close();
    if (!ok) {
        std::fprintf(stderr, "[player] could not import %s to %s\n",
                     uri.c_str(), dest.string().c_str());
        fs::remove(dest, ec);
    }
    return ok;
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

// Which dumps the folder has to hold. system.freebios replaces the two BIOS
// dumps and system.generated_firmware replaces firmware.bin; neither is
// then asked for nor loaded.
struct DumpNeeds {
    bool bios;
    bool firmware;
    bool any() const { return bios || firmware; }
};

DumpNeeds dump_needs(const NdsFrontendOptions& options) {
    return {!options.freebios, !options.generated_firmware};
}

bool bios_complete(const fs::path& dir, DumpNeeds needs) {
    return std::all_of(std::begin(kBiosFiles), std::end(kBiosFiles),
                       [&](const char* name) {
                           const bool firmware =
                               std::strcmp(name, "firmware.bin") == 0;
                           if (firmware ? !needs.firmware : !needs.bios)
                               return true;
                           return fs::is_regular_file(dir / name);
                       });
}

const char* bios_files_wanted(DumpNeeds needs) {
    if (needs.bios && needs.firmware)
        return "biosnds7.rom, biosnds9.rom and firmware.bin";
    return needs.bios ? "biosnds7.rom and biosnds9.rom" : "firmware.bin";
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
        bool imported = false;
#if defined(__ANDROID__) && defined(NDS_HAVE_SDL3)
        // Straight to the copy the code below would make.
        if (candidate.rfind("content://", 0) == 0) {
            const fs::path copy = root / (options.app_name + ".nds");
            if (!import_uri(candidate, copy)) {
                nds_player_error("The selected file could not be read.");
                candidate.clear();
                continue;
            }
            candidate = copy.string();
            imported = true;
        }
#endif
        const fs::path path = fs::absolute(candidate);
        if (rom_matches(path, options.expected_rom_sha1)) {
            // Keep our own copy in the data folder and run from it, so the
            // original can be moved or deleted.
            const fs::path copy = root / (options.app_name + ".nds");
            std::error_code ec;
            if (fs::equivalent(path, copy, ec)) {
                *rom = copy;
            } else {
                ec.clear();
                fs::create_directories(root, ec);
                ec.clear();
                if (fs::copy_file(path, copy,
                                  fs::copy_options::overwrite_existing, ec)) {
                    *rom = copy;
                } else {
                    std::fprintf(stderr,
                                 "[player] could not copy the ROM to %s: %s\n",
                                 copy.string().c_str(), ec.message().c_str());
                    *rom = path;
                }
            }
            remember(settings, "rom", to_stored(root, *rom));
            return true;
        }
        if (imported) {
            std::error_code ec;
            fs::remove(path, ec);
        }
        nds_player_error((imported ? std::string("The selected file")
                                   : path.filename().string()) +
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
    const DumpNeeds needs = dump_needs(options);
    std::vector<fs::path> candidates;
    if (!options.paths.bios.empty())
        candidates.push_back(resolve_stored(root, options.paths.bios));
    candidates.push_back(root / "bios");
    candidates.push_back(executable_dir() / "bios");
#if defined(NDS_SOURCE_BIOS_DIR)
    candidates.push_back(NDS_SOURCE_BIOS_DIR);
#endif
    for (const fs::path& dir : candidates) {
        if (bios_complete(dir, needs)) {
            *bios = dir.lexically_normal();
            return true;
        }
    }
    for (;;) {
        std::string picked;
        switch (pick_folder(
            std::string("Select the folder with ") +
                bios_files_wanted(needs), &picked)) {
            case DialogResult::Picked:
                break;
            case DialogResult::Cancelled:
                *cancelled = true;
                return false;
            case DialogResult::Unavailable:
                nds_player_error(std::string("Put ") +
                                 bios_files_wanted(needs) + " in " +
                                 (root / "bios").string() + ".");
                return false;
        }
        if (bios_complete(picked, needs)) {
            *bios = picked;
            remember(settings, "bios", to_stored(root, picked));
            return true;
        }
        nds_player_error(picked + " must contain " +
                         bios_files_wanted(needs) + ".");
    }
}

// `--instance N`: one of several copies of the game on this machine,
// linked by local wireless. Each instance is its own console: its own guest
// MAC and UDP port (both derived from N), and from instance 1 on its own
// save, savestates and logs, so two processes never write the same file.
// The save starts as a copy of the main one.
bool apply_instance(int instance, const fs::path& main_save,
                    NdsFrontendOptions* options, NdsPlayerLaunch* out) {
    options->instance_index = static_cast<uint32_t>(instance);
    options->local_wireless.enabled = true;
    std::fprintf(stderr, "[player] instance %d, local wireless on\n",
                 instance);
    if (instance == 0) return true;

    const std::string suffix = "instance-" + std::to_string(instance);
    fs::path save = main_save;
    save.replace_filename(main_save.stem().string() + "." + suffix +
                          main_save.extension().string());
    std::error_code error;
    if (!fs::exists(save) && fs::exists(main_save)) {
        fs::copy_file(main_save, save, error);
        if (error) {
            nds_player_error("Cannot copy " + main_save.string() + " to " +
                             save.string() + ": " + error.message());
            return false;
        }
        std::fprintf(stderr, "[player] copied %s to %s\n",
                     main_save.string().c_str(), save.string().c_str());
    }
    const fs::path savestates = fs::path(out->savestate_dir) / suffix;
    const fs::path logs = fs::path(out->log_dir) / suffix;
    fs::create_directories(savestates, error);
    fs::create_directories(logs, error);
    if (error) {
        nds_player_error("Cannot create " + savestates.string() + ": " +
                         error.message());
        return false;
    }
    out->save = save.string();
    out->savestate_dir = savestates.string();
    out->log_dir = logs.string();
    std::fprintf(stderr, "[player] instance save: %s\n", out->save.c_str());
    return true;
}

}  // namespace

bool nds_player_embedded_dumps(std::vector<uint8_t>* arm9_bios,
                               std::vector<uint8_t>* arm7_bios,
                               std::vector<uint8_t>* firmware) {
#if defined(NDS_EMBEDDED_DUMPS)
    arm9_bios->assign(std::begin(kEmbeddedBios9), std::end(kEmbeddedBios9));
    arm7_bios->assign(std::begin(kEmbeddedBios7), std::end(kEmbeddedBios7));
    firmware->assign(std::begin(kEmbeddedFirmware),
                     std::end(kEmbeddedFirmware));
    return true;
#else
    (void)arm9_bios; (void)arm7_bios; (void)firmware;
    return false;
#endif
}

namespace {

// Player-mode command line: an optional .nds path and an optional
// `--instance N`, in any order. `instance` stays -1 without the flag.
struct PlayerArgs {
    const char* rom = nullptr;
    int instance = -1;
    // Latency-lab flags (nds_is_local_wireless_tuning_flag), applied after
    // the settings file so the command line wins.
    std::vector<std::pair<std::string, std::string>> local_wireless_tuning;
};

bool is_nds_path(const char* arg) {
    std::string extension = fs::path(arg).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return extension == ".nds";
}

bool parse_player_args(int argc, char** argv, PlayerArgs* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--instance" && i + 1 < argc) {
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 0 || value > 15)
                return false;
            out->instance = static_cast<int>(value);
        } else if (nds_is_local_wireless_tuning_flag(arg) && i + 1 < argc) {
            out->local_wireless_tuning.emplace_back(arg, argv[++i]);
        } else if (arg[0] != '-' && !out->rom && is_nds_path(argv[i])) {
            out->rom = argv[i];
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

bool nds_player_requested(int argc, char** argv) {
    if (sizeof(kTitleConfig) <= 1u) return false;
    PlayerArgs args;
    return parse_player_args(argc, argv, &args);
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
    // Logs sit next to the executable whatever the data directory is, so a
    // bug report never means hunting through Documents. Not on Android,
    // where the "executable" is the system's read-only app_process.
#if defined(__ANDROID__)
    const fs::path logs = root / "logs";
#else
    const fs::path logs = executable_dir() / "logs";
#endif
    for (const fs::path& dir : {root / "saves", root / "savestates", logs})
        fs::create_directories(dir, fs_error);
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
#if defined(NDS_EMBEDDED_DUMPS)
    // Nothing to ask for: the dumps are in the executable. The data
    // directory stands in for the bios folder (the generated identity
    // file, if any, lives there).
    const bool embedded = true;
    bios = root;
    const bool have_bios = true;
#else
    const bool embedded = false;
    // FreeBIOS with generated firmware loads no dump at all: nothing to ask
    // for, and the data directory holds the generated identity file.
    const bool have_bios = !dump_needs(*options).any();
    if (have_bios) bios = root;
#endif
    PlayerArgs args;
    parse_player_args(argc, argv, &args);
    for (const auto& [flag, value] : args.local_wireless_tuning) {
        if (!nds_parse_local_wireless_tuning(flag, value,
                                             &options->local_wireless,
                                             &error)) {
            nds_player_error(error);
            return false;
        }
    }
    if (!resolve_rom(root, settings, *options, args.rom, &rom, &cancelled) ||
        (!have_bios &&
         !resolve_bios(root, settings, *options, &bios, &cancelled))) {
        if (cancelled) out->exit_code = 0;
        return false;
    }
    out->rom = rom.string();
    out->bios_dir = bios.string();
    // A generated image replaces the dump; passing the dump's path too
    // would make main refuse --generated-firmware with --firmware-path.
    out->firmware = embedded || options->generated_firmware
        ? std::string()
        : (bios / "firmware.bin").string();
    out->embedded_dumps = embedded;
    const fs::path save = options->paths.save.empty()
        ? root / "saves" / (app + ".sav")
        : resolve_stored(root, options->paths.save);
    out->save = save.string();
    out->savestate_dir = (root / "savestates").string();
    out->log_dir = logs.string();
    out->data_dir = root.string();
    if (args.instance >= 0 && !apply_instance(args.instance, save, options,
                                              out)) {
        return false;
    }
    return true;
}
