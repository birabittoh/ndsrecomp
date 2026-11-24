#include "player_launch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#endif

#if defined(NDS_HAVE_SDL3)
#include <SDL3/SDL.h>
#elif defined(NDS_HAVE_SDL2)
#include <SDL.h>
#endif

#include "sha1.h"
#include "rom_folder.h"
#include "release_recipe.h"
#include "rom_title.h"

namespace fs = std::filesystem;

namespace {

const unsigned char kTitleConfig[] = {
#include "nds_title_config.inc"
};

#include "nds_release_recipes.inc"

// The built-in recipe that rebuilds the expected image's code from `sha1`.
const NdsBuiltinRecipe* recipe_for(const std::string& sha1,
                                   const std::string& expected) {
    for (const NdsBuiltinRecipe* r = kReleaseRecipes; r->data; ++r) {
        NdsReleaseRecipeInfo info;
        if (nds_release_recipe_info(r->data, r->size, &info, nullptr) &&
            info.applies_to == sha1 && info.runs_as == expected)
            return r;
    }
    return nullptr;
}

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

// The player's prompts and errors, in the system's language when it is one
// of the DS firmware's five, else in English. {key} placeholders are filled
// in by tr().
enum class Text {
    AskRom, NoRomSelected, ChooseRom, Quit, PickRomTitle, ErrorTitle,
    FileUnreadable, SelectedFile, NotThisRom, NotThisFolder, NoPicker,
    CannotUnpack, CannotConvert, PickBiosTitle, PutBios, BiosMissing, And,
    Count
};

const char* const kTexts[][static_cast<int>(Text::Count)] = {
    {   // English
        "To play, select your own {game} ROM (a .nds file) on the next "
        "screen.",
        "No ROM was selected.",
        "Choose ROM",
        "Quit",
        "Select your {game} ROM",
        "Cannot start the game",
        "The selected file could not be read.",
        "The selected file",
        "{file} is not the ROM this build runs: {game}, SHA-1 {sha1}.",
        "{path} is not the unpacked ROM this build runs.",
        "No file picker is available. Start the game with the ROM's path as "
        "its only argument.",
        "Cannot unpack the ROM: {error}",
        "Cannot convert the ROM: {error}",
        "Select the folder with {files}",
        "Put {files} in {dir}.",
        "{dir} must contain {files}.",
        "and",
    },
    {   // French
        "Pour jouer, sélectionnez votre propre ROM de {game} (un fichier "
        ".nds) sur l'écran suivant.",
        "Aucune ROM n'a été sélectionnée.",
        "Choisir la ROM",
        "Quitter",
        "Sélectionnez votre ROM de {game}",
        "Impossible de lancer le jeu",
        "Le fichier sélectionné n'a pas pu être lu.",
        "Le fichier sélectionné",
        "{file} n'est pas la ROM que cette version utilise : {game}, SHA-1 "
        "{sha1}.",
        "{path} n'est pas la ROM décompressée que cette version utilise.",
        "Aucun sélecteur de fichiers n'est disponible. Lancez le jeu avec le "
        "chemin de la ROM comme seul argument.",
        "Impossible de décompresser la ROM : {error}",
        "Impossible de convertir la ROM : {error}",
        "Sélectionnez le dossier contenant {files}",
        "Placez {files} dans {dir}.",
        "{dir} doit contenir {files}.",
        "et",
    },
    {   // German
        "Wähle zum Spielen auf dem nächsten Bildschirm dein eigenes ROM von "
        "{game} (eine .nds-Datei) aus.",
        "Es wurde kein ROM ausgewählt.",
        "ROM auswählen",
        "Beenden",
        "Wähle dein ROM von {game} aus",
        "Das Spiel kann nicht gestartet werden",
        "Die ausgewählte Datei konnte nicht gelesen werden.",
        "Die ausgewählte Datei",
        "{file} ist nicht das ROM, das diese Version ausführt: {game}, SHA-1 "
        "{sha1}.",
        "{path} ist nicht das entpackte ROM, das diese Version ausführt.",
        "Es ist keine Dateiauswahl verfügbar. Starte das Spiel mit dem Pfad "
        "des ROMs als einzigem Argument.",
        "Das ROM kann nicht entpackt werden: {error}",
        "Das ROM kann nicht umgewandelt werden: {error}",
        "Wähle den Ordner mit {files} aus",
        "Lege {files} in {dir} ab.",
        "{dir} muss {files} enthalten.",
        "und",
    },
    {   // Italian
        "Per giocare, seleziona la tua ROM di {game} (un file .nds) nella "
        "schermata successiva.",
        "Nessuna ROM selezionata.",
        "Scegli ROM",
        "Esci",
        "Seleziona la tua ROM di {game}",
        "Impossibile avviare il gioco",
        "Impossibile leggere il file selezionato.",
        "Il file selezionato",
        "{file} non è la ROM che questa versione esegue: {game}, SHA-1 "
        "{sha1}.",
        "{path} non è la ROM estratta che questa versione esegue.",
        "Nessun selettore di file disponibile. Avvia il gioco con il percorso "
        "della ROM come unico argomento.",
        "Impossibile estrarre la ROM: {error}",
        "Impossibile convertire la ROM: {error}",
        "Seleziona la cartella con {files}",
        "Metti {files} in {dir}.",
        "{dir} deve contenere {files}.",
        "e",
    },
    {   // Spanish
        "Para jugar, selecciona tu propia ROM de {game} (un archivo .nds) en "
        "la siguiente pantalla.",
        "No se ha seleccionado ninguna ROM.",
        "Elegir ROM",
        "Salir",
        "Selecciona tu ROM de {game}",
        "No se puede iniciar el juego",
        "No se ha podido leer el archivo seleccionado.",
        "El archivo seleccionado",
        "{file} no es la ROM que ejecuta esta versión: {game}, SHA-1 "
        "{sha1}.",
        "{path} no es la ROM descomprimida que ejecuta esta versión.",
        "No hay ningún selector de archivos disponible. Inicia el juego con "
        "la ruta de la ROM como único argumento.",
        "No se puede descomprimir la ROM: {error}",
        "No se puede convertir la ROM: {error}",
        "Selecciona la carpeta con {files}",
        "Coloca {files} en {dir}.",
        "{dir} debe contener {files}.",
        "y",
    },
};

int text_language() {
    static const int language = [] {
        const char* name = locale_language();
        const char* const names[] = {"english", "french", "german",
                                     "italian", "spanish"};
        for (int i = 0; i < 5; ++i)
            if (name && std::strcmp(name, names[i]) == 0) return i;
        return 0;
    }();
    return language;
}

std::string tr(Text text,
               std::initializer_list<std::pair<const char*, std::string>>
                   values = {}) {
    std::string out = kTexts[text_language()][static_cast<int>(text)];
    for (const auto& [key, value] : values) {
        const std::string token = std::string("{") + key + "}";
        for (size_t at = out.find(token); at != std::string::npos;
             at = out.find(token, at + value.size()))
            out.replace(at, token.size(), value);
    }
    return out;
}

bool write_default_settings(const fs::path& path,
                            const NdsFrontendOptions& options) {
    std::ofstream out(path);
    out << "# " << nds_game_display_name(options) << " settings.\n"
        << "# Edit this file with the game closed. The in-game menu (Esc)\n"
        << "# updates the values it covers and keeps everything else,\n"
        << "# comments included.\n"
        << "\n[paths]\n"
        << "# Empty means the default. Relative paths start from this "
           "folder.\n"
        << "rom = \"\"    # asked for on first launch, unpacked into assets/\n"
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
        << "# Game text: system (the OS language, checked at every "
           "launch),\n# english, french, german, italian or spanish.\n"
        << "language = \"system\"\n"
        << "\n[audio]\n"
        << "volume = " << static_cast<unsigned>(options.audio_volume)
        << "   # 0..100\n"
        << "mute = " << (options.audio_muted ? "true" : "false") << "\n"
        << "fast_forward_volume = "
        << static_cast<unsigned>(options.audio_fast_forward_volume)
        << "   # percent of volume while fast-forwarding, 0 mutes\n"
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

#if defined(NDS_HAVE_SDL3)
// Says what the file picker is for before it opens (the Android one has no
// title, so on its own it looks like the app handed over to the file
// manager), and comes back here when it is cancelled. False: the player
// chose to quit.
#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1,
                                         nullptr, 0);
    std::wstring wide(size > 0 ? size - 1 : 0, L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

// SDL's Windows message box only has the stock icons: a TaskDialog (Common
// Controls v6, from the embedded manifest) shows the .exe's own, the
// cartridge banner icon (resource 101, CMakeLists.txt). -1: unavailable.
int task_dialog_choice(const std::string& title, const std::string& message,
                       const char* yes, const char* no) {
    using TaskDialogIndirectFn =
        HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    const HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    const auto task_dialog = comctl
        ? reinterpret_cast<TaskDialogIndirectFn>(
              GetProcAddress(comctl, "TaskDialogIndirect"))
        : nullptr;
    using LoadIconMetricFn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, HICON*);
    const auto load_icon_metric = comctl
        ? reinterpret_cast<LoadIconMetricFn>(
              GetProcAddress(comctl, "LoadIconMetric"))
        : nullptr;
    if (!task_dialog) return -1;
    const std::wstring wtitle = widen(title), wmessage = widen(message),
                       wyes = widen(yes), wno = widen(no);
    const TASKDIALOG_BUTTON buttons[] = {{IDOK, wyes.c_str()},
                                         {IDCANCEL, wno.c_str()}};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = wtitle.c_str();
    config.pszContent = wmessage.c_str();
    config.pButtons = buttons;
    config.cButtons = 2;
    config.nDefaultButton = IDOK;
    HICON icon = nullptr;
    if (load_icon_metric &&
        SUCCEEDED(load_icon_metric(config.hInstance, MAKEINTRESOURCEW(101),
                                   LIM_LARGE, &icon))) {
        config.dwFlags |= TDF_USE_HICON_MAIN;
        config.hMainIcon = icon;
    } else {
        config.pszMainIcon = TD_INFORMATION_ICON;
    }
    int pressed = 0;
    const HRESULT result = task_dialog(&config, &pressed, nullptr, nullptr);
    if (icon) DestroyIcon(icon);
    if (FAILED(result)) return -1;
    return pressed == IDOK ? 1 : 0;
}
#endif

bool ask_to_pick_rom(const NdsFrontendOptions& options, bool again) {
    const std::string name = nds_game_display_name(options);
    const std::string message =
        (again ? tr(Text::NoRomSelected) + "\n\n" : std::string()) +
        tr(Text::AskRom, {{"game", name}});
    const std::string choose = tr(Text::ChooseRom), quit = tr(Text::Quit);
#if defined(_WIN32)
    const int choice =
        task_dialog_choice(name, message, choose.c_str(), quit.c_str());
    if (choice >= 0) return choice == 1;
#endif
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, quit.c_str()},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, choose.c_str()},
    };
    const SDL_MessageBoxData data{
        SDL_MESSAGEBOX_INFORMATION, nullptr, name.c_str(), message.c_str(),
        static_cast<int>(SDL_arraysize(buttons)), buttons, nullptr};
    int chosen = 0;
    if (!SDL_ShowMessageBox(&data, &chosen)) return true;  // straight on
    return chosen == 1;
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

