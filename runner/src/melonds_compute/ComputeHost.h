// Host-context owner for the opt-in melonDS compute renderer.
// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
#pragma once
#include <cstdint>
#include <vector>
#include "../gpu2d.h"
#include "../frontend_window_size.h"

struct SDL_Window;
struct NdsGpu2dDirectFrame;

struct NdsComputePresentTicks {
    unsigned long long upload = 0;
    unsigned long long draw = 0;
    unsigned long long swap = 0;
    bool diagnostic = false;
    unsigned long long setup = 0;
    unsigned long long error = 0;
};

// Borrowed pointers refer to an OWNED frontend packet, not reusable device
// surfaces. Texture objects belong to the main GL owner and cannot be replaced
// by another producer request until this presentation has finished.
struct NdsComputePresentationFrame {
    NdsGpu2dDirectFrame direct{};
    NdsGpu2dHdFrame hd{};
    uint32_t hires_texture{};
    uint32_t native_texture{};
    uint8_t scale{1};
    bool has_direct{}, has_hd{};
};

// Two implementations sit behind this API, selected at build time:
// ComputeHost.cpp (OpenGL 4.3: COMPUTE and OPENGL) and src/plume/PlumeHost.cpp
// (Plume: Vulkan, or D3D12 on Windows). The frontend drives either unchanged.
bool nds_compute_host_start(SDL_Window* presentation_window = nullptr);
bool nds_compute_host_make_current();
bool nds_compute_host_has_visible_context();
// Whether this host's presenter composes the 3D layer itself, from the
// renderer's GPU surfaces (the OBJ-only direct path and the HD path). When it
// does not, the 2D compositor must keep the 3D layer in the CPU frame, and HD
// layer surfaces would only be emitted to be ignored.
bool nds_compute_host_composes_3d();
// Whether the OBJ-only direct path (no CPU composite at all) can be used: it
// needs the native 3D surface on the GPU. The GL host has one; the Plume host
// keeps native 3D on the CPU (soft renderer) and composes only HD frames.
bool nds_compute_host_presents_direct();
// What the 3D raster is sized by, from [display]: `percent` of the presented
// rect (resolution_scale; 0 = off) or else `fixed_scale` x native
// (internal_resolution). Only the Plume host uses it, and takes the rect's
// exact size rather than a whole multiple; the GL renderers' integer scale
// comes from nds_gpu3d_set_internal_scale as before.
void nds_compute_host_set_raster_request(unsigned percent, unsigned fixed_scale);
// [display] thin_outlines: 3D outlines one presented pixel wide instead of one
// DS pixel. Only the Plume host draws outlines above native.
void nds_compute_host_set_thin_outlines(bool thin);
// Where the host may keep files between runs (a pipeline cache). Before
// nds_compute_host_start.
void nds_compute_host_set_cache_directory(const char* directory);
// Observer only: the actual last swapped top-window image, in top-down
// 0xFFRRGGBB order. Does not re-render or advance emulation. Never used by
// ordinary presentation, capture hardware or performance measurements.
bool nds_compute_host_read_presented(std::vector<uint32_t>& pixels,
                                      unsigned& width, unsigned& height);
// Presentation geometry for the visible GL window, from [display]
// window_mode / scaling. Takes effect on the next present.
void nds_compute_host_set_present_mode(NdsWindowMode mode,
                                       NdsScalingFilter scaling);
// Optional host overlay (virtual stylus cursor, notices) for the next
// present: `width` x 192 ARGB, alpha 0 transparent, drawn over the image in
// the same rect. The pointer is read during that present only; pass nullptr
// to clear.
void nds_compute_host_set_overlay(const uint32_t* argb, unsigned width);
// Optional second LCD for the next present, in the nds_single_layout() inset:
// `width` x 192 ARGB at `percent` of the main screen, which moves left to make
// room. The pointer is read during that present only; pass nullptr to clear.
void nds_compute_host_set_inset(const uint32_t* argb, unsigned width,
                                int percent);
bool nds_compute_host_present_top(const unsigned int* fallback_pixels,
                                  unsigned short fallback_width,
                                  const NdsGpu2dDirectFrame* direct_frame,
                                  NdsComputePresentTicks* ticks,
                                  unsigned antialiasing = 0,
                                  const NdsComputePresentationFrame* owned = nullptr);
void nds_compute_host_stop();
bool nds_compute_host_active();

// Resolve a GL entry point through the host context. The vendored glad
// loader is generated for GL 4.3 core with no extensions, so anything the
// runner wants that only exists as an ARB extension at that level (notably
// ARB_buffer_storage's glBufferStorage, used for the persistently mapped
// 3D readback buffer) has to come from here rather than from glad. Returns
// nullptr when no context has been created or the driver lacks the entry.
typedef void (*NdsGlProc)(void);
NdsGlProc nds_compute_host_gl_proc(const char* name);

// GL identification strings, captured once when the context comes up. These
// were previously only printed to stderr, which meant a field diagnostics
// bundle could not say which GPU/driver produced it -- the single most
// load-bearing fact when a performance report arrives from a machine nobody
// has access to. Empty strings until a context has been created.
const char* nds_compute_host_gl_renderer();
const char* nds_compute_host_gl_vendor();
const char* nds_compute_host_gl_version();
