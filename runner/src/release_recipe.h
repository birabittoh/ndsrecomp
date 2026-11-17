#pragma once

// Rebuilding one release's code from another release's ROM (a recipe from
// tools/release_recipe.py, which documents the format). The build's banks
// carry most of the code they were made from in their validation copies; a
// recipe fills in the rest from the other release's files, so a player who
// owns only that release gets a folder that runs as the build's release.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct NdsReleaseRecipeInfo {
    std::string runs_as;     // SHA-1 of the image the result runs as
    std::string applies_to;  // SHA-1 of the image it rebuilds from
};

bool nds_release_recipe_info(const uint8_t* recipe, size_t size,
                             NdsReleaseRecipeInfo* info, std::string* error);

// Decompresses DS "code compression" (backward LZ). `data` must end with the
// compression footer. ndspy.codeCompression.decompress's exact output.
bool nds_code_decompress(const std::vector<uint8_t>& data,
                         std::vector<uint8_t>* out, std::string* error);

// Writes to `dest` (which must not exist) the unpacked ROM folder `source`
// with its ARM9 code rebuilt by `recipe`: the rebuilt files uncompressed,
// the ARM9 moved past the end of the image (so it no longer has a secure
// area), the overlay table marking nothing compressed, and rom.toml naming
// the recipe's runs_as as its identity. Data files are hard links where the
// file system allows, else copies. The validation copies come from the
// title banks configured for runs_as (title_banks.h). Works in
// `<dest>.partial` and renames it at the end.
bool nds_release_convert_folder(const std::filesystem::path& source,
                                const std::filesystem::path& dest,
                                const uint8_t* recipe, size_t recipe_size,
                                std::string* error);