bool read_bytes(const fs::path& path, std::vector<uint8_t>* bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes->assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
    return true;
}

// An unpacked ROM folder made from the image this build runs, or converted
// to run as it (rom.toml identity).
bool folder_matches(const fs::path& dir, const std::string& expected_sha1) {
    if (!nds_rom_folder_is(dir)) return false;
    NdsRomFolderInfo info;
    std::string error;
    if (!nds_rom_folder_info(dir, &info, &error)) {
        std::fprintf(stderr, "[player] %s\n", error.c_str());
        return false;
    }
    return expected_sha1.empty() || info.sha1 == expected_sha1 ||
           info.identity == expected_sha1;
}

// The library folder for an image: its [[game.releases]] id, else its game
// code. An existing folder holding another image is left alone.
fs::path release_dir(const fs::path& library, const NdsFrontendOptions& options,
                     const std::vector<uint8_t>& rom, const std::string& sha1) {
    std::string id;
    for (const NdsRelease& release : options.releases)
        if (release.sha1 == sha1) id = release.id;
    if (id.empty()) {
        for (size_t i = 0x0C; i < 0x10 && i < rom.size(); ++i) {
            const unsigned char c = rom[i];
            id.push_back(std::isalnum(c) ? static_cast<char>(std::tolower(c))
                                         : '_');
        }
        if (rom.size() > 0x1E && rom[0x1E])
            id += "-rev" + std::to_string(rom[0x1E]);
    }
    fs::path dir = library / id;
    for (int n = 2; fs::exists(dir) && !folder_matches(dir, sha1); ++n)
        dir = library / (id + "-" + std::to_string(n));
    return dir;
}

