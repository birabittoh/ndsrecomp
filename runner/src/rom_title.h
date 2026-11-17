#pragma once

#include <string>

struct NdsFrontendOptions;

// The game's name from the cartridge banner of the ROM the runner was built
// with (NDS_EXE_ICON_ROM, tools/gen_nds_title.py), for a firmware language
// code (0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish,
// 6 Chinese, 7 Korean). nullptr when the build has no ROM, the banner lacks
// the language or its title is empty.
const char* nds_rom_title(int language);

// The name to show for the game: the banner title in the language the game is
// shown in (a title's choice, nds_title_set_language, else the firmware
// language option), else its English one, else [game].name, else "ndsrecomp".
std::string nds_game_display_name(const NdsFrontendOptions& options);
