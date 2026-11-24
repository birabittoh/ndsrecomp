#include "rom_folder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#include "sha1.h"
#include "toml.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kManifest = "rom.toml";
constexpr int64_t kFormat = 1;

uint16_t u16(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}

uint32_t u32(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t{d[at]} | (uint32_t{d[at + 1]} << 8) |
           (uint32_t{d[at + 2]} << 16) | (uint32_t{d[at + 3]} << 24);
}

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t banner_size(const std::vector<uint8_t>& rom, uint32_t offset) {
    if (offset == 0 || size_t{offset} + 2 > rom.size()) return 0;
    switch (u16(rom, offset)) {
        case 2: return 0x940;
        case 3: return 0xA40;
        case 0x103: return 0x23C0;
        default: return 0x840;
    }
}

uint16_t crc16(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return static_cast<uint16_t>(crc);
}

bool safe_name(const std::string& name) {
    size_t start = 0;
    for (;;) {
        const size_t slash = name.find('/', start);
        const std::string part = name.substr(
            start, slash == std::string::npos ? std::string::npos
                                              : slash - start);
        if (part.empty() || part == "." || part == ".." ||
            part.find_first_of("\\:") != std::string::npos)
            return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

void fnt_walk(const std::vector<uint8_t>& rom, uint32_t fnt_off,
              uint32_t fnt_size, uint16_t dir_count, uint16_t dir_id,
              const std::string& prefix, int depth,
              std::map<uint32_t, std::string>* names) {
    if (depth > 64 || (dir_id & 0xFFFu) >= dir_count) return;
    const size_t entry = fnt_off + 8u * (dir_id & 0xFFFu);
    size_t p = fnt_off + u32(rom, entry);
    uint32_t file_id = u16(rom, entry + 4);
    const size_t end = size_t{fnt_off} + fnt_size;
    while (p < end) {
        const uint8_t kind = rom[p++];
        if (kind == 0) return;
        const size_t length = kind & 0x7Fu;
        if (p + length > end) return;
        const std::string name(reinterpret_cast<const char*>(&rom[p]),
                               length);
        p += length;
        if (kind & 0x80u) {
            if (p + 2 > end) return;
            fnt_walk(rom, fnt_off, fnt_size, dir_count, u16(rom, p),
                     prefix + name + "/", depth + 1, names);
            p += 2;
        } else {
            (*names)[file_id++] = prefix + name;
        }
    }
}

std::string hex(uint64_t value, int width = 0) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%0*llX", width,
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string toml_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

struct Part {
    std::string name;
    uint32_t offset;
    uint32_t size;
};

bool write_file(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (size) out.write(reinterpret_cast<const char*>(data),
                        static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    if (size < 0) return false;
    in.seekg(0);
    out->resize(static_cast<size_t>(size));
    return size == 0 ||
           static_cast<bool>(in.read(reinterpret_cast<char*>(out->data()),
                                     size));
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool parse_manifest(const fs::path& dir, toml::table* root,
                    std::string* error) {
    try {
        *root = toml::parse_file((dir / kManifest).string());
    } catch (const toml::parse_error& e) {
        return fail(error, (dir / kManifest).string() + ": " +
                               std::string(e.description()));
    } catch (const std::exception& e) {
        return fail(error, (dir / kManifest).string() + ": " + e.what());
    }
    if ((*root)["rom"]["format"].value<int64_t>() != kFormat)
        return fail(error, (dir / kManifest).string() +
                               ": not a format this build reads");
    return true;
}

}  // namespace

bool nds_rom_folder_is(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / kManifest, ec);
}

bool nds_rom_folder_info(const fs::path& dir, NdsRomFolderInfo* info,
                         std::string* error) {
    toml::table root;
    if (!parse_manifest(dir, &root, error)) return false;
    info->game_code = root["rom"]["game_code"].value_or(std::string());
    info->revision =
        static_cast<uint32_t>(root["rom"]["revision"].value_or(int64_t{0}));
    info->sha1 = root["rom"]["sha1"].value_or(std::string());
    info->identity = root["rom"]["identity"].value_or(std::string());
    info->prepared = root["rom"]["prepared"].value_or(std::string());
    return true;
}

bool nds_rom_folder_unpack(const std::vector<uint8_t>& rom,
                           const fs::path& dir, std::string* error) {
    if (rom.size() < 0x200u || rom.size() > 0xFFFFFFFFu)
        return fail(error, "not a DS ROM");
    const uint32_t size = static_cast<uint32_t>(rom.size());
    uint32_t header_size = u32(rom, 0x84);
    if (header_size == 0) header_size = 0x4000;
    header_size = std::max<uint32_t>(0x200, std::min(header_size, size));
    const uint32_t fnt_off = u32(rom, 0x40), fnt_size = u32(rom, 0x44);
    const uint32_t fat_off = u32(rom, 0x48), fat_size = u32(rom, 0x4C);
    const uint32_t banner = u32(rom, 0x68);

    std::vector<Part> regions;
    for (const Part& part : std::initializer_list<Part>{
             {"header.bin", 0, header_size},
             {"arm9.bin", u32(rom, 0x20), u32(rom, 0x2C)},
             {"arm7.bin", u32(rom, 0x30), u32(rom, 0x3C)},
             {"y9.bin", u32(rom, 0x50), u32(rom, 0x54)},
             {"y7.bin", u32(rom, 0x58), u32(rom, 0x5C)},
             {"fnt.bin", fnt_off, fnt_size},
             {"banner.bin", banner, banner_size(rom, banner)}}) {
        if (part.size && uint64_t{part.offset} + part.size <= size)
            regions.push_back(part);
    }
    if (uint64_t{fat_off} + fat_size > size)
        return fail(error, "the FAT lies outside the image");

    std::map<uint32_t, std::string> names;
    {
        std::map<uint32_t, std::string> fnt;
        const bool fnt_ok = std::any_of(
            regions.begin(), regions.end(),
            [](const Part& p) { return p.name == "fnt.bin"; });
        if (fnt_ok && fnt_size >= 8)
            fnt_walk(rom, fnt_off, fnt_size, u16(rom, fnt_off + 6), 0xF000,
                     "", 0, &fnt);
        for (const auto& [id, name] : fnt)
            if (safe_name(name)) names[id] = "data/" + name;
    }
    for (const Part& region : regions) {
        const char cpu = region.name == "y9.bin" ? '9'
                       : region.name == "y7.bin" ? '7' : 0;
        if (!cpu) continue;
        for (uint32_t i = 0; i + 32 <= region.size; i += 32) {
            const uint32_t overlay = u32(rom, region.offset + i);
            const uint32_t file = u32(rom, region.offset + i + 24);
            char name[64];
            std::snprintf(name, sizeof name, "overlay/overlay%c_%04u.bin",
                          cpu, overlay);
            names[file] = name;
        }
    }

    std::vector<Part> files;
    for (uint32_t i = 0; i < fat_size / 8; ++i) {
        const uint32_t start = u32(rom, fat_off + 8 * i);
        const uint32_t end = u32(rom, fat_off + 8 * i + 4);
        if (end < start || end > size)
            return fail(error, "FAT entry " + std::to_string(i) +
                                   " lies outside the image");
        const auto named = names.find(i);
        char fallback[32];
        std::snprintf(fallback, sizeof fallback, "unnamed/%04u.bin", i);
        files.push_back({named != names.end() ? named->second : fallback,
                         start, end - start});
    }

    const uint8_t fill = rom.back();
    std::vector<std::pair<uint32_t, uint32_t>> covered;
    for (const Part& p : regions) covered.emplace_back(p.offset, p.size);
    covered.emplace_back(fat_off, fat_size);
    for (const Part& p : files)
        if (p.size) covered.emplace_back(p.offset, p.size);
    std::sort(covered.begin(), covered.end());
    covered.emplace_back(size, 0);
    std::vector<Part> extra;
    uint64_t pos = 0;
    for (const auto& [offset, length] : covered) {
        // The gap's bytes between its first and last that are not padding.
        uint64_t first = pos, last = offset;
        while (first < last && rom[first] == fill) ++first;
        while (last > first && rom[last - 1] == fill) --last;
        if (first < last)
            extra.push_back({"extra/" + hex(first, 8) + ".bin",
                             static_cast<uint32_t>(first),
                             static_cast<uint32_t>(last - first)});
        pos = std::max<uint64_t>(pos, uint64_t{offset} + length);
    }

    fs::path tmp = dir;
    tmp += ".partial";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    for (const std::vector<Part>* list : {&regions, &extra, &files}) {
        for (const Part& p : *list) {
            if (!write_file(tmp / p.name, rom.data() + p.offset, p.size)) {
                fs::remove_all(tmp, ec);
                return fail(error, "cannot write " + (tmp / p.name).string());
            }
        }
    }

    std::string game_code;
    for (size_t i = 0x0C; i < 0x10; ++i)
        game_code.push_back(static_cast<char>(rom[i]));
    std::ostringstream m;
    m << "# An unpacked Nintendo DS ROM (ndsrecomp tools/nds_folder.py).\n"
      << "# The files can be edited; offsets are the original image's.\n"
      << "\n[rom]\n"
      << "format = " << kFormat << "\n"
      << "game_code = " << toml_string(game_code) << "\n"
      << "revision = " << unsigned{rom[0x1E]} << "\n"
      << "sha1 = \"" << gba::sha1(rom.data(), rom.size()).hex() << "\"\n"
      << "size = 0x" << hex(size) << "\n"
      << "fill = 0x" << hex(fill, 2) << "\n"
      << "fat = 0x" << hex(fat_off) << "\n"
      << "\n[regions]\n";
    for (const Part& p : regions)
        m << toml_string(p.name) << " = 0x" << hex(p.offset) << "\n";
    m << "\n[extra]\n";
    for (const Part& p : extra)
        m << toml_string(p.name) << " = 0x" << hex(p.offset) << "\n";
    m << "\n[files]\nfat = [\n";
    for (const Part& p : files)
        m << "  [" << toml_string(p.name) << ", 0x" << hex(p.offset)
          << ", 0x" << hex(p.size) << "],\n";
    m << "]\n";
    const std::string text = m.str();
    if (!write_file(tmp / kManifest,
                    reinterpret_cast<const uint8_t*>(text.data()),
                    text.size())) {
        fs::remove_all(tmp, ec);
        return fail(error, "cannot write " + (tmp / kManifest).string());
    }
    // A sync client (OneDrive) or a virus scanner can still hold the files
    // just written, failing the directory rename for a moment.
    for (int attempt = 0;; ++attempt) {
        fs::rename(tmp, dir, ec);
        if (!ec || attempt == 20) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (ec) {
        const std::string reason = ec.message();
        fs::remove_all(tmp, ec);
        return fail(error, "cannot create " + dir.string() + ": " + reason);
    }
    return true;
}

bool nds_rom_folder_pack(const fs::path& dir, std::vector<uint8_t>* rom,
                         std::string* error) {
    toml::table root;
    if (!parse_manifest(dir, &root, error)) return false;
    const auto size_value = root["rom"]["size"].value<int64_t>();
    const auto fill_value = root["rom"]["fill"].value<int64_t>();
    const auto fat_value = root["rom"]["fat"].value<int64_t>();
    const toml::array* fat = root["files"]["fat"].as_array();
    if (!size_value || !fill_value || !fat_value || !fat ||
        *size_value <= 0 || *size_value > 0x80000000LL)
        return fail(error, (dir / kManifest).string() + " is incomplete");

    struct Placed {
        std::string name;
        uint32_t offset;
        std::vector<uint8_t> data;
    };
    std::vector<Placed> fixed;
    for (const char* table : {"regions", "extra"}) {
        const toml::table* entries = root[table].as_table();
        if (!entries) continue;
        for (const auto& [key, value] : *entries) {
            const auto offset = value.value<int64_t>();
            if (!offset || *offset < 0)
                return fail(error, (dir / kManifest).string() + ": bad " +
                                       std::string(key.str()));
            Placed part{std::string(key.str()),
                        static_cast<uint32_t>(*offset), {}};
            if (!read_file(dir / part.name, &part.data))
                return fail(error, "cannot read " + (dir / part.name).string());
            fixed.push_back(std::move(part));
        }
    }
    const auto header = std::find_if(
        fixed.begin(), fixed.end(),
        [](const Placed& p) { return p.name == "header.bin"; });
    if (header == fixed.end() || header->data.size() < 0x200)
        return fail(error, (dir / "header.bin").string() +
                               " is missing or short");
    static const std::pair<const char*, size_t> kSized[] = {
        {"arm9.bin", 0x2C}, {"arm7.bin", 0x3C}, {"y9.bin", 0x54},
        {"y7.bin", 0x5C}, {"fnt.bin", 0x44}};
    for (const Placed& p : fixed) {
        for (const auto& [name, at] : kSized) {
            if (p.name == name && p.data.size() != u32(header->data, at))
                return fail(error, (dir / p.name).string() +
                                       " changed size; it has to keep it");
        }
    }

    uint64_t end = u32(header->data, 0x80);
    for (const Placed& p : fixed)
        end = std::max<uint64_t>(end, uint64_t{p.offset} + p.data.size());
    std::vector<Placed> files;
    std::vector<uint32_t> slots;
    files.reserve(fat->size());
    for (const toml::node& node : *fat) {
        const toml::array* entry = node.as_array();
        const bool shaped = entry && entry->size() == 3;
        const auto name =
            shaped ? (*entry)[0].value<std::string>() : std::nullopt;
        const auto offset =
            shaped ? (*entry)[1].value<int64_t>() : std::nullopt;
        const auto slot = shaped ? (*entry)[2].value<int64_t>() : std::nullopt;
        if (!name || !offset || !slot || *offset < 0 || *slot < 0)
            return fail(error, (dir / kManifest).string() +
                                   ": bad FAT entry " +
                                   std::to_string(files.size()));
        Placed file{*name, static_cast<uint32_t>(*offset), {}};
        if (!read_file(dir / file.name, &file.data))
            return fail(error, "cannot read " + (dir / file.name).string());
        end = std::max<uint64_t>(end, uint64_t{file.offset} + *slot);
        slots.push_back(static_cast<uint32_t>(*slot));
        files.push_back(std::move(file));
    }
    bool moved = false;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].data.size() <= slots[i]) continue;
        files[i].offset = static_cast<uint32_t>((end + 0x1FF) & ~uint64_t{0x1FF});
        end = uint64_t{files[i].offset} + files[i].data.size();
        moved = true;
    }
    uint64_t size = static_cast<uint64_t>(*size_value);
    while (size < end) size *= 2;
    if (size > 0x80000000ull)
        return fail(error, dir.string() + " packs into more than 2 GiB");

    const uint32_t fat_off = static_cast<uint32_t>(*fat_value);
    if (uint64_t{fat_off} + 8 * files.size() > size)
        return fail(error, (dir / kManifest).string() + ": bad FAT offset");
    rom->assign(static_cast<size_t>(size),
                static_cast<uint8_t>(*fill_value));
    for (const Placed& p : fixed) {
        if (uint64_t{p.offset} + p.data.size() > size)
            return fail(error, (dir / p.name).string() +
                                   " lies outside the image");
        std::copy(p.data.begin(), p.data.end(), rom->begin() + p.offset);
    }
    for (size_t i = 0; i < files.size(); ++i) {
        const Placed& f = files[i];
        std::copy(f.data.begin(), f.data.end(), rom->begin() + f.offset);
        put32(*rom, fat_off + 8 * i, f.offset);
        put32(*rom, fat_off + 8 * i + 4,
              f.offset + static_cast<uint32_t>(f.data.size()));
    }
    if (moved) {
        put32(*rom, 0x80, static_cast<uint32_t>(end));
        uint8_t chip = 0;
        while ((uint64_t{0x20000} << chip) < size) ++chip;
        (*rom)[0x14] = chip;
        const uint16_t crc = crc16(rom->data(), 0x15E);
        (*rom)[0x15E] = static_cast<uint8_t>(crc);
        (*rom)[0x15F] = static_cast<uint8_t>(crc >> 8);
    }
    return true;
}
