// title_save.h -- importing and exporting the cartridge save from a title's
// own menus.
//
// A title hook asks for an import or an export; the frontend shows the
// file dialog on its next frame and does the work:
//   - export writes the cartridge save image to the picked file;
//   - import reads the picked file, turns it into a save image with the
//     title's converter (a raw image of the right size is taken as is
//     without one), installs it as the cartridge save, writes it to the
//     save file and resets the console, so the game boots with it.
// The result is shown as a notice on the screen.
//
// Like a title menu request (title_menu.h), this is host state a hook only
// sends: the guest sees the new save after the reset, as after a launch.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Turns a picked file's bytes into a cartridge save image of exactly the
// save's size. False and *error (shown to the player) refuse the file.
using NdsTitleSaveConvert = bool (*)(const std::vector<uint8_t>& file,
                                     std::vector<uint8_t>* image,
                                     std::string* error);

// For the title thread. A request made while one is pending is dropped.
void nds_title_save_import(NdsTitleSaveConvert convert);
void nds_title_save_export();

// For the frontend.
enum class NdsTitleSaveAction { Import, Export };
struct NdsTitleSaveRequest {
    NdsTitleSaveAction action = NdsTitleSaveAction::Export;
    NdsTitleSaveConvert convert = nullptr;
};
// The pending request, if any; it stays pending until
// nds_title_save_finish().
bool nds_title_save_take_request(NdsTitleSaveRequest* request);
void nds_title_save_finish();
