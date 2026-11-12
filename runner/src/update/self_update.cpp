// update/self_update.cpp -- the platform-independent half of the
// self-updater (check, download, verify, extract, stage). See
// update/self_update.h; ApplyAndRestart() lives in self_update_<platform>.cpp.
#include "update/self_update.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#endif

#include <nlohmann/json.hpp>
#include <picosha2.h>

#include "toml.hpp"
#include "update/archive.h"
#include "update/http.h"

#ifndef NDS_UPDATE_REPO
#define NDS_UPDATE_REPO ""
#endif
#ifndef NDS_UPDATE_ASSET_FORMAT
#define NDS_UPDATE_ASSET_FORMAT ""
#endif
#ifndef NDS_RELEASE_VERSION
#define NDS_RELEASE_VERSION ""
#endif

namespace nds::update {

namespace {

using nlohmann::json;

std::string ReplaceAll(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// Overlays `source` (the user's existing TOML) onto `target` (the one the
// new release ships), so the shipped file's shape and new keys win by
// default but every key the user already had keeps its value. Keys the user
// had that the new file no longer defines are kept too.
void MergeTomlOnto(toml::table& target, const toml::table& source) {
    for (const auto& [key, value] : source) {
        if (value.is_table() && target.contains(key) && target[key].is_table()) {
            MergeTomlOnto(*target[key].as_table(), *value.as_table());
        } else {
            target.insert_or_assign(key, value);
        }
    }
}

// Merges every top-level *.toml the staged content ships with the same-named
// file already at `install_root` (if any), so values the user set in an
// installed TOML survive an update that also ships a new default one.
// Best effort: a parse failure on either side leaves the shipped file as is.
void MergeShippedToml(const std::filesystem::path& install_root,
                      const std::filesystem::path& content_root) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(content_root, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".toml")
            continue;
        const auto existing_path = install_root / entry.path().filename();
        if (!std::filesystem::is_regular_file(existing_path, ec))
            continue;  // nothing installed there yet: the shipped defaults stand
        try {
            auto shipped = toml::parse_file(entry.path().string());
            const auto existing = toml::parse_file(existing_path.string());
            MergeTomlOnto(shipped, existing);
            std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
            out << shipped;
        } catch (const toml::parse_error& e) {
            std::fprintf(stderr, "[update] could not merge %s: %s\n",
                         entry.path().string().c_str(),
                         std::string(e.description()).c_str());
        }
    }
}

// The release asset's extension. macOS ships a .dmg rather than a tarball
// because the .app has to keep a working code signature, which the tar
// reader here (no symlinks, no permission bits) would not preserve. Android
// ships the signed APK itself, handed to the system installer as it is.
std::string_view AssetExtension() {
#if defined(_WIN32)
    return ".zip";
#elif defined(__APPLE__)
    return ".dmg";
#elif defined(__ANDROID__)
    return ".apk";
#else
    return ".tar.gz";
#endif
}

bool ParseVersionComponents(std::string_view version, std::vector<int>& out) {
    out.clear();
    std::string current;
    auto flush = [&]() -> bool {
        if (current.empty() ||
            !std::all_of(current.begin(), current.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return false;
        }
        out.push_back(std::stoi(current));
        current.clear();
        return true;
    };
    for (const char c : version) {
        if (c == '.') {
            if (!flush()) return false;
        } else {
            current += c;
        }
    }
    return flush();
}

}  // namespace

Config BuildConfig() {
    return {NDS_UPDATE_REPO, NDS_UPDATE_ASSET_FORMAT, NDS_RELEASE_VERSION};
}

std::string HostPlatformId() {
#if defined(_WIN32)
    return "win-amd64";
#elif defined(__APPLE__)
    // Before the architecture branches below: an arm64 mac is __aarch64__
    // too, and would otherwise ask for the Linux asset.
#if defined(__aarch64__) || defined(_M_ARM64)
    return "mac-arm64";
#else
    return "mac-amd64";
#endif
#elif defined(__ANDROID__)
    // Also ahead of the architecture branches: Android is aarch64 too.
#if defined(__aarch64__) || defined(_M_ARM64)
    return "android-arm64";
#else
    return "android-amd64";
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "linux-arm64";
#else
    return "linux-amd64";
#endif
}

int CompareVersionStrings(std::string_view have, std::string_view want) {
    std::vector<int> have_parts;
    std::vector<int> want_parts;
    ParseVersionComponents(have, have_parts);
    ParseVersionComponents(want, want_parts);
    const size_t n = std::max(have_parts.size(), want_parts.size());
    for (size_t i = 0; i < n; ++i) {
        const int hv = i < have_parts.size() ? have_parts[i] : 0;
        const int wv = i < want_parts.size() ? want_parts[i] : 0;
        if (hv != wv) return hv - wv;
    }
    return 0;
}

std::filesystem::path SelfUpdater::ExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) return {};
    buffer.resize(len);
    return std::filesystem::path(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::strlen(buffer.c_str()));
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(buffer, ec);
    return ec ? std::filesystem::path(buffer) : canonical;
