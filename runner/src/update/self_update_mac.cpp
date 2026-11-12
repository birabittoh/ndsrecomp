// update/self_update_mac.cpp -- SelfUpdater::ApplyAndRestart on macOS. See
// update/self_update.h.
//
// The same detached helper script as on Linux (self_update_posix.cpp),
// since a running process cannot swap its own executable and dylibs out
// from under itself. macOS adds one constraint: an .app bundle carries a
// code signature over its whole contents. Replacing files inside the
// running bundle breaks that signature, and Gatekeeper then kills the
// relaunched app. So when the executable lives in a bundle, the whole .app
// is swapped as a unit, which keeps the incoming bundle's own signature.
#include "update/self_update.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace nds::update {

namespace {

// Wraps `text` for /bin/sh: single quotes, so nothing inside is expanded;
// an embedded quote closes, escapes and reopens them.
std::string ShellQuote(const std::string& text) {
    std::string quoted = "'";
    for (const char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

int RunShell(const std::string& command) {
    const int status = std::system(command.c_str());
    return status == -1 ? -1 : WEXITSTATUS(status);
}

// The staged entry that replaces `bundle`: one with the same name first,
// then the only .app, so a release that renames the bundle still applies.
std::filesystem::path FindStagedBundle(
    const std::vector<std::filesystem::path>& staged_entries,
    const std::filesystem::path& bundle) {
    std::filesystem::path only_app;
    size_t app_count = 0;
    for (const auto& entry : staged_entries) {
        if (entry.extension() != ".app") continue;
        if (entry.filename() == bundle.filename()) return entry;
        only_app = entry;
        ++app_count;
    }
    return app_count == 1 ? only_app : std::filesystem::path{};
}

}  // namespace

bool SelfUpdater::ExtractDmg(const std::filesystem::path& dmg,
                             const std::filesystem::path& dest_dir,
                             std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        error = "failed to create destination directory: " + ec.message();
        return false;
    }
    // An explicit mount point, so the image's volume name never has to be
    // read back out of hdiutil's output.
    const auto mount_point = dest_dir.parent_path() / "dmg-mount";
    std::filesystem::remove_all(mount_point, ec);

    const std::string attach =
        "hdiutil attach -nobrowse -readonly -noverify -quiet -mountpoint " +
        ShellQuote(mount_point.string()) + " " + ShellQuote(dmg.string());
    if (RunShell(attach) != 0) {
        error = "failed to mount " + dmg.filename().string();
        return false;
    }

    bool ok = true;
    size_t copied = 0;
    for (const auto& entry : std::filesystem::directory_iterator(mount_point, ec)) {
        const auto name = entry.path().filename().string();
        // The drag-to-install "Applications" link, .background and
        // .DS_Store belong to the disk image's presentation, not the install.
        if (entry.is_symlink() || name.empty() || name[0] == '.') continue;
        // ditto rather than a file system copy: it keeps the extended
        // attributes and permissions an .app's code signature covers.
        const std::string copy = "ditto " + ShellQuote(entry.path().string()) + " " +
                                 ShellQuote((dest_dir / name).string());
        if (RunShell(copy) != 0) {
            error = "failed to copy " + name + " out of the disk image";
            ok = false;
            break;
        }
        ++copied;
    }
    if (ok && copied == 0) {
        error = "disk image contains nothing to install";
        ok = false;
    }

    RunShell("hdiutil detach -quiet " + ShellQuote(mount_point.string()) +
             " || hdiutil detach -force -quiet " + ShellQuote(mount_point.string()));
    std::filesystem::remove_all(mount_point, ec);
    return ok;
}

bool SelfUpdater::ApplyAndRestart(const std::filesystem::path& install_root,
                                  const std::filesystem::path& executable_path) {
    if (!HasPendingSelfUpdate(install_root, {})) return false;
    const auto staging = StagingRoot(install_root);

    std::error_code ec;
    std::vector<std::filesystem::path> staged_entries;
    for (const auto& entry : std::filesystem::directory_iterator(staging, ec))
        staged_entries.push_back(entry.path());
    if (staged_entries.empty()) return false;

    const auto bundle = EnclosingAppBundle(executable_path);
    std::filesystem::path staged_bundle;
    if (!bundle.empty()) {
        staged_bundle = FindStagedBundle(staged_entries, bundle);
        if (staged_bundle.empty()) {
            // Running from a bundle, but the release ships none. Merging loose
            // files into the bundle would break its signature and get the
            // relaunch killed, so refuse rather than brick the install.
            std::fprintf(stderr,
                         "[update] running from %s but the staged update has no "
                         ".app; not applying it (it would break the bundle's "
                         "signature)\n",
                         bundle.string().c_str());
            return false;
        }
    }

    const auto temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        std::fprintf(stderr, "[update] no usable temp directory (%s)\n",
                     ec.message().c_str());
        return false;
    }
    const pid_t pid = getpid();
    const auto script_path =
        temp_dir / ("nds_selfupdate_" + std::to_string(pid) + ".sh");
    const auto log_path = temp_dir / "nds_selfupdate.log";
    std::ofstream script(script_path, std::ios::binary | std::ios::trunc);
    if (!script) {
        std::fprintf(stderr, "[update] could not create the update helper script\n");
        return false;
    }
    script << "#!/bin/sh\n";
    script << "NDSLOG=\"" << log_path.string() << "\"\n";
    script << "echo \"==== self-update helper, pid " << pid << " ====\" >>\"$NDSLOG\"\n";
    script << "date >>\"$NDSLOG\"\n";
    // The same zombie-aware, capped wait as the Linux helper (see
    // self_update_posix.cpp for why `kill -0` alone is not enough). BSD
    // `ps -o stat= -p` behaves the same.
    script << "ndswait=0\n";
    script << "while kill -0 " << pid << " 2>/dev/null; do\n";
    script << "  ndsstate=$(ps -o stat= -p " << pid << " 2>/dev/null | head -n1)\n";
    script << "  case \"$ndsstate\" in *Z*) break;; esac\n";
    script << "  ndswait=$((ndswait+1))\n";
    script << "  if [ \"$ndswait\" -ge 60 ]; then\n";
    script << "    echo \"[FAILED] pid " << pid
           << " still alive after ${ndswait}s\" >>\"$NDSLOG\"\n";
    script << "    exit 1\n";
    script << "  fi\n";
    script << "  sleep 1\n";
    script << "done\n";
    script << "echo \"pid gone after ${ndswait}s, applying\" >>\"$NDSLOG\"\n";

