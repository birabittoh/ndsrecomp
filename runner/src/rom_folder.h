#pragma once

// A cartridge image unpacked into a folder of files (ndstool's layout plus a
// rom.toml manifest that keeps every offset), and packed back into an image
// at boot. An unchanged folder packs into the exact image it came from; a
// file that grew is moved after the last used byte and the FAT follows it.
// tools/nds_folder.py reads and writes the same format and documents it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct NdsRomFolderInfo {
    std::string game_code;
    uint32_t revision = 0;
    // The image the folder was unpacked from.
    std::string sha1;
    // Optional: the SHA-1 of the image whose code this folder carries, when
    // it is not the image it was unpacked from (a release converted to
    // another's code). The runner keys everything that names a cartridge on
    // it instead of the packed image's hash. Empty when absent.
    std::string identity;
};

// `dir` holds a rom.toml.
bool nds_rom_folder_is(const std::filesystem::path& dir);

// The [rom] table of `dir`'s manifest, without reading the files.
bool nds_rom_folder_info(const std::filesystem::path& dir,
                         NdsRomFolderInfo* info, std::string* error);

// Writes `rom` unpacked to `dir`, which must not exist. Works in
// `<dir>.partial` and renames it at the end, so `dir` is either complete or
// missing.
bool nds_rom_folder_unpack(const std::vector<uint8_t>& rom,
                           const std::filesystem::path& dir,
                           std::string* error);

bool nds_rom_folder_pack(const std::filesystem::path& dir,
                         std::vector<uint8_t>* rom, std::string* error);
