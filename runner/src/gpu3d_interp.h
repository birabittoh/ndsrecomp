#pragma once

#include <cstddef>
#include <cstdint>

#include "GPU3D.h"

// Motion smoothing for games that draw 3D below the DS frame rate. A game at
// 30 holds each image for two DS frames, so the second frame is free: the
// renderer gets a list halfway between the last two images, then the real
// image one DS frame late. Host-only: the blended list lives outside the
// GPU3D polygon banks and the guest never sees it (save only after
// nds_gx_interp_show_real, whose pointers a savestate can serialise).

struct NdsGxInterpStats {
    uint64_t images = 0;    // blended images handed to the renderer
    uint64_t polygons = 0;  // polygons in them
    uint64_t blended = 0;   // of those, polygons paired with the last image
};

// One polygon of an image, kept after its GPU3D bank has been reused.
struct NdsGxSnapshotPolygon {
    uint32_t attr = 0;
    uint32_t tex_param = 0;
    uint32_t tex_palette = 0;
    uint32_t num_vertices = 0;
    float x = 0.f;  // screen centroid
    float y = 0.f;
    uint64_t identity = 0;  // surface and texcoords: which model polygon it is
    melonDS::Vertex vertices[10];
    int32_t final_z[10] = {};
    melonDS::Polygon source;  // its vertex pointers do not outlive the bank
};

void nds_gx_snapshot_polygon(const melonDS::Polygon& polygon,
                             NdsGxSnapshotPolygon* out);

// Pairs two images' polygons, both in submission order. Writes the index in
// `a` for each polygon of `b`, or -1, and returns the number paired. Polygons
// are paired by what they are, their surface and texcoords, not by where they
// sit in the list: a model's polygons look alike, so one leaving the view must
// not pair each later one with its neighbour. Their positions only choose
// among repeats.
size_t nds_gx_match(const NdsGxSnapshotPolygon* a, size_t a_count,
                    const NdsGxSnapshotPolygon* b, size_t b_count,
                    int32_t* b_to_a);

struct NdsGxBlendCounts {
    size_t paired = 0;     // polygons of `current` with one in `last`
    size_t departing = 0;  // polygons of `last` still showing halfway
};

// Writes the image halfway from `last` to `current`, whose polygons are in
// `bank`. `polygons` gets one per polygon of `current`, in the same order, then
// `counts->departing` more; it needs room for 2048, and `vertices` for ten
// each. Corner v of polygon i is written at vertices[i * 10 + v]. False when
// the two images are different scenes.
//
// Polygons are not blended one by one: every polygon with a corner at the same
// clip-space vertex gets the same result, so a mesh cannot come apart.
bool nds_gx_blend_image(const NdsGxSnapshotPolygon* last, size_t last_count,
                        const NdsGxSnapshotPolygon* current,
                        const melonDS::Polygon* bank, size_t count,
                        int32_t render_width, melonDS::Polygon* polygons,
                        melonDS::Vertex* vertices, NdsGxBlendCounts* counts);

// After GPU3D::VBlank. `swap` means the game's SWAP_BUFFERS took effect there,
// `new_image` that its geometry differs from the last swap.
void nds_gx_interp_after_vblank(melonDS::GPU3D& gpu3d, bool active, bool swap,
                                bool new_image, NdsGxInterpStats* stats);
// Puts the real render list back.
void nds_gx_interp_show_real(melonDS::GPU3D& gpu3d);
// Forgets the last image, for a savestate load.
void nds_gx_interp_reset();
