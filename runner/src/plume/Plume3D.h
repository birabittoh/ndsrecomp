// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// The DS 3D engine drawn through Plume, for presentation only.
//
// The integer soft renderer stays the installed melonDS Renderer3D and the
// single source of every faithful consumer (2D compositor lines, display
// capture, readback). This renderer draws the same latched render list into
// a raster of any size, which only the host's HD compose samples, so nothing
// ever reads it back to the CPU.
//
// The raster size is free: the host asks for the size of the rect it
// presents into (times resolution_scale), not a whole multiple of 256x192.
// No Plume types here: gpu3d.cpp includes this.
#pragma once

#include <cstdint>
#include <string>

namespace melonDS { class GPU; struct Polygon; }

// Host lifetime: after the device exists, and before it goes away.
// `pipeline_cache_path`: a file the driver's pipeline cache is loaded from
// and saved to, or empty for none.
bool nds_plume3d_start(const std::string& pipeline_cache_path);
void nds_plume3d_stop();
bool nds_plume3d_ready();

// The raster the next frame is drawn at, clamped to the device limit, and
// the outline width in raster pixels (0: one DS pixel). A changed size is
// taken once it has been asked for over several frames.
void nds_plume3d_request_size(uint32_t width, uint32_t height, uint32_t edge_pixels);

// Draws a render list (the one GPU3D latched at VBlank, or smooth_3d's
// halfway list) after the faithful renderer's RenderFrame, which made the
// flat texture views coherent. texture_generation changes whenever those
// views were refreshed.
void nds_plume3d_render(const melonDS::GPU& gpu, melonDS::Polygon* const* polygons,
                        uint32_t count, uint64_t texture_generation);

// The 3D layer is empty (AbortFrame) or not ours to draw: the presenter
// falls back to the CPU composite until the next nds_plume3d_render.
void nds_plume3d_invalidate();
