// update/self_update.h -- the self-updater: checks a GitHub Releases repo
// for a newer version of this build, downloads, sha256-verifies and extracts
// it, and stages it to replace the install on a restart.
//
// The release this build compares itself against is configured at build
// time (runner/CMakeLists.txt): NDS_UPDATE_REPO ("owner/name"),
// NDS_UPDATE_ASSET_FORMAT (the release asset's name without its extension,
// with "{tag}" and "{platform}" placeholders) and NDS_RELEASE_VERSION (this
// build's own X.Y.Z). Any of them empty turns the updater off.
//
// Applying the staged content is not something this process can do itself:
// the install root holds its own running executable, which the OS keeps
// locked (Windows) or mapped for the whole life of the process, including a
// relaunched instance, which would be executing from that same file the
// moment it tried to replace it. ApplyAndRestart() instead hands the swap
// to a small detached helper script that outlives this process, waits for
// it to exit, does the swap and relaunches the game; see
// self_update_win.cpp, self_update_posix.cpp and self_update_mac.cpp.
// Android hands the staged APK to the system package installer instead
// (self_update_android.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace nds::update {

enum class CheckState { kIdle, kChecking, kUpToDate, kUpdateAvailable, kFailed };

struct UpdateInfo {
    std::string tag;         // the release's tag_name, e.g. "v1.4.0"
    std::string version;     // `tag` without a leading 'v' / 'V', e.g. "1.4.0"
    std::string asset_name;  // the matched release asset's file name
    std::string asset_url;   // its browser_download_url
    std::string sha256;      // lowercase hex, from GitHub's asset digest
    std::string notes;       // the release body (changelog)
};

struct InstallResult {
    bool in_progress = false;
    bool done = false;
    bool ok = false;
    std::string message;
    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes = 0;
};

struct Config {
    std::string repo;             // "owner/name"
    std::string asset_format;     // "Name-{tag}-{platform}"
    std::string current_version;  // X.Y.Z, without a leading 'v'
};

// The configuration this build was stamped with.
Config BuildConfig();

// "win-amd64", "linux-amd64", "linux-arm64", "mac-amd64", "mac-arm64",
// "android-arm64": the {platform} of a release asset, so it has to match the
// release workflow's matrix.
std::string HostPlatformId();

// Compares dotted version strings numerically: < 0, 0 or > 0 as `have` is
// older than, the same as or newer than `want`. A version that does not
// parse counts as all zeros.
int CompareVersionStrings(std::string_view have, std::string_view want);

class SelfUpdater {
public:
    explicit SelfUpdater(Config config) : config_(std::move(config)) {}
    ~SelfUpdater();
    SelfUpdater(const SelfUpdater&) = delete;
    SelfUpdater& operator=(const SelfUpdater&) = delete;

    // False on platforms where no ApplyAndRestart is built, so a staged
    // update could never be applied. Callers skip the whole update UI then;
    // CheckAsync and InstallAsync refuse too.
    static bool SupportsSelfUpdate();

    // True when the build is configured for updates (repo, asset format and
    // its own version are all set) and the platform supports them.
    bool Enabled() const;

    // Starts a background check of the repo's latest release; a no-op if one
    // is in flight. Settles straight to kFailed, with no request made, when
    // !Enabled().
    void CheckAsync();

    CheckState state() const { return state_.load(std::memory_order_acquire); }

    // The matched, newer release once state() is kUpdateAvailable.
    std::optional<UpdateInfo> Available() const;

    // Downloads `info`'s asset, verifies its sha256, extracts it and stages
    // it under StagingRoot(install_root) for ApplyAndRestart(). A no-op if an
    // install is in flight or the platform has no ApplyAndRestart.
    void InstallAsync(const UpdateInfo& info,
                      const std::filesystem::path& install_root);

    InstallResult InstallSnapshot() const;

