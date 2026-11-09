#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../vendor/melonds/GPU3D.h"

namespace melonDS {
class NDS;
}

// Motion smoothing for games that draw 3D below the DS frame rate. A game at
// 30 holds each image for two DS frames, so the second frame is free: the
// renderer gets the image halfway between the last two, then the real image
// one DS frame late. An image drawn right after the last one (a game at 60)
// is shown one frame late too, as the last image again, so the halfway image
// only fills the frames such a game misses.
//
// The halfway image is drawn again from the game's own commands
// (gpu3d_gxstream.h): the newer image's command stream with its matrices
// blended towards the older one's, run through a private geometry engine.
// Host-only: the guest never sees it (save only after
// nds_gx_interp_show_real, whose pointers a savestate can serialise).

struct NdsGxInterpStats {
    uint64_t images = 0;         // halfway images handed to the renderer
    uint64_t units = 0;          // draw units that set matrices, in those
    uint64_t blended_units = 0;  // of those, found in the older image
    uint64_t skipped = 0;        // pairs of images too different to blend
};

enum class NdsGxInterpTarget {
    // The halfway list replaces GPU3D's render list (an accelerated
    // renderer that only presents).
    RenderList,
    // GPU3D's list stays real for the faithful renderer; the halfway list is
    // only handed out by nds_gx_interp_presented.
    Presentation,
};

// After GPU3D::VBlank. `flushed`: that VBlank consumed a SWAP_BUFFERS.
// `swap`: it did so with rendering on, so a new list is shown. `new_image`:
// its geometry differs from the last swap's.
void nds_gx_interp_after_vblank(melonDS::GPU3D& gpu3d, melonDS::NDS& nds,
                                bool active, NdsGxInterpTarget target,
                                bool flushed, bool swap, bool new_image,
                                NdsGxInterpStats* stats);
// The halfway list for this frame's presentation, when target is
// Presentation and one is showing.
bool nds_gx_interp_presented(melonDS::Polygon* const** list, uint32_t* count);
// Puts the real render list back.
void nds_gx_interp_show_real(melonDS::GPU3D& gpu3d);
// Forgets the last image and the stream being recorded (savestate load,
// reset).
void nds_gx_interp_reset();
// NDS_GX_CMD_TRACE: record each image's commands while smoothing is off too.
void nds_gx_interp_set_trace(FILE* file);
// The user saw something wrong just now: with NDS_GX_CMD_TRACE_RING, save
// the images around it. Any thread.
void nds_gx_interp_mark();
