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

namespace melonDS { class GPU; }

// Host lifetime: after the device exists, and before it goes away.
bool nds_plume3d_start();
void nds_plume3d_stop();
bool nds_plume3d_ready();

// The raster the next frame is drawn at. Clamped to [native, device limit].
void nds_plume3d_request_size(uint32_t width, uint32_t height);

// Draws the render list GPU3D latched at VBlank, after the faithful
// renderer's RenderFrame (which made the flat texture views coherent).
// texture_generation changes whenever those views were refreshed.
void nds_plume3d_render(const melonDS::GPU& gpu, uint64_t texture_generation);

// The 3D layer is empty (AbortFrame) or not ours to draw: the presenter
// falls back to the CPU composite until the next nds_plume3d_render.
void nds_plume3d_invalidate();