enum class Install { Done, NotThisGame, Failed };

// A folder unpacked from `sha1` and, if `identity` is set, converted to run
// as it, its data prepared (if at all) by this build's step.
bool folder_is(const fs::path& dir, const std::string& sha1,
               const std::string& identity) {
    if (!nds_rom_folder_is(dir)) return false;
    NdsRomFolderInfo info;
    if (!nds_rom_folder_info(dir, &info, nullptr) || info.sha1 != sha1 ||
        info.identity != identity)
        return false;
    if (!info.prepared.empty() &&
        info.prepared != nds_release_prepare_version()) {
        std::fprintf(stderr, "[player] %s was prepared by another build, "
                             "converting it again\n", dir.string().c_str());
        return false;
    }
    return true;
}

// Unpacks `rom` into the library, or finds it already there. Another release
// with a built-in recipe is unpacked and its code rebuilt (release_recipe.h).
Install install_rom(const fs::path& library, const NdsFrontendOptions& options,
                    const std::vector<uint8_t>& rom, fs::path* dir) {
    const std::string sha1 = gba::sha1(rom.data(), rom.size()).hex();
    const std::string& expected = options.expected_rom_sha1;
    const NdsBuiltinRecipe* recipe =
        expected.empty() || sha1 == expected ? nullptr
                                             : recipe_for(sha1, expected);
    if (!expected.empty() && sha1 != expected && !recipe)
        return Install::NotThisGame;
    *dir = release_dir(library, options, rom, sha1);
    if (recipe ? folder_is(*dir, sha1, expected) : folder_matches(*dir, sha1))
        return Install::Done;
    std::error_code ec;
    fs::create_directories(library, ec);
    std::string error;
    if (!recipe) {
        std::fprintf(stderr, "[player] unpacking the ROM to %s\n",
                     dir->string().c_str());
        if (!nds_rom_folder_unpack(rom, *dir, &error)) {
            nds_player_error(tr(Text::CannotUnpack, {{"error", error}}));
            return Install::Failed;
        }
        return Install::Done;
    }
    // A plain unpack of this image already in place is converted as it is.
    const bool unpacked = folder_is(*dir, sha1, "");
    fs::path source = *dir;
    if (!unpacked) {
        source += ".unpacked";
        fs::remove_all(source, ec);
        std::fprintf(stderr, "[player] unpacking the ROM to %s\n",
                     source.string().c_str());
        if (!nds_rom_folder_unpack(rom, source, &error)) {
            nds_player_error(tr(Text::CannotUnpack, {{"error", error}}));
            return Install::Failed;
        }
    }
    fs::path converted = *dir;
    converted += ".converted";
    fs::remove_all(converted, ec);
    std::fprintf(stderr, "[player] rebuilding the code of %s to run as %s\n",
                 source.string().c_str(), expected.c_str());
    if (!nds_release_convert_folder(source, converted, recipe->data,
                                    recipe->size, &error)) {
        fs::remove_all(converted, ec);
        if (!unpacked) fs::remove_all(source, ec);
        nds_player_error(tr(Text::CannotConvert, {{"error", error}}));
        return Install::Failed;
    }
    fs::remove_all(source, ec);
    fs::remove_all(*dir, ec);
    fs::rename(converted, *dir, ec);
    if (ec) {
        nds_player_error("Cannot create " + dir->string() + ": " + ec.message());
        return Install::Failed;
    }
    return Install::Done;
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

std::string bios_files_wanted(DumpNeeds needs) {
    const std::string conjunction = " " + tr(Text::And) + " ";
    if (needs.bios && needs.firmware)
        return "biosnds7.rom, biosnds9.rom" + conjunction + "firmware.bin";
    return needs.bios ? "biosnds7.rom" + conjunction + "biosnds9.rom"
                      : std::string("firmware.bin");
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

// The ROM is kept unpacked, one folder per release, in <data>/assets
// (rom_folder.h); paths.rom names the folder in use. A picked .nds is
// unpacked there and can then be moved or deleted. A folder can also be
// picked or given on the command line, and is then used where it is.
// False: the player cancelled (*cancelled), or an error was already shown.
bool resolve_rom(const fs::path& root, const fs::path& settings,
                 const NdsFrontendOptions& options, const char* argument,
                 fs::path* rom, bool* cancelled) {
    const fs::path library = root / "assets";
    const std::string& expected = options.expected_rom_sha1;
    const fs::path old_copy = root / (options.app_name + ".nds");
    if (!argument && !options.paths.rom.empty()) {
        const fs::path stored = resolve_stored(root, options.paths.rom);
        if (folder_matches(stored, expected)) {
            *rom = stored;
            return true;
        }
        std::vector<uint8_t> bytes;
        if (fs::is_regular_file(stored) && read_bytes(stored, &bytes)) {
            // A .nds from before ROMs were unpacked.
            const Install install = install_rom(library, options, bytes, rom);
            if (install == Install::Failed) return false;
            if (install == Install::Done) {
                remember(settings, "rom", to_stored(root, *rom));
                std::error_code ec;
                if (fs::equivalent(stored, old_copy, ec)) {
                    fs::remove(old_copy, ec);
                    std::fprintf(stderr, "[player] removed %s\n",
                                 old_copy.string().c_str());
                }
                return true;
            }
        }
        std::fprintf(stderr, "[player] remembered ROM %s is gone\n",
                     stored.string().c_str());
    }
    if (!argument && !expected.empty()) {
        // A folder unpacked earlier, or next to the executable (a build).
        for (const fs::path& dir : {library, executable_dir() / "assets"}) {
            std::error_code ec;
            for (const fs::directory_entry& entry :
                 fs::directory_iterator(dir, ec)) {
                if (!folder_matches(entry.path(), expected)) continue;
                *rom = entry.path();
                remember(settings, "rom", to_stored(root, *rom));
                return true;
            }
        }
    }
    std::string candidate = argument ? argument : "";
#if defined(NDS_HAVE_SDL3)
    bool asked_before = false;
#endif
    for (;;) {
        if (candidate.empty()) {
#if defined(NDS_HAVE_SDL3)
            if (!ask_to_pick_rom(options, asked_before)) {
                *cancelled = true;
                return false;
            }
#endif
            switch (pick_file(tr(Text::PickRomTitle,
                                 {{"game", nds_game_display_name(options)}}),
                              &candidate)) {
                case DialogResult::Picked:
                    break;
                case DialogResult::Cancelled:
#if defined(NDS_HAVE_SDL3)
                    asked_before = true;
                    continue;
#else
                    *cancelled = true;
                    return false;
#endif
                case DialogResult::Unavailable:
                    nds_player_error(tr(Text::NoPicker));
                    return false;
            }
        }
        bool imported = false;
#if defined(__ANDROID__) && defined(NDS_HAVE_SDL3)
        // A file the code below can read, unpacked and then removed.
        if (candidate.rfind("content://", 0) == 0) {
            const fs::path copy = root / (options.app_name + ".import.nds");
            if (!import_uri(candidate, copy)) {
                nds_player_error(tr(Text::FileUnreadable));
                candidate.clear();
                continue;
            }
            candidate = copy.string();
            imported = true;
        }
#endif
        const fs::path path = fs::absolute(candidate);
        if (fs::is_directory(path)) {
            if (folder_matches(path, expected)) {
                *rom = path;
                remember(settings, "rom", to_stored(root, *rom));
                return true;
            }
            nds_player_error(
                tr(Text::NotThisFolder, {{"path", path.string()}}));
            if (argument) return false;
            candidate.clear();
            continue;
        }
        std::vector<uint8_t> bytes;
        const bool read = read_bytes(path, &bytes);
        if (imported) {
            std::error_code ec;
            fs::remove(path, ec);
        }
        if (read) {
            const Install install = install_rom(library, options, bytes, rom);
            if (install == Install::Failed) return false;
            if (install == Install::Done) {
                remember(settings, "rom", to_stored(root, *rom));
                return true;
            }
        }
        nds_player_error(tr(
            Text::NotThisRom,
            {{"file", imported ? tr(Text::SelectedFile)
                               : path.filename().string()},
             {"game", nds_game_display_name(options)},
             {"sha1", expected}}));
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
            tr(Text::PickBiosTitle, {{"files", bios_files_wanted(needs)}}),
            &picked)) {
            case DialogResult::Picked:
                break;
            case DialogResult::Cancelled:
                *cancelled = true;
                return false;
            case DialogResult::Unavailable:
                nds_player_error(
                    tr(Text::PutBios, {{"files", bios_files_wanted(needs)},
                                       {"dir", (root / "bios").string()}}));
                return false;
        }
        if (bios_complete(picked, needs)) {
            *bios = picked;
            remember(settings, "bios", to_stored(root, picked));
            return true;
        }
        nds_player_error(tr(Text::BiosMissing,
                            {{"dir", picked},
                             {"files", bios_files_wanted(needs)}}));
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

// Player-mode command line: an optional .nds path (or unpacked ROM
// folder) and an optional
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
        } else if (arg[0] != '-' && !out->rom &&
                   (is_nds_path(argv[i]) || nds_rom_folder_is(argv[i]))) {
            out->rom = argv[i];
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

void nds_resolve_system_language(NdsFrontendOptions* options) {
    if (options->firmware_language != NdsFirmwareLanguage::System) return;
    options->firmware_language = NdsFirmwareLanguage::Preserve;
    if (const char* name = locale_language()) {
        nds_parse_firmware_language(name, &options->firmware_language);
        std::fprintf(stderr, "[player] system language: %s\n", name);
    }
}

bool nds_player_requested(int argc, char** argv) {
    if (sizeof(kTitleConfig) <= 1u) return false;
    PlayerArgs args;
    return parse_player_args(argc, argv, &args);
}

void nds_player_error(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
#if defined(NDS_HAVE_SDL3) || defined(NDS_HAVE_SDL2)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                             tr(Text::ErrorTitle).c_str(), message.c_str(),
                             nullptr);
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
    for (const fs::path& dir : {root / "saves", root / "savestates"}) {
        fs::create_directories(dir, fs_error);
        if (fs_error) {
            nds_player_error("Cannot create " + dir.string() + ": " +
                             fs_error.message());
            return false;
        }
    }
    // Logs sit next to the executable whatever the data directory is, so a
    // bug report never means hunting through Documents. Not on Android,
    // where the "executable" is the system's read-only app_process, nor on
    // macOS, where it is inside the .app bundle: Gatekeeper runs a
    // downloaded app from a read-only translocated copy (as does opening it
    // straight from the DMG), and writing into a signed bundle breaks its
    // signature. Anywhere else, a read-only install falls back to the data
    // directory too.
#if defined(__ANDROID__) || defined(__APPLE__)
    fs::path logs = root / "logs";
#else
    fs::path logs = executable_dir() / "logs";
    fs::create_directories(logs, fs_error);
    if (fs_error) {
        std::fprintf(stderr, "[player] cannot create %s (%s), logging to the "
                             "data directory\n",
                     logs.string().c_str(), fs_error.message().c_str());
        fs_error.clear();
        logs = root / "logs";
    }
#endif
    fs::create_directories(logs, fs_error);
    if (fs_error) {
        nds_player_error("Cannot create " + logs.string() + ": " +
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
    nds_resolve_system_language(options);
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

// ---------------------------------------------------------------------------
// Headless content preparation (--prepare-content), for external launchers.

namespace {

constexpr const char* kOwnerFile = ".recomplauncher-content";
constexpr const char* kOwnerText = "ndsrecomp content v1\n";

bool owned(const fs::path& dir) {
    std::ifstream in(dir / kOwnerFile, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {}) == kOwnerText;
}

bool mark_owned(const fs::path& dir) {
    std::ofstream out(dir / kOwnerFile, std::ios::binary);
    out << kOwnerText;
    out.close();
    return static_cast<bool>(out);
}

void report(double fraction, const char* text) {
    std::printf("progress %.3f %s\n", fraction, text);
    std::fflush(stdout);
}

// The arguments as paths. On Windows main's argv is in the ANSI code page,
// so read the UTF-16 command line instead.
std::vector<fs::path> path_args(int argc, char** argv) {
    std::vector<fs::path> args;
#if defined(_WIN32)
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 0; i < count; ++i) args.emplace_back(wide[i]);
        LocalFree(wide);
        if (count == argc) return args;
        args.clear();
    }
#endif
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

// The cartridge image the source holds: a .nds, or a folder unpacked from
// one (a folder already converted to another release's code is refused,
// since it no longer packs into the image it came from).
std::string read_source(const fs::path& source, std::vector<uint8_t>* rom) {
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        NdsRomFolderInfo info;
        std::string error;
        if (!nds_rom_folder_is(source))
            return "The folder is not an unpacked ROM (it has no rom.toml).";
        if (!nds_rom_folder_info(source, &info, &error)) return error;
        if (!info.identity.empty())
            return "This folder was converted from another release. Select "
                   "the original ROM instead.";
        if (!nds_rom_folder_pack(source, rom, &error)) return error;
        return {};
    }
    if (!fs::is_regular_file(source, ec) || !read_bytes(source, rom))
        return "Cannot read " + source.string() + ".";
    return {};
}

// Writes the folder the game runs from for `rom` to `dest`, which must not
// exist: unpacked, and its code rebuilt when `recipe` is set.
std::string build_folder(const std::vector<uint8_t>& rom,
                         const NdsBuiltinRecipe* recipe, const fs::path& dest) {
    std::string error;
    if (!recipe) {
        report(0.1, "Unpacking the ROM");
        if (!nds_rom_folder_unpack(rom, dest, &error))
            return "Cannot unpack the ROM: " + error;
        return {};
    }
    fs::path unpacked = dest;
    unpacked += ".unpacked";
    // Left over by a run stopped while converting.
    std::error_code ec;
    fs::remove_all(unpacked, ec);
    report(0.1, "Unpacking the ROM");
    if (!nds_rom_folder_unpack(rom, unpacked, &error))
        return "Cannot unpack the ROM: " + error;
    report(0.5, "Rebuilding the game code");
    const bool ok = nds_release_convert_folder(unpacked, dest, recipe->data,
                                               recipe->size, &error);
    fs::remove_all(unpacked, ec);
    if (!ok) return "Cannot convert the ROM: " + error;
    return {};
}

std::string prepare_content(const fs::path& source_arg,
                            const fs::path& destination_arg) {
    NdsFrontendOptions options;
    std::string error;
    if (!nds_load_frontend_config_text(title_config(), &options, &error))
        return "Invalid embedded game config: " + error;
    const std::string& expected = options.expected_rom_sha1;
    const std::string name = nds_game_display_name(options);

    std::error_code ec;
    const fs::path source = fs::absolute(source_arg, ec);
    const fs::path destination =
        fs::weakly_canonical(fs::absolute(destination_arg, ec), ec);
    if (ec || destination.filename().empty() ||
        destination == destination.root_path())
        return "The destination is not a usable folder.";
    if (fs::is_symlink(fs::symlink_status(destination_arg, ec)))
        return "The destination must not be a symbolic link.";
    if (fs::exists(destination, ec) &&
        (!fs::is_directory(destination, ec) || !owned(destination)))
        return "The destination already exists and was not prepared by this "
               "command.";
    for (fs::path at = destination; !at.empty(); at = at.parent_path()) {
        if (fs::equivalent(at, source, ec))
            return "The source and destination must be separate.";
        if (at == at.parent_path()) break;
    }

    report(0, "Identifying your copy");
    std::vector<uint8_t> rom;
    error = read_source(source, &rom);
    if (!error.empty()) return error;
    const std::string sha1 = gba::sha1(rom.data(), rom.size()).hex();
    const NdsBuiltinRecipe* recipe =
        expected.empty() || sha1 == expected ? nullptr
                                             : recipe_for(sha1, expected);
    if (!expected.empty() && sha1 != expected && !recipe)
        return "This is not a ROM this build runs: " + name + ", SHA-1 " +
               expected + " (got " + sha1 + ").";

    // Everything is built beside the destination and swapped in at the end,
    // so a failure leaves the previous copy as it was.
    fs::path work = destination;
    work += ".preparing";
    if (fs::exists(fs::symlink_status(work, ec))) {
        if (!fs::is_directory(work, ec) || !owned(work))
            return work.string() + " is in the way. Move it aside and retry.";
        const fs::path old = work / "old";
        if (fs::exists(old, ec) && !fs::exists(destination, ec))
            fs::rename(old, destination, ec);  // an interrupted swap
        fs::remove_all(work, ec);
        if (ec) return "Cannot remove " + work.string() + ": " + ec.message();
    }
    fs::create_directories(work, ec);
    if (ec || !mark_owned(work))
        return "Cannot create " + work.string() + ".";
    const fs::path fresh = work / "new";
    const fs::path old = work / "old";

    bool moved_old = false;
    error = build_folder(rom, recipe, fresh);
    if (error.empty() && !mark_owned(fresh))
        error = "Cannot write to " + fresh.string() + ".";
    if (error.empty()) {
        report(0.95, "Activating the game files");
        if (fs::exists(destination, ec)) {
            fs::rename(destination, old, ec);
            if (ec) error = "Cannot replace " + destination.string() + ": " +
                            ec.message();
            else moved_old = true;
        }
    }
    if (error.empty()) {
        fs::rename(fresh, destination, ec);
        if (ec) error = "Cannot create " + destination.string() + ": " +
                        ec.message();
    }
    if (!error.empty()) {
        if (moved_old) {
            fs::rename(old, destination, ec);
            if (ec) return error + " The previous copy is in " + old.string();
        }
        fs::remove_all(work, ec);
        return error;
    }
    fs::remove_all(work, ec);
    report(1, "Ready to play");
    return {};
}

}  // namespace

int nds_prepare_content_command(int argc, char** argv) {
    bool requested = false;
    for (int i = 1; i < argc; ++i)
        requested |= std::strcmp(argv[i], "--prepare-content") == 0;
    if (!requested) return -1;
    const std::vector<fs::path> args = path_args(argc, argv);
    fs::path source, destination;
    bool invalid = sizeof(kTitleConfig) <= 1u;
    for (int i = 1; i < argc && !invalid; ++i) {
        const std::string arg = argv[i];
        if (arg == "--prepare-content") continue;
        fs::path* value = arg == "--source"        ? &source
                        : arg == "--destination"   ? &destination
                                                   : nullptr;
        if (!value || i + 1 >= argc || !value->empty() ||
            std::strncmp(argv[i + 1], "--", 2) == 0)
            invalid = true;
        else
            *value = args[++i];
    }
    if (invalid || source.empty() || destination.empty()) {
        std::fprintf(stderr, "usage: %s --prepare-content --source <rom.nds "
                             "or unpacked folder> --destination <folder>\n",
                     argv[0]);
        return 2;
    }
    const std::string error = prepare_content(source, destination);
    if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
