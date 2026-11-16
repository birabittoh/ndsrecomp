// update/archive.h -- unpacking a downloaded release and moving it into
// place, for the self-updater.
//
// The archives come off the network, so an entry with an absolute path or a
// ".." segment is refused rather than trusted to stay inside `dest_dir`.
// None of these throw: false with `error` set on any failure.
#pragma once

#include <filesystem>
#include <string>

namespace nds::archive {

// Extracts a .zip (miniz) into `dest_dir`, creating it if needed.
bool ExtractZip(const std::filesystem::path& archive,
                const std::filesystem::path& dest_dir, std::string& error);

// Extracts a gzip-compressed ustar/GNU .tar.gz into `dest_dir`, creating it
// if needed. Only regular files and directories are honoured: symlinks,
// devices and the like are skipped, and permission bits are not restored.
bool ExtractTarGz(const std::filesystem::path& archive,
                  const std::filesystem::path& dest_dir, std::string& error);

// Renames `from` to `to`, falling back to a recursive copy (then removing
// `from`) when the rename fails, e.g. across file systems. Both are retried
// a few times with a short backoff: on Windows a file that was just written
// (as an extraction just did) is briefly held open by real-time antivirus
// scanning, and losing that race fails a rename the caller can do nothing
// about except try again shortly after.
bool MoveOrCopyDirectory(const std::filesystem::path& from,
                         const std::filesystem::path& to, std::string& error);

}  // namespace nds::archive