#elif defined(__ANDROID__)
    return {};
#else
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : exe;
#endif
}

std::filesystem::path SelfUpdater::EnclosingAppBundle(
    const std::filesystem::path& executable_path) {
    const auto macos_dir = executable_path.parent_path();
    if (macos_dir.filename() != "MacOS") return {};
    const auto contents_dir = macos_dir.parent_path();
    if (contents_dir.filename() != "Contents") return {};
    auto bundle = contents_dir.parent_path();
    if (bundle.extension() != ".app") return {};
    return bundle;
}

std::filesystem::path SelfUpdater::InstallRoot() {
#if defined(__ANDROID__)
    const char* files = SDL_GetAndroidInternalStoragePath();
    return files ? std::filesystem::path(files) : std::filesystem::path();
#else
    const auto exe = ExecutablePath();
    if (exe.empty()) return {};
    const auto bundle = EnclosingAppBundle(exe);
    return bundle.empty() ? exe.parent_path() : bundle.parent_path();
#endif
}

bool SelfUpdater::SupportsSelfUpdate() {
    // In step with runner/CMakeLists.txt, which only builds an
    // ApplyAndRestart for these platforms.
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

bool SelfUpdater::RestartsItself() {
#if defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

bool SelfUpdater::Enabled() const {
    return SupportsSelfUpdate() && !config_.repo.empty() &&
           !config_.asset_format.empty() && !config_.current_version.empty();
}

std::string SelfUpdater::ExpandAssetFormat(const std::string& format,
                                           const std::string& tag) {
    std::string result = ReplaceAll(format, "{tag}", tag);
    return ReplaceAll(result, "{platform}", HostPlatformId());
}

SelfUpdater::~SelfUpdater() {
    if (check_thread_.joinable()) check_thread_.join();
    if (install_thread_.joinable()) install_thread_.join();
}

void SelfUpdater::CheckAsync() {
    bool expected = false;
    if (!check_in_flight_.compare_exchange_strong(expected, true)) return;
    if (check_thread_.joinable()) check_thread_.join();

    if (!Enabled()) {
        // Turned off at build time, or nothing to compare a tag against:
        // settle straight to kFailed without a request.
        state_.store(CheckState::kFailed, std::memory_order_release);
        check_in_flight_.store(false, std::memory_order_release);
        return;
    }

    state_.store(CheckState::kChecking, std::memory_order_release);
    check_thread_ = std::thread([this, repo = config_.repo,
                                 format = config_.asset_format,
                                 current = config_.current_version] {
        CheckWorker(repo, format, current);
        check_in_flight_.store(false, std::memory_order_release);
    });
}

void SelfUpdater::CheckWorker(std::string repo, std::string format,
                              std::string current_version) {
    const std::string url = "https://api.github.com/repos/" + repo + "/releases/latest";
    const auto response = net::HttpGet(url);
    if (!response.ok()) {
        std::fprintf(stderr, "[update] release lookup failed: %s\n",
                     response.error.empty()
                         ? ("HTTP " + std::to_string(response.status)).c_str()
                         : response.error.c_str());
        state_.store(CheckState::kFailed, std::memory_order_release);
        return;
    }

    try {
        const json parsed = json::parse(response.body);
        const std::string tag = parsed.value("tag_name", "");
        if (tag.empty() || !parsed.contains("assets") || !parsed["assets"].is_array()) {
            std::fprintf(stderr, "[update] malformed release response (no tag_name/assets)\n");
            state_.store(CheckState::kFailed, std::memory_order_release);
            return;
        }

        const std::string expected_name =
            ExpandAssetFormat(format, tag) + std::string(AssetExtension());

        UpdateInfo info;
        for (const auto& asset : parsed["assets"]) {
            if (asset.value("name", "") != expected_name) continue;
            info.asset_name = expected_name;
            info.asset_url = asset.value("browser_download_url", "");
            const std::string digest = asset.value("digest", "");
            constexpr std::string_view kSha256Prefix = "sha256:";
            if (digest.rfind(kSha256Prefix, 0) == 0)
                info.sha256 = digest.substr(kSha256Prefix.size());
            break;
        }
        if (info.asset_url.empty()) {
            std::fprintf(stderr, "[update] release '%s' has no asset named '%s'\n",
                         tag.c_str(), expected_name.c_str());
            state_.store(CheckState::kFailed, std::memory_order_release);
            return;
        }

        info.tag = tag;
        info.version = (tag.front() == 'v' || tag.front() == 'V') ? tag.substr(1) : tag;
        info.notes = parsed.value("body", "");

        if (CompareVersionStrings(current_version, info.version) >= 0) {
            std::fprintf(stderr, "[update] %s is up to date (latest %s)\n",
                         current_version.c_str(), info.version.c_str());
            state_.store(CheckState::kUpToDate, std::memory_order_release);
            return;
        }

        std::fprintf(stderr, "[update] %s is available (running %s)\n",
                     info.version.c_str(), current_version.c_str());
        {
            std::lock_guard<std::mutex> lock(info_mutex_);
            available_ = info;
        }
        state_.store(CheckState::kUpdateAvailable, std::memory_order_release);
    } catch (const json::exception& e) {
        std::fprintf(stderr, "[update] could not parse the release response: %s\n",
                     e.what());
        state_.store(CheckState::kFailed, std::memory_order_release);
    }
}

std::optional<UpdateInfo> SelfUpdater::Available() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return available_;
}

std::filesystem::path SelfUpdater::StagingRoot(const std::filesystem::path& install_root) {
    return install_root / ".pending-self-update";
}

bool SelfUpdater::HasPendingSelfUpdate(const std::filesystem::path& install_root,
                                       const std::string& current_version) {
    std::error_code ec;
    const auto staging = StagingRoot(install_root);
    if (!std::filesystem::is_directory(staging, ec)) return false;
#if defined(__ANDROID__)
    // The system installer applies the APK, so nothing here ever clears the
    // staging folder. Once the running build is at least the staged
    // version, the file is stale (installed, or updated another way).
    if (!current_version.empty()) {
        const auto staged = StagedApkPath(install_root);
        if (staged.empty() ||
            CompareVersionStrings(current_version, StagedApkVersion(staged)) >= 0) {
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    }
#else
    (void)current_version;
#endif
    return std::filesystem::directory_iterator(staging, ec) !=
           std::filesystem::directory_iterator();
}

std::filesystem::path SelfUpdater::StagedApkPath(const std::filesystem::path& install_root) {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(StagingRoot(install_root), ec)) {
        if (entry.path().extension() == ".apk") return entry.path();
    }
    return {};
}

std::string SelfUpdater::StagedApkVersion(const std::filesystem::path& apk) {
    // Staged as "update-<version>.apk" by InstallWorker.
    const std::string stem = apk.stem().string();
    constexpr std::string_view kPrefix = "update-";
    return stem.rfind(kPrefix, 0) == 0 ? stem.substr(kPrefix.size()) : std::string();
}

void SelfUpdater::InstallAsync(const UpdateInfo& info,
                               const std::filesystem::path& install_root) {
    if (!SupportsSelfUpdate()) return;  // nothing could apply what this stages
    bool expected = false;
    if (!install_in_flight_.compare_exchange_strong(expected, true)) return;
    if (install_thread_.joinable()) install_thread_.join();
    {
        std::lock_guard<std::mutex> lock(install_mutex_);
        install_result_ = InstallResult{};
        install_result_.in_progress = true;
    }
    install_thread_ = std::thread([this, info, install_root] {
        InstallWorker(info, install_root);
        install_in_flight_.store(false, std::memory_order_release);
    });
}

void SelfUpdater::InstallWorker(UpdateInfo info, std::filesystem::path install_root) {
    auto fail = [this](std::string message) {
        std::fprintf(stderr, "[update] install failed: %s\n", message.c_str());
        std::lock_guard<std::mutex> lock(install_mutex_);
        install_result_.in_progress = false;
        install_result_.done = true;
        install_result_.ok = false;
        install_result_.message = std::move(message);
    };

    std::error_code ec;
    const auto temp_root = install_root / ".auto-update-download";
    std::filesystem::create_directories(temp_root, ec);
    if (ec) {
        fail("failed to create download folder: " + ec.message());
        return;
    }

    // Named after the asset's real suffix, which path::extension() cannot
    // give for ".tar.gz" (it only sees ".gz").
    auto ends_with = [&info](std::string_view suffix) {
        return info.asset_name.size() > suffix.size() &&
               info.asset_name.compare(info.asset_name.size() - suffix.size(),
                                       suffix.size(), suffix) == 0;
    };
    const bool is_tar_gz = ends_with(".tar.gz");
    const bool is_dmg = ends_with(".dmg");
    const bool is_apk = ends_with(".apk");
    const auto temp_archive =
        temp_root / (is_tar_gz ? "download.tar.gz"
                     : is_dmg  ? "download.dmg"
                     : is_apk  ? "download.apk"
                               : "download.zip");

    std::fprintf(stderr, "[update] downloading %s\n", info.asset_url.c_str());
    std::string download_error;
    auto progress = [this](uint64_t downloaded, uint64_t total) {
        std::lock_guard<std::mutex> lock(install_mutex_);
        install_result_.downloaded_bytes = downloaded;
        install_result_.total_bytes = total;
    };
    if (!net::HttpDownloadToFile(info.asset_url, temp_archive, progress, download_error)) {
        std::filesystem::remove_all(temp_root, ec);
        fail("download failed: " + download_error);
        return;
    }

    if (!info.sha256.empty()) {
        std::ifstream file(temp_archive, std::ios::binary);
        std::vector<unsigned char> hash(picosha2::k_digest_size);
        picosha2::hash256(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>(), hash.begin(), hash.end());
        const std::string actual = picosha2::bytes_to_hex_string(hash.begin(), hash.end());
        file.close();
        std::string expected_lower = info.sha256;
        std::transform(expected_lower.begin(), expected_lower.end(),
                       expected_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (actual != expected_lower) {
            std::filesystem::remove_all(temp_root, ec);
            fail("checksum mismatch: expected " + info.sha256 + ", got " + actual +
                 "; the release asset may have changed. Refusing to install.");
            return;
        }
    }

    const auto staging = StagingRoot(install_root);
    if (is_apk) {
        // Nothing to unpack: the installer takes the APK whole. The version
        // in the name lets HasPendingSelfUpdate() tell a stale one apart.
        std::filesystem::remove_all(staging, ec);
        std::filesystem::create_directories(staging, ec);
        std::filesystem::rename(temp_archive, staging / ("update-" + info.version + ".apk"), ec);
        std::error_code ignored;
        std::filesystem::remove_all(temp_root, ignored);
        if (ec) {
            fail("failed to stage update: " + ec.message());
            return;
        }
        std::lock_guard<std::mutex> lock(install_mutex_);
        install_result_.in_progress = false;
        install_result_.done = true;
        install_result_.ok = true;
        install_result_.message = "Downloaded v" + info.version;
        return;
    }

    const auto extracted = temp_root / "extracted";
    std::filesystem::remove_all(extracted, ec);
    std::string extract_error;
    bool extracted_ok = false;
    if (is_dmg) {
#if defined(__APPLE__)
        extracted_ok = ExtractDmg(temp_archive, extracted, extract_error);
#else
        extract_error = "disk images can only be opened on macOS";
#endif
    } else if (is_tar_gz) {
        extracted_ok = archive::ExtractTarGz(temp_archive, extracted, extract_error);
    } else {
        extracted_ok = archive::ExtractZip(temp_archive, extracted, extract_error);
    }
    if (!extracted_ok) {
        std::filesystem::remove_all(temp_root, ec);
        fail("extract failed: " + extract_error);
        return;
    }
    std::filesystem::remove(temp_archive, ec);

    // Unwrap a single top-level directory if the archive was packed that way.
    std::filesystem::path content_root = extracted;
    {
        std::vector<std::filesystem::directory_entry> top_level;
        for (const auto& e : std::filesystem::directory_iterator(extracted, ec))
            top_level.push_back(e);
        // A lone .app is the payload itself, not a wrapper: unwrapping it
        // would stage the bundle's Contents/ as if it were the install root.
        if (top_level.size() == 1 && top_level[0].is_directory() &&
            top_level[0].path().extension() != ".app") {
            content_root = top_level[0].path();
        }
    }

    // A no-op for an .app payload, whose files live inside the bundle, and
    // deliberately so: rewriting anything under Contents/ would break the
    // signature the whole-bundle swap exists to keep. User settings are not
    // affected either way: they live in the data folder, not next to the
    // executable.
    MergeShippedToml(install_root, content_root);

    std::filesystem::remove_all(staging, ec);
    std::string stage_error;
    if (!archive::MoveOrCopyDirectory(content_root, staging, stage_error)) {
        std::filesystem::remove_all(temp_root, ec);
        fail("failed to stage extracted update: " + stage_error);
        return;
    }
    std::filesystem::remove_all(temp_root, ec);

    std::fprintf(stderr, "[update] v%s staged in %s\n", info.version.c_str(),
                 staging.string().c_str());
    std::lock_guard<std::mutex> lock(install_mutex_);
    install_result_.in_progress = false;
    install_result_.done = true;
    install_result_.ok = true;
    install_result_.message = "Downloaded v" + info.version + "; restart to apply";
}

InstallResult SelfUpdater::InstallSnapshot() const {
    std::lock_guard<std::mutex> lock(install_mutex_);
    return install_result_;
}

}  // namespace nds::update
