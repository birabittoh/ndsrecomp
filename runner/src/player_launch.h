#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "frontend.h"

// The zero-configuration way to run a title build: start the executable with
// no arguments (or just a .nds path). The title's game.toml is embedded at
// build time (-DNDS_TITLE_CONFIG), so nothing depends on the working
// directory, and everything the player owns lives in one data directory:
//
//   settings.toml   editable; the runtime menu writes changes back to it
//   saves/          cartridge battery save
//   savestates/     savestate slots
//   bios/           optional home for the BIOS and firmware dumps
//   logs/           <app>_NNN.log (stderr, log_file.h), performance logs
//                   and coverage manifests
//
// The directory is named after [game].app_name:
//   Linux    $XDG_DATA_HOME/<app> (~/.local/share/<app>)
//   Windows  Documents\<app>
//   macOS    ~/Library/Application Support/<app>
// A file called portable.txt next to the executable selects the executable's
// own directory instead.
//
// `--instance N` (0..15) also counts as player mode and turns on local
// wireless, so several copies of the game on one machine can play together:
// start one with `--instance 0` and another with `--instance 1`. Instance N
// gets its own guest MAC, and from 1 on its own save (a copy of the main
// one, made on first launch: saves/<app>.instance-N.sav), savestates and
// logs.
//
// Any other command line keeps its explicit behaviour: scripted and headless
// runs never read settings.toml.

struct NdsPlayerLaunch {
    std::string rom;
    std::string bios_dir;
    std::string firmware;
    bool embedded_dumps = false;  // dumps come from the executable
    std::string save;
    std::string savestate_dir;
    std::string log_dir;
    // Exit status when nds_player_prepare returns false.
    int exit_code = 0;
};

bool nds_player_requested(int argc, char** argv);

// The dumps baked in with -DNDS_EMBED_DUMPS_DIR. False when this build has
// none, in which case the player supplies a folder instead.
bool nds_player_embedded_dumps(std::vector<uint8_t>* arm9_bios,
                               std::vector<uint8_t>* arm7_bios,
                               std::vector<uint8_t>* firmware);

// Load the embedded config and settings.toml into `options`, then find the
// ROM and BIOS, asking the player for whichever is unknown and remembering
// the answer. False means do not boot: the player cancelled, or a dialog
// already reported the error.
bool nds_player_prepare(int argc, char** argv, NdsFrontendOptions* options,
                        NdsPlayerLaunch* out);

// Show a fatal startup error: a player has no terminal to read stderr in.
void nds_player_error(const std::string& message);
