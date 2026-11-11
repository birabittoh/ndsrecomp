#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decodes the cartridge banner's 32x32 icon (header 0x68 -> banner+0x20 4bpp
// tiles, banner+0x220 BGR555 palette, colour 0 transparent) and scales it up
// by `scale` with nearest-neighbour, so a compositor or taskbar that resizes
// it afterwards keeps the pixel edges. Output is RGBA8, (32*scale)^2 pixels.
// Returns false when the ROM has no banner.
bool nds_rom_banner_icon_rgba(const uint8_t* rom, size_t size, int scale,
                              std::vector<uint8_t>* rgba);
