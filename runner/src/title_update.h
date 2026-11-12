// title_update.h -- the self-updater (update/self_update.h), for a title
// that shows it in its own menus.
//
// The updater runs on the frontend thread: it checks for a newer release
// quietly at startup, and downloads, stages and applies one when asked. A
// title hook runs on the title thread, so the two meet here: the frontend
// publishes the updater's state once per host frame, and a hook asks for
// the install, which the frontend starts on its next frame. Once the update
// is staged the frontend applies it at once: on desktop it starts the
// helper that swaps the files and quits, and the helper starts the new
// version; on Android it opens the system package installer and keeps
// running.
//
// Like a title menu row (title_menu.h), this is host state a hook can read:
// a hook that only shows it and asks for the install leaves the guest
// deterministic.
#pragma once

#include <cstdint>
#include <string>

enum class NdsTitleUpdateState {
    Off,          // no release configured, no version stamped, or unsupported
    Checking,
    UpToDate,
    Available,    // `available_version` is newer than `current_version`
    Downloading,  // download, verification and extraction
    Applying,     // staged; restarting, or handed to the package installer
    Failed,       // the check or the install failed; `message` says why
};

struct NdsTitleUpdateStatus {
    std::string current_version;    // this build's X.Y.Z; "" when not stamped
    NdsTitleUpdateState state = NdsTitleUpdateState::Off;
    std::string available_version;  // X.Y.Z of the newer release, if any
    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes = 0;       // 0 while unknown
    std::string message;            // English, for Failed
    // True where applying restarts into the new version; false on Android,
    // where the package installer takes over and the game keeps running.
    bool restarts = true;
};

// For the title thread. The state as of the frontend's last frame; Off with
// only `current_version` set before the frontend first published it (and
// in a headless run).
NdsTitleUpdateStatus nds_title_update_status();
// Ask for the available update to be downloaded, staged and applied.
// Ignored unless the state is Available (or Failed after an install).
void nds_title_update_install();

// For the frontend.
void nds_title_update_publish(NdsTitleUpdateStatus status);
// True once per install request, oldest first.
bool nds_title_update_take_install_request();