    auto emit_swap = [&](const std::filesystem::path& src,
                         const std::filesystem::path& dest) {
        script << "rm -rf \"" << dest.string() << "\" >>\"$NDSLOG\" 2>&1\n";
        script << "if mv \"" << src.string() << "\" \"" << dest.string()
               << "\" >>\"$NDSLOG\" 2>&1; then\n";
        script << "  echo \"[ok] " << dest.string() << "\" >>\"$NDSLOG\"\n";
        script << "else\n";
        script << "  echo \"[FAILED] " << dest.string() << "\" >>\"$NDSLOG\"\n";
        script << "fi\n";
        // Clear the quarantine flag if the payload picked one up. The
        // downloader here does not set it (only LaunchServices does), so
        // this is normally a no-op; it matters when the archive arrived
        // some other way.
        script << "xattr -d -r com.apple.quarantine \"" << dest.string()
               << "\" >>\"$NDSLOG\" 2>&1\n";
    };

    std::filesystem::path relaunch_target;
    if (!bundle.empty()) {
        // Whole-bundle swap: `bundle` is the destination even when the staged
        // .app is named differently, so the relaunch path stays valid.
        emit_swap(staged_bundle, bundle);
        relaunch_target = bundle;
        // Any other staged entries are siblings of the bundle.
        for (const auto& staged_entry : staged_entries) {
            if (staged_entry == staged_bundle) continue;
            emit_swap(staged_entry, install_root / staged_entry.filename());
        }
    } else {
        for (const auto& staged_entry : staged_entries)
            emit_swap(staged_entry, install_root / staged_entry.filename());
        script << "chmod +x \"" << executable_path.string() << "\" 2>/dev/null\n";
    }
    script << "rmdir \"" << staging.string() << "\" 2>/dev/null\n";
    script << "echo relaunching >>\"$NDSLOG\"\n";
    script << "cd \"" << install_root.string() << "\" || exit 1\n";
    if (!relaunch_target.empty()) {
        // `open -n` hands the bundle to LaunchServices, which gives the new
        // instance a normal app session (Dock tile, activation, TCC
        // identity); exec'ing Contents/MacOS directly does not.
        script << "open -n \"" << relaunch_target.string() << "\" >>\"$NDSLOG\" 2>&1 || \\\n";
        script << "  \"" << executable_path.string() << "\" &\n";
    } else {
        script << "\"" << executable_path.string() << "\" &\n";
    }
    // Safe to unlink while /bin/sh is still reading this file: sh holds it
    // open, and the inode outlives its directory entry.
    script << "rm -- \"$0\"\n";
    script.close();
    std::filesystem::permissions(
        script_path,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
        ec);

    const pid_t child = fork();
    if (child < 0) {
        std::fprintf(stderr, "[update] fork failed for the update helper\n");
        return false;
    }
    if (child == 0) {
        setsid();  // its own session, so it outlives this process
        execl("/bin/sh", "sh", script_path.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // execl only returns on failure
    }
    return true;
}

}  // namespace nds::update
