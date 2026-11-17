// rom_folder_test                       synthetic round trip and relocation
// rom_folder_test unpack game.nds DIR   (for cross-checking tools/nds_folder.py)
// rom_folder_test pack DIR out.nds

#include "rom_folder.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t get32(const std::vector<uint8_t>& d, size_t at) {
    return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) |
           (uint32_t{d[at + 3]} << 24);
}

std::vector<uint8_t> read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

// A 128 KiB image: header, a 0x100-byte ARM9 and ARM7, an FNT with
// "a.bin" and "sub/b.bin", a FAT of two files and one stray byte.
std::vector<uint8_t> synthetic() {
    std::vector<uint8_t> rom(0x20000, 0xFF);
    std::fill(rom.begin(), rom.begin() + 0x4000, 0);
    std::memcpy(&rom[0], "SYNTHETIC", 9);
    std::memcpy(&rom[0x0C], "TEST", 4);
    put32(rom, 0x20, 0x4000); put32(rom, 0x2C, 0x100);
    for (int i = 0; i < 0x100; ++i) rom[0x4000 + i] = static_cast<uint8_t>(i);
    put32(rom, 0x30, 0x4200); put32(rom, 0x3C, 0x100);
    for (int i = 0; i < 0x100; ++i) rom[0x4200 + i] = static_cast<uint8_t>(~i);
    // FNT: root (dir 0xF000) and sub (0xF001).
    const uint32_t fnt = 0x4400;
    std::vector<uint8_t> t(16, 0);
    put32(t, 0, 16); t[4] = 0; t[5] = 0; t[6] = 2; t[7] = 0;
    t[12] = 0; t[13] = 0; t[14] = 0; t[15] = 0xF0;
    const uint8_t root[] = {5, 'a', '.', 'b', 'i', 'n',
                            0x83, 's', 'u', 'b', 0x01, 0xF0, 0};
    t.insert(t.end(), root, root + sizeof root);
    put32(t, 8, static_cast<uint32_t>(t.size()));
    t[12] = 1;  // sub's first file id
    const uint8_t sub[] = {5, 'b', '.', 'b', 'i', 'n', 0};
    t.insert(t.end(), sub, sub + sizeof sub);
    std::copy(t.begin(), t.end(), rom.begin() + fnt);
    put32(rom, 0x40, fnt); put32(rom, 0x44, static_cast<uint32_t>(t.size()));
    const uint32_t fat = 0x4600;
    put32(rom, 0x48, fat); put32(rom, 0x4C, 16);
    put32(rom, fat, 0x5000); put32(rom, fat + 4, 0x5010);
    put32(rom, fat + 8, 0x5200); put32(rom, fat + 12, 0x5280);
    for (int i = 0; i < 0x10; ++i) rom[0x5000 + i] = 0xA0;
    for (int i = 0; i < 0x80; ++i) rom[0x5200 + i] = 0xB0;
    rom[0x6000] = 0x42;
    put32(rom, 0x80, 0x5280);
    put32(rom, 0x84, 0x4000);
    return rom;
}

int self_test(const fs::path& scratch) {
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    const std::vector<uint8_t> rom = synthetic();
    const fs::path dir = scratch / "rom";
    std::string error;
    check(nds_rom_folder_unpack(rom, dir, &error), "unpack");
    check(nds_rom_folder_is(dir), "is a folder");
    check(fs::is_regular_file(dir / "data/a.bin") &&
          fs::is_regular_file(dir / "data/sub/b.bin"), "named files");
    check(fs::is_regular_file(dir / "extra/00006000.bin"), "stray byte kept");
    check(!fs::exists(scratch / "rom.partial"), "no partial left");
    check(!nds_rom_folder_unpack(rom, dir, &error), "refuses to overwrite");
    NdsRomFolderInfo info;
    check(nds_rom_folder_info(dir, &info, &error) &&
          info.game_code == "TEST", "info");
    std::vector<uint8_t> packed;
    check(nds_rom_folder_pack(dir, &packed, &error) && packed == rom,
          "unchanged folder packs back exactly");

    write(dir / "data/a.bin", std::vector<uint8_t>(0x40, 0xC0));
    write(dir / "data/sub/b.bin", std::vector<uint8_t>(4, 0xD0));
    check(nds_rom_folder_pack(dir, &packed, &error), "pack edited");
    const uint32_t a = get32(packed, 0x4600), a_end = get32(packed, 0x4604);
    check(a == 0x6200 && a_end == 0x6240, "grown file moved past the end");
    check(packed[a] == 0xC0 && packed[0x5000] == 0xFF, "moved data, old slot padded");
    check(get32(packed, 0x4608) == 0x5200 && get32(packed, 0x460C) == 0x5204,
          "shrunk file stays");
    check(get32(packed, 0x80) == 0x6240, "used size follows");
    check(packed[0x6000] == 0x42, "extra still there");

    write(dir / "arm9.bin", std::vector<uint8_t>(0x10, 0));
    check(!nds_rom_folder_pack(dir, &packed, &error), "arm9 must keep size");
    fs::remove_all(scratch, ec);
    if (failures == 0) std::puts("rom_folder_test: ok");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string error;
    if (argc == 4 && std::strcmp(argv[1], "unpack") == 0) {
        if (!nds_rom_folder_unpack(read(argv[2]), argv[3], &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    if (argc == 4 && std::strcmp(argv[1], "pack") == 0) {
        std::vector<uint8_t> rom;
        if (!nds_rom_folder_pack(argv[2], &rom, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        write(argv[3], rom);
        return 0;
    }
    return self_test(fs::temp_directory_path() / "ndsrecomp_rom_folder_test");
}
