#include "rom_icon.h"

bool nds_rom_banner_icon_rgba(const uint8_t* rom, size_t size, int scale,
                              std::vector<uint8_t>* rgba) {
    if (!rom || !rgba || scale < 1 || size < 0x70)
        return false;
    const uint32_t banner = rom[0x68] | rom[0x69] << 8 | rom[0x6A] << 16 |
                            static_cast<uint32_t>(rom[0x6B]) << 24;
    if (banner < 0x200 || banner > size || size - banner < 0x240)
        return false;
    const uint8_t* bitmap = rom + banner + 0x20;
    const uint8_t* palette = rom + banner + 0x220;

    uint8_t colour[16][4];
    for (int i = 0; i < 16; ++i) {
        const unsigned c = palette[i * 2] | palette[i * 2 + 1] << 8;
        const unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        colour[i][0] = static_cast<uint8_t>(r << 3 | r >> 2);
        colour[i][1] = static_cast<uint8_t>(g << 3 | g >> 2);
        colour[i][2] = static_cast<uint8_t>(b << 3 | b >> 2);
        colour[i][3] = i == 0 ? 0 : 255;
    }

    const int side = 32 * scale;
    rgba->assign(static_cast<size_t>(side) * side * 4, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            // 4x4 tiles of 8x8 pixels, 32 bytes each, low nibble first.
            const int tile = (y / 8) * 4 + (x / 8);
            const uint8_t byte =
                bitmap[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
            const int index = (x & 1) ? byte >> 4 : byte & 15;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    uint8_t* out = rgba->data() +
                        ((static_cast<size_t>(y) * scale + dy) * side +
                         static_cast<size_t>(x) * scale + dx) * 4;
                    for (int k = 0; k < 4; ++k)
                        out[k] = colour[index][k];
                }
            }
        }
    }
    return true;
}