    // The running executable, absolute; empty when it cannot be found (and
    // on Android, which has none of its own).
    static std::filesystem::path ExecutablePath();

    // The directory an update replaces entries in, which every other path
    // here takes as `install_root`: the executable's folder everywhere but
    // in a macOS .app, where the executable sits in Contents/MacOS and the
    // bundle's parent owns the install (the bundle is swapped whole; see
    // self_update_mac.cpp), and on Android, the app's internal files
    // directory (the FileProvider serves the staged APK from there).
    static std::filesystem::path InstallRoot();

    // The .app holding `executable_path` (.../Foo.app/Contents/MacOS/foo),
    // or an empty path when it is not laid out that way, which is what
    // every non-macOS build gets. Pure path inspection.
    static std::filesystem::path EnclosingAppBundle(
        const std::filesystem::path& executable_path);

#if defined(__APPLE__)
    // Mounts `dmg` read-only, copies its real top-level entries into
    // `dest_dir` with ditto (which keeps the code signature intact, unlike a
    // plain recursive copy) and detaches it. The drag-to-install
    // "Applications" link and dot-files are skipped.
    static bool ExtractDmg(const std::filesystem::path& dmg,
                           const std::filesystem::path& dest_dir,
                           std::string& error);
#endif

    // Where a staged update's extracted content lives, as children that
    // mirror the install root's own top-level layout.
    static std::filesystem::path StagingRoot(const std::filesystem::path& install_root);

    // True if an update is staged and waiting for ApplyAndRestart(). On
    // Android a staged APK no newer than `current_version` is discarded
    // here, since the system installer never clears the staging folder.
    static bool HasPendingSelfUpdate(const std::filesystem::path& install_root,
                                     const std::string& current_version);

    // The staged "update-<version>.apk" (empty if none) and the version its
    // name carries. Android only in practice; pure path inspection.
    static std::filesystem::path StagedApkPath(const std::filesystem::path& install_root);
    static std::string StagedApkVersion(const std::filesystem::path& apk);

    // Spawns a detached helper (a temp script) that waits for this process
    // to exit, moves every top-level entry of StagingRoot(install_root) onto
    // its namesake directly under `install_root` (overwriting it; save data,
    // settings and anything else the release does not ship are never
    // touched), relaunches `executable_path` and deletes itself. False, with
    // nothing spawned, if no update is staged. True when the helper was
    // spawned, in which case the caller must quit right after; the helper's
    // wait loop expects this process to be gone soon.
    //
    // Android instead hands the staged APK to the system package installer
    // and returns once its activity is up; the caller must NOT quit, the
    // installer replaces the app by itself. The app has to declare a
    // FileProvider with authority "<applicationId>.fileprovider" whose paths
    // cover StagingRoot(), request REQUEST_INSTALL_PACKAGES, and depend on
    // androidx.core; `executable_path` is ignored.
    static bool ApplyAndRestart(const std::filesystem::path& install_root,
                                const std::filesystem::path& executable_path);

    // Whether ApplyAndRestart() relaunches the game itself (false on
    // Android, where the package installer takes over).
    static bool RestartsItself();

    // The release asset's base name (no extension) for `tag`, from `format`
    // with its "{tag}" and "{platform}" placeholders filled in.
    static std::string ExpandAssetFormat(const std::string& format,
                                         const std::string& tag);

private:
    void CheckWorker(std::string repo, std::string format,
                     std::string current_version);
    void InstallWorker(UpdateInfo info, std::filesystem::path install_root);

    Config config_;

    std::atomic<bool> check_in_flight_{false};
    std::thread check_thread_;
    std::atomic<CheckState> state_{CheckState::kIdle};
    mutable std::mutex info_mutex_;
    std::optional<UpdateInfo> available_;

    std::atomic<bool> install_in_flight_{false};
    std::thread install_thread_;
    mutable std::mutex install_mutex_;
    InstallResult install_result_;
};

}  // namespace nds::update
