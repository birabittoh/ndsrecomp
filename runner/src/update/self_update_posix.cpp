// update/self_update_posix.cpp -- SelfUpdater::ApplyAndRestart on Linux: a
// detached sh helper. See update/self_update.h.
#include "update/self_update.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace nds::update {

bool SelfUpdater::ApplyAndRestart(const std::filesystem::path& install_root,
                                  const std::filesystem::path& executable_path) {
    if (!HasPendingSelfUpdate(install_root, {})) return false;
    const auto staging = StagingRoot(install_root);

    std::error_code ec;
    std::vector<std::filesystem::path> staged_entries;
    for (const auto& entry : std::filesystem::directory_iterator(staging, ec))
        staged_entries.push_back(entry.path());
    if (staged_entries.empty()) return false;

    // As on Windows: this process's own executable lives under
    // `install_root` and stays mapped for as long as it runs, including a
    // relaunched instance; a detached shell script outside `install_root`
    // waits for this pid to exit before touching anything.
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
    // Wait for the old process to go away. `kill -0` alone is not enough:
    // it also succeeds on a zombie, and this helper is not the game's
    // parent, so it cannot reap it; if whatever started the game is slow to
    // wait() on it, a plain `kill -0` loop would spin forever and the update
    // would never apply. A zombie counts as gone, and the wait is capped so
    // the helper always moves on.
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
    for (const auto& staged_entry : staged_entries) {
        const auto dest = install_root / staged_entry.filename();
        script << "rm -rf \"" << dest.string() << "\" >>\"$NDSLOG\" 2>&1\n";
        script << "if mv \"" << staged_entry.string() << "\" \"" << dest.string()
               << "\" >>\"$NDSLOG\" 2>&1; then\n";
        script << "  echo \"[ok] " << dest.string() << "\" >>\"$NDSLOG\"\n";
        script << "else\n";
        script << "  echo \"[FAILED] " << dest.string() << "\" >>\"$NDSLOG\"\n";
        script << "fi\n";
    }
    script << "rmdir \"" << staging.string() << "\" 2>/dev/null\n";
    // The tar reader restores no permission bits.
    script << "chmod +x \"" << executable_path.string() << "\" 2>/dev/null\n";
    script << "echo relaunching >>\"$NDSLOG\"\n";
    // Relaunch from the install root, as the Windows helper's `start /D`
    // does; otherwise the new process would inherit whatever working folder
    // the old one had.
    script << "cd \"" << install_root.string() << "\" || exit 1\n";
    script << "\"" << executable_path.string() << "\" &\n";
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
