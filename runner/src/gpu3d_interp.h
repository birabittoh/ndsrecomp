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
    melonDS::Vertex vertices[10];
    int32_t final_z[10] = {};
};

void nds_gx_snapshot_polygon(const melonDS::Polygon& polygon,
                             NdsGxSnapshotPolygon* out);

// Pairs two images' polygons, both in submission order. Writes the index in
// `a` for each polygon of `b`, or -1, and returns the number paired. Polygons
// come and go as they leave the view, so a mismatch looks ahead for the point
// where the sequences line up again. Textures repeat across a scene, so a pair
// must also be close on screen.
size_t nds_gx_match(const NdsGxSnapshotPolygon* a, size_t a_count,
                    const NdsGxSnapshotPolygon* b, size_t b_count,
                    int32_t* b_to_a);

// Writes the polygon halfway from `from` to `to`, its vertices into `vertices`
// (room for to.NumVertices). False when they are too different to blend, and
// `out` is then not usable.
bool nds_gx_blend_polygon(const NdsGxSnapshotPolygon& from,
                          const melonDS::Polygon& to, int32_t render_width,
                          melonDS::Polygon* out, melonDS::Vertex* vertices);

// Writes `polygon` moved by (dx, dy) pixels, leaving vertices on the screen
// edge in place so that clipped geometry still reaches it.
void nds_gx_shift_polygon(const melonDS::Polygon& polygon, float dx, float dy,
                          int32_t render_width, melonDS::Polygon* out,
                          melonDS::Vertex* vertices);

// After GPU3D::VBlank. `swap` means the game's SWAP_BUFFERS took effect there,
// `new_image` that its geometry differs from the last swap.
void nds_gx_interp_after_vblank(melonDS::GPU3D& gpu3d, bool active, bool swap,
                                bool new_image, NdsGxInterpStats* stats);
// Puts the real render list back.
void nds_gx_interp_show_real(melonDS::GPU3D& gpu3d);
// Forgets the last image, for a savestate load.
void nds_gx_interp_reset();
