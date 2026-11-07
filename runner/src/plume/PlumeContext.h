// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// What the Plume host shares with the Plume 3D renderer: one device and one
// direct queue, both owned by PlumeHost.cpp. Everything is recorded and
// submitted on the main (render owner) thread: the frontend presents there
// and nds_gpu3d_vcount215 is marshalled there, so submission order on the
// single queue is the order of the calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "plume_render_interface.h"

struct NdsPlumeContext {
    plume::RenderInterface* render_interface = nullptr;
    plume::RenderDevice* device = nullptr;
    plume::RenderCommandQueue* queue = nullptr;
};

// Null until the host has a device (headless runs never create one).
const NdsPlumeContext* nds_plume_context();

// Waits for every frame the host has in flight. A caller replacing a
// resource the presenter samples calls this first.
void nds_plume_host_wait_idle();

// A shader from whichever embedded blob the API consumes.
std::unique_ptr<plume::RenderShader> nds_plume_create_shader(
    const void* spirv, size_t spirv_size, const void* dxil, size_t dxil_size,
    const char* entry);

// The Plume 3D renderer's finished raster (RGB6/alpha5 normalised by 63 and
// 31, RGBA order) in SHADER_READ layout, or null when the last frame has no
// raster the presenter may use (none drawn yet, aborted, clear bitmap).
plume::RenderTexture* nds_plume3d_output(uint32_t* width, uint32_t* height);
