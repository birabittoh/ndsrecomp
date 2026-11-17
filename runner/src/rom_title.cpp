#include "rom_title.h"

#include "frontend.h"
#include "title_settings.h"

#if defined(NDS_ROM_TITLES_HEADER)
#include NDS_ROM_TITLES_HEADER
#endif

const char* nds_rom_title(int language) {
#if defined(NDS_ROM_TITLES_HEADER)
    if (language >= 0 && language < 8 && *kNdsRomTitles[language])
        return kNdsRomTitles[language];
#else
    (void)language;
#endif
    return nullptr;
}

std::string nds_game_display_name(const NdsFrontendOptions& options) {
    int language = nds_title_language();
    if (language < 0 && options.firmware_language != NdsFirmwareLanguage::Preserve)
        language = static_cast<int>(options.firmware_language);
    if (const char* title = nds_rom_title(language)) return title;
    if (const char* title = nds_rom_title(1)) return title;
    return options.game_name.empty() ? "ndsrecomp" : options.game_name;
}
