// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// The Plume renderer host: the ComputeHost.h API over Plume, an RHI over
// Vulkan (Linux, and Windows on request) and D3D12 (Windows). It replaces the
// GL host (src/melonds_compute/ComputeHost.cpp) in NDS_ACCELERATED_RENDERER=
// PLUME builds, and owns the top window's presentation: the window carries a
// Vulkan surface or a D3D12 swap chain, never a GL context.
//
// Shaders are HLSL in src/plume/shaders, compiled by DXC at build time
// (cmake/NdsPlume.cmake) and linked in as byte arrays; nothing is compiled at
// startup.
//
// Scope today: presentation only. The 3D layer is still rasterized by the
// integer software renderer and arrives inside the CPU composite, so this
// host reports nds_compute_host_composes_3d() == false and the frontend keeps
// the direct OBJ and HD surfaces switched off. What it does do is everything
// the GL presenter does with that composite: window mode and scaling filter
// (nds_single_layout, shared with the GL path and mouse-to-touch), the
// integer-mode box resolve, the single-screen inset and the frontend overlay.
//
// Frames in flight: two slots, each with its own command list, fence, upload
// buffer and textures. A slot is reused only after its fence has signalled,
// so nothing the GPU may still read is ever overwritten.
#include "ComputeHost.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "plume_render_interface.h"

#include "gpu3d.h"

#include "shaders/ndsFullscreenVert.hlsl.spirv.h"
#include "shaders/ndsBlitFrag.hlsl.spirv.h"
#if defined(_WIN32)
#include "shaders/ndsFullscreenVert.hlsl.dxil.h"
#include "shaders/ndsBlitFrag.hlsl.dxil.h"
#endif

namespace plume {
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#if defined(_WIN32)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}  // namespace plume

namespace {
using namespace plume;

constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
// The frontend's frames are 0xAARRGGBB words: little endian bytes B, G, R, A.
constexpr RenderFormat kImageFormat = RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t kSwapChainBuffers = 3;
constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxWidth = 448;
constexpr uint32_t kHeight = 192;
// D3D12 needs placed-footprint rows on 256 bytes; Vulkan accepts that too.
constexpr uint32_t kRowAlignment = 256;
constexpr uint32_t kRowPitch =
    (kMaxWidth * 4u + kRowAlignment - 1u) / kRowAlignment * kRowAlignment;
constexpr uint64_t kImageBytes = uint64_t(kRowPitch) * kHeight;

// Mirrors blit.frag.hlsl.
constexpr uint32_t kBlitBoxResolve = 1u;
constexpr uint32_t kBlitKeepAlpha = 2u;
struct BlitConstants {
    uint32_t flags;
    uint32_t pad;
    int32_t rect_size[2];
};

// The images one present uploads: the shown LCD, the other one as the
// single-screen inset, and the frontend overlay (stylus, notices, FPS).
enum Image : uint32_t { kMain, kInset, kOverlay, kImageCount };

struct ImageSlot {
    std::unique_ptr<RenderTexture> texture;
    std::unique_ptr<RenderDescriptorSet> set;
    int width = 0;
};

struct FrameSlot {
    std::unique_ptr<RenderCommandList> commands;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderCommandSemaphore> acquire;
    // kImageCount images of kImageBytes each, at fixed offsets.
    std::unique_ptr<RenderBuffer> staging;
    ImageSlot images[kImageCount];
    bool submitted = false;
};

struct Backend {
    std::unique_ptr<RenderInterface> render_interface;
    std::unique_ptr<RenderDevice> device;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderSwapChain> swap_chain;
    std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> release_semaphores;
    std::unique_ptr<RenderPipelineLayout> layout;
    std::unique_ptr<RenderShader> vertex_shader;
    std::unique_ptr<RenderShader> blit_shader;
    std::unique_ptr<RenderPipeline> opaque_pipeline;
    std::unique_ptr<RenderPipeline> blended_pipeline;
    std::unique_ptr<RenderSampler> nearest;
    std::unique_ptr<RenderSampler> linear;
    FrameSlot slots[kFramesInFlight];
    uint32_t slot = 0;
    const char* api = "";
};

Backend g_backend;
SDL_Window* g_window = nullptr;
bool g_active = false;
bool g_visible = false;
bool g_resize_pending = false;
NdsWindowMode g_window_mode = NdsWindowMode::Letterbox;
NdsScalingFilter g_scaling = NdsScalingFilter::Integer;
const uint32_t* g_overlay_pixels = nullptr;
unsigned g_overlay_width = 0;
const uint32_t* g_inset_pixels = nullptr;
unsigned g_inset_width = 0;
int g_inset_percent = 0;
uint64_t g_presented = 0;
uint64_t g_dropped = 0;

// Same role as the GL host's identity strings: read by host_info for the
// diagnostics bundle, so fixed buffers that outlive any reader.
char g_renderer_name[192] = {};
char g_vendor_name[64] = {};
char g_api_version[64] = {};

const char* vendor_name(RenderDeviceVendor vendor) {
    switch (vendor) {
        case RenderDeviceVendor::AMD: return "AMD";
        case RenderDeviceVendor::NVIDIA: return "NVIDIA";
        case RenderDeviceVendor::INTEL: return "Intel";
        case RenderDeviceVendor::APPLE: return "Apple";
        default: return "unknown vendor";
    }
}

void wait_slot(FrameSlot& slot) {
    if (!slot.submitted) return;
    g_backend.queue->waitForCommandFence(slot.fence.get());
    slot.submitted = false;
}

void wait_all_slots() {
    for (FrameSlot& slot : g_backend.slots) wait_slot(slot);
}

void create_framebuffers() {
    g_backend.framebuffers.clear();
    RenderSwapChain& chain = *g_backend.swap_chain;
    for (uint32_t i = 0; i < chain.getTextureCount(); ++i) {
        const RenderTexture* color = chain.getTexture(i);
        RenderFramebufferDesc desc;
        desc.colorAttachments = &color;
        desc.colorAttachmentsCount = 1;
        g_backend.framebuffers.push_back(g_backend.device->createFramebuffer(desc));
    }
    // One release semaphore per swap chain image: present waits on the one
    // its image's submission signalled.
    while (g_backend.release_semaphores.size() < chain.getTextureCount())
        g_backend.release_semaphores.push_back(
            g_backend.device->createCommandSemaphore());
}

// Every frame is drawn at the window's current size: apply a pending or
// required resize before acquiring. A resize replaces the swap chain images,
// which a frame still in flight may be drawing into, so it drains the queue.
void resize_if_needed() {
    RenderSwapChain& chain = *g_backend.swap_chain;
    if (!g_resize_pending && !chain.needsResize()) return;
    g_resize_pending = false;
    wait_all_slots();
    g_backend.framebuffers.clear();
    if (!chain.resize()) {
        std::fprintf(stderr, "[plume] swap chain resize failed\n");
        g_resize_pending = true;
        return;
    }
    if (!chain.isEmpty()) create_framebuffers();
}

std::unique_ptr<RenderShader> create_shader(const void* spirv, size_t spirv_size,
                                            const void* dxil, size_t dxil_size,
                                            const char* entry) {
    const RenderShaderFormat format =
        g_backend.render_interface->getCapabilities().shaderFormat;
    if (format == RenderShaderFormat::SPIRV)
        return g_backend.device->createShader(spirv, spirv_size, entry, format);
    if (format == RenderShaderFormat::DXIL && dxil)
        return g_backend.device->createShader(dxil, dxil_size, entry, format);
    return nullptr;
}

RenderDescriptorSetDesc image_set_desc() {
    static const RenderDescriptorRange ranges[] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::SAMPLER, 1, 1),
    };
    return RenderDescriptorSetDesc(ranges, 2);
}

bool create_pipelines() {
    const RenderDescriptorSetDesc set_desc = image_set_desc();
    const RenderPushConstantRange push_constants(
        0, 0, 0, sizeof(BlitConstants), RenderShaderStageFlag::PIXEL);
    RenderPipelineLayoutDesc layout_desc;
    layout_desc.pushConstantRanges = &push_constants;
    layout_desc.pushConstantRangesCount = 1;
    layout_desc.descriptorSetDescs = &set_desc;
    layout_desc.descriptorSetDescsCount = 1;
    g_backend.layout = g_backend.device->createPipelineLayout(layout_desc);
    if (!g_backend.layout) return false;

#if defined(_WIN32)
    const void* vert_dxil = ndsFullscreenVertBlobDXIL;
    const size_t vert_dxil_size = sizeof(ndsFullscreenVertBlobDXIL);
    const void* blit_dxil = ndsBlitFragBlobDXIL;
    const size_t blit_dxil_size = sizeof(ndsBlitFragBlobDXIL);
#else
    const void* vert_dxil = nullptr;
    const size_t vert_dxil_size = 0;
    const void* blit_dxil = nullptr;
    const size_t blit_dxil_size = 0;
#endif
    g_backend.vertex_shader = create_shader(
        ndsFullscreenVertBlobSPIRV, sizeof(ndsFullscreenVertBlobSPIRV),
        vert_dxil, vert_dxil_size, "VSMain");
    g_backend.blit_shader = create_shader(
        ndsBlitFragBlobSPIRV, sizeof(ndsBlitFragBlobSPIRV),
        blit_dxil, blit_dxil_size, "PSMain");
    if (!g_backend.vertex_shader || !g_backend.blit_shader) {
        std::fprintf(stderr,
            "[plume] no embedded shader blob for this API's shader format\n");
        return false;
    }

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_backend.layout.get();
    desc.vertexShader = g_backend.vertex_shader.get();
    desc.pixelShader = g_backend.blit_shader.get();
    desc.renderTargetFormat[0] = kSwapChainFormat;
    desc.renderTargetCount = 1;
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = RenderCullMode::NONE;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    g_backend.opaque_pipeline = g_backend.device->createGraphicsPipeline(desc);
    // The overlay is straight alpha over the image, as glBlendFunc(SRC_ALPHA,
    // ONE_MINUS_SRC_ALPHA) in the GL presenter.
    desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
    g_backend.blended_pipeline = g_backend.device->createGraphicsPipeline(desc);
    if (!g_backend.opaque_pipeline || !g_backend.blended_pipeline) return false;

    RenderSamplerDesc sampler;
    sampler.mipmapMode = RenderMipmapMode::NEAREST;
    sampler.addressU = RenderTextureAddressMode::CLAMP;
    sampler.addressV = RenderTextureAddressMode::CLAMP;
    sampler.addressW = RenderTextureAddressMode::CLAMP;
    sampler.minFilter = sampler.magFilter = RenderFilter::NEAREST;
    g_backend.nearest = g_backend.device->createSampler(sampler);
    sampler.minFilter = sampler.magFilter = RenderFilter::LINEAR;
    g_backend.linear = g_backend.device->createSampler(sampler);
    return g_backend.nearest && g_backend.linear;
}

bool create_slots() {
    for (FrameSlot& slot : g_backend.slots) {
        slot.commands = g_backend.queue->createCommandList();
        slot.fence = g_backend.device->createCommandFence();
        slot.acquire = g_backend.device->createCommandSemaphore();
        slot.staging = g_backend.device->createBuffer(
            RenderBufferDesc::UploadBuffer(kImageBytes * kImageCount));
        if (!slot.commands || !slot.fence || !slot.acquire || !slot.staging)
            return false;
        for (ImageSlot& image : slot.images) {
            image.set = g_backend.device->createDescriptorSet(image_set_desc());
            if (!image.set) return false;
        }
    }
    return true;
}

RenderWindow render_window_for(SDL_Window* window) {
#if defined(_WIN32)
    return static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
#elif defined(PLUME_SDL3_VULKAN_ENABLED)
    return window;
#else
    (void)window;
    return RenderWindow{};
#endif
}

bool create_interface() {
#if defined(_WIN32)
    // D3D12 unless NDS_PLUME_API=vulkan, each falling back to the other.
    const char* requested = std::getenv("NDS_PLUME_API");
    const bool vulkan_first = requested && (std::strcmp(requested, "vulkan") == 0 ||
                                            std::strcmp(requested, "vk") == 0);
    if (vulkan_first) {
        g_backend.render_interface = CreateVulkanInterface();
        g_backend.api = "Vulkan";
    }
    if (!g_backend.render_interface) {
        g_backend.render_interface = CreateD3D12Interface();
        g_backend.api = "D3D12";
    }
    if (!g_backend.render_interface && !vulkan_first) {
        g_backend.render_interface = CreateVulkanInterface();
        g_backend.api = "Vulkan";
    }
    return g_backend.render_interface != nullptr;
#elif defined(PLUME_SDL3_VULKAN_ENABLED)
    g_backend.render_interface = CreateVulkanInterface();
    g_backend.api = "Vulkan";
    return g_backend.render_interface != nullptr;
#else
    // Apple needs a {NSWindow, CAMetalLayer} pair (SDL_Metal_CreateView);
    // not wired yet.
    return false;
#endif
}

bool start_presenter(SDL_Window* window) {
    if (!create_interface()) {
        std::fprintf(stderr, "[plume] no render interface could be created\n");
        return false;
    }
    g_backend.device = g_backend.render_interface->createDevice();
    if (!g_backend.device) {
        std::fprintf(stderr, "[plume] the %s interface created no device\n",
                     g_backend.api);
        return false;
    }
    g_backend.queue =
        g_backend.device->createCommandQueue(RenderCommandListType::DIRECT);
    if (!g_backend.queue || !create_slots()) {
        std::fprintf(stderr, "[plume] queue or frame slot creation failed\n");
        return false;
    }
    g_backend.swap_chain = g_backend.queue->createSwapChain(RenderSwapChainDesc(
        render_window_for(window), kSwapChainFormat, kSwapChainBuffers));
    if (!g_backend.swap_chain) {
        std::fprintf(stderr, "[plume] no swap chain on the presentation window\n");
        return false;
    }
    // The GL presenter swaps with interval 0 and the frontend paces itself
    // from the audio queue, so a present must not block on the vblank here
    // either: vsync off (immediate where the surface supports it).
    g_backend.swap_chain->setVsyncEnabled(false);
    // A swap chain has no images until its first resize.
    if (!g_backend.swap_chain->resize()) {
        std::fprintf(stderr, "[plume] initial swap chain resize failed\n");
        return false;
    }
    if (!g_backend.swap_chain->isEmpty()) create_framebuffers();
    if (!create_pipelines()) {
        std::fprintf(stderr, "[plume] presenter pipeline creation failed\n");
        return false;
    }

    const RenderDeviceDescription& description = g_backend.device->getDescription();
    std::snprintf(g_renderer_name, sizeof(g_renderer_name), "%s",
                  description.name.c_str());
    std::snprintf(g_vendor_name, sizeof(g_vendor_name), "%s",
                  vendor_name(description.vendor));
    std::snprintf(g_api_version, sizeof(g_api_version), "Plume %s",
                  g_backend.api);
    std::fprintf(stderr, "[plume] %s on %s, swap chain %ux%u, %u images\n",
                 g_backend.api, g_renderer_name, g_backend.swap_chain->getWidth(),
                 g_backend.swap_chain->getHeight(),
                 g_backend.swap_chain->getTextureCount());
    return true;
}

void release_backend() {
    if (g_backend.queue) wait_all_slots();
    for (FrameSlot& slot : g_backend.slots) slot = FrameSlot{};
    g_backend.framebuffers.clear();
    g_backend.release_semaphores.clear();
    g_backend.swap_chain.reset();
    g_backend.blended_pipeline.reset();
    g_backend.opaque_pipeline.reset();
    g_backend.blit_shader.reset();
    g_backend.vertex_shader.reset();
    g_backend.layout.reset();
    g_backend.nearest.reset();
    g_backend.linear.reset();
    g_backend.queue.reset();
    g_backend.device.reset();
    g_backend.render_interface.reset();
    g_backend.slot = 0;
    g_backend.api = "";
    g_window = nullptr;
    g_active = false;
    g_visible = false;
    g_resize_pending = false;
}

// Copies `width` x 192 ARGB rows into the slot's staging area for `image`,
// and (re)creates the slot's texture if the width changed. Safe without a
// wait: this slot's previous frame has retired, and the other slot has
// textures of its own.
bool stage_image(FrameSlot& slot, uint8_t* staging, Image image,
                 const uint32_t* pixels, int width) {
    ImageSlot& target = slot.images[image];
    if (target.width != width || !target.texture) {
        target.texture = g_backend.device->createTexture(
            RenderTextureDesc::Texture2D(uint32_t(width), kHeight, 1, kImageFormat));
        if (!target.texture) return false;
        target.width = width;
        target.set->setTexture(0, target.texture.get(),
                               RenderTextureLayout::SHADER_READ);
    }
    uint8_t* dst = staging + kImageBytes * image;
    const size_t row_bytes = size_t(width) * 4u;
    for (uint32_t y = 0; y < kHeight; ++y)
        std::memcpy(dst + size_t(y) * kRowPitch, pixels + size_t(y) * width,
                    row_bytes);
    return true;
}

void record_upload(FrameSlot& slot, Image image) {
    RenderCommandList& commands = *slot.commands;
    RenderTexture* texture = slot.images[image].texture.get();
    commands.barriers(RenderBarrierStage::COPY,
                      RenderTextureBarrier(texture, RenderTextureLayout::COPY_DEST));
    commands.copyTextureRegion(
        RenderTextureCopyLocation::Subresource(texture),
        RenderTextureCopyLocation::PlacedFootprint(
            slot.staging.get(), kImageFormat, uint32_t(slot.images[image].width),
            kHeight, 1, kRowPitch / 4u, kImageBytes * image));
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
}

// Draws `image` into `rect` (drawable pixels, top-left origin; may extend
// past the window in Original mode, which the scissor clips).
void record_blit(FrameSlot& slot, Image image, const NdsPresentRect& rect,
                 bool linear, uint32_t flags, uint32_t fb_width,
                 uint32_t fb_height) {
    if (rect.w <= 0 || rect.h <= 0) return;
    const int32_t left = std::max(rect.x, 0);
    const int32_t top = std::max(rect.y, 0);
    const int32_t right = std::min<int32_t>(rect.x + rect.w, int32_t(fb_width));
    const int32_t bottom = std::min<int32_t>(rect.y + rect.h, int32_t(fb_height));
    if (right <= left || bottom <= top) return;
    RenderCommandList& commands = *slot.commands;
    ImageSlot& source = slot.images[image];
    source.set->setSampler(1, linear ? g_backend.linear.get()
                                     : g_backend.nearest.get());
    commands.setPipeline((flags & kBlitKeepAlpha)
                             ? g_backend.blended_pipeline.get()
                             : g_backend.opaque_pipeline.get());
    commands.setGraphicsDescriptorSet(source.set.get(), 0);
    const BlitConstants constants{flags, 0u, {rect.w, rect.h}};
    commands.setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
    commands.setViewports(RenderViewport(float(rect.x), float(rect.y),
                                         float(rect.w), float(rect.h)));
    commands.setScissors(RenderRect(left, top, right, bottom));
    commands.drawInstanced(3, 1, 0, 0);
}
}  // namespace

bool nds_compute_host_start(SDL_Window* presentation_window)
{
    const NdsGpu3dRendererPolicy policy = nds_gpu3d_renderer_policy();
    if (policy == NdsGpu3dRendererPolicy::Soft) return true;
    if (g_active) return true;
    const bool required = policy == NdsGpu3dRendererPolicy::Compute;
    auto fail_or_fallback = [&](const char* reason) {
        nds_gpu3d_restore_soft_renderer();
        release_backend();
        if (required) return false;
        std::fprintf(stderr,
            "[gpu3d] Plume auto-selection failed (%s); using threaded software\n",
            reason);
        return true;
    };

    // Headless and serve runs have no window to present to, and until the 3D
    // passes exist there is nothing for a device to do without one.
    if (presentation_window) {
        SDL_SetMainReady();
        g_window = presentation_window;
        if (!start_presenter(presentation_window))
            return fail_or_fallback("Plume presenter initialization");
        g_visible = true;
    }
    if (!nds_gpu3d_use_compute_renderer())
        return fail_or_fallback("renderer initialization");
    g_active = true;
    std::fprintf(stderr, "[gpu3d] renderer: %s\n",
                 nds_gpu3d_compiled_accelerated_backend());
    return true;
}

bool nds_compute_host_make_current()
{
    // No context to bind: Plume objects are not tied to a thread.
    return true;
}

bool nds_compute_host_has_visible_context()
{
    return g_active && g_visible;
}

bool nds_compute_host_composes_3d()
{
    // Not until the Plume 3D passes exist: the CPU composite carries 3D.
    return false;
}

bool nds_compute_host_read_presented(std::vector<uint32_t>& pixels,
                                      unsigned& width, unsigned& height)
{
    // Observer-only (debug server). Swap chain images are not copy sources
    // here; this needs an offscreen copy of the presented rect first.
    pixels.clear();
    width = height = 0;
    return false;
}

void nds_compute_host_set_overlay(const uint32_t* argb, unsigned width)
{
    g_overlay_pixels = width > 0 && width <= kMaxWidth ? argb : nullptr;
    g_overlay_width = g_overlay_pixels ? width : 0;
}

void nds_compute_host_set_inset(const uint32_t* argb, unsigned width,
                                int percent)
{
    const bool shown = argb && percent > 0 && width > 0 && width <= kMaxWidth;
    g_inset_pixels = shown ? argb : nullptr;
    g_inset_width = shown ? width : 0;
    g_inset_percent = shown ? percent : 0;
}

void nds_compute_host_set_present_mode(NdsWindowMode mode,
                                       NdsScalingFilter scaling)
{
    g_window_mode = mode;
    g_scaling = scaling;
}

bool nds_compute_host_present_top(const unsigned int* fallback_pixels,
                                  unsigned short fallback_width,
                                  const NdsGpu2dDirectFrame* direct_frame,
                                  NdsComputePresentTicks* ticks,
                                  unsigned antialiasing,
                                  const NdsComputePresentationFrame* owned)
{
    // The direct and HD surfaces are never produced for this host (see
    // nds_compute_host_composes_3d), and antialiasing resolves samples of a
    // denser 3D raster that does not exist yet.
    (void)direct_frame;
    (void)antialiasing;
    (void)owned;
    if (!g_active || !g_visible || !ticks || !fallback_pixels) return false;
    const int width = fallback_width;
    if (width <= 0 || width > int(kMaxWidth)) return false;
    const auto diagnostic_begin = ticks->diagnostic ? SDL_GetPerformanceCounter() : 0;

    resize_if_needed();
    RenderSwapChain& chain = *g_backend.swap_chain;
    // Minimised (zero-sized surface): nothing to present to, and not an error.
    if (chain.isEmpty() || g_backend.framebuffers.empty()) return true;

    FrameSlot& slot = g_backend.slots[g_backend.slot];
    uint32_t image_index = 0;
    if (!chain.acquireTexture(slot.acquire.get(), &image_index) ||
        image_index >= g_backend.framebuffers.size()) {
        // Usually an out-of-date surface between the check above and here; the
        // next frame's resize picks it up. A dropped frame, not a failure.
        g_resize_pending = true;
        if (++g_dropped == 1)
            std::fprintf(stderr, "[plume] swap chain acquire failed; frames are "
                                 "dropped until it recovers\n");
        return true;
    }

    unsigned long long start = SDL_GetPerformanceCounter();
    if (ticks->diagnostic) ticks->setup = start - diagnostic_begin;
    auto* staging = static_cast<uint8_t*>(slot.staging->map());
    if (!staging) return false;
    const bool inset = g_inset_pixels != nullptr;
    const bool overlay = g_overlay_pixels != nullptr;
    bool staged = stage_image(slot, staging, kMain, fallback_pixels, width);
    if (staged && inset)
        staged = stage_image(slot, staging, kInset, g_inset_pixels, int(g_inset_width));
    if (staged && overlay)
        staged = stage_image(slot, staging, kOverlay, g_overlay_pixels,
                             int(g_overlay_width));
    slot.staging->unmap();
    if (!staged) return false;
    ticks->upload += SDL_GetPerformanceCounter() - start;

    start = SDL_GetPerformanceCounter();
    const uint32_t fb_width = chain.getWidth();
    const uint32_t fb_height = chain.getHeight();
    RenderTexture* backbuffer = chain.getTexture(image_index);
    RenderCommandList& commands = *slot.commands;
    commands.begin();
    record_upload(slot, kMain);
    if (inset) record_upload(slot, kInset);
    if (overlay) record_upload(slot, kOverlay);
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
    commands.setFramebuffer(g_backend.framebuffers[image_index].get());
    commands.setViewports(RenderViewport(0.0f, 0.0f, float(fb_width), float(fb_height)));
    commands.setScissors(RenderRect(0, 0, int32_t(fb_width), int32_t(fb_height)));
    commands.clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    commands.setGraphicsPipelineLayout(g_backend.layout.get());

    const NdsSingleLayout layout = nds_single_layout(
        g_window_mode, g_scaling, int(fb_width), int(fb_height), width,
        int(kHeight), g_inset_percent);
    const NdsPresentRect& rect = layout.main;
    // The GL presenter's filter choice, at the CPU composite's own density:
    // nearest at the exact rect, bilinear resampled, and a box resolve where
    // integer (or nearest at 2x and up) would otherwise skip source texels.
    const bool supersampled = width > rect.w || int(kHeight) > rect.h;
    const bool box_resolve =
        (g_scaling == NdsScalingFilter::Integer && supersampled) ||
        (g_scaling == NdsScalingFilter::Nearest && width >= 2 * rect.w &&
         int(kHeight) >= 2 * rect.h);
    const bool bilinear = g_scaling == NdsScalingFilter::Bilinear;
    record_blit(slot, kMain, rect, bilinear && !box_resolve,
                box_resolve ? kBlitBoxResolve : 0u, fb_width, fb_height);
    if (inset && layout.inset.w > 0 && layout.inset.h > 0) {
        // Point sampling a shrunk inset drops whole rows of text.
        const bool linear = bilinear || layout.inset.w < int(g_inset_width);
        record_blit(slot, kInset, layout.inset, linear, 0u, fb_width, fb_height);
    }
    if (overlay)
        record_blit(slot, kOverlay, rect, bilinear, kBlitKeepAlpha, fb_width,
                    fb_height);
    commands.barriers(RenderBarrierStage::NONE,
                      RenderTextureBarrier(backbuffer, RenderTextureLayout::PRESENT));
    commands.end();

    const RenderCommandList* submit = slot.commands.get();
    RenderCommandSemaphore* wait = slot.acquire.get();
    RenderCommandSemaphore* signal = g_backend.release_semaphores[image_index].get();
    g_backend.queue->executeCommandLists(&submit, 1, &wait, 1, &signal, 1,
                                         slot.fence.get());
    slot.submitted = true;
    ticks->draw += SDL_GetPerformanceCounter() - start;

    start = SDL_GetPerformanceCounter();
    if (!chain.present(image_index, &signal, 1)) {
        g_resize_pending = true;
        if (++g_dropped == 1)
            std::fprintf(stderr, "[plume] present failed (0x%llx); the swap chain "
                                 "may be out of date\n",
                         static_cast<unsigned long long>(chain.getLastError()));
    }
    // Frames in flight: the next slot's last frame has had a whole frame of
    // CPU time to finish, and only it is waited on.
    g_backend.slot = (g_backend.slot + 1u) % kFramesInFlight;
    wait_slot(g_backend.slots[g_backend.slot]);
    ticks->swap += SDL_GetPerformanceCounter() - start;
    if (++g_presented % 3600 == 0 && g_dropped != 0)
        std::fprintf(stderr, "[plume] presented %llu frames, %llu dropped\n",
                     static_cast<unsigned long long>(g_presented),
                     static_cast<unsigned long long>(g_dropped));
    return true;
}

void nds_compute_host_stop()
{
    if (g_active) nds_gpu3d_restore_soft_renderer();
    release_backend();
}

bool nds_compute_host_active()
{
    return g_active;
}

NdsGlProc nds_compute_host_gl_proc(const char*)
{
    return nullptr;
}

const char* nds_compute_host_gl_renderer()
{
    return g_renderer_name;
}

const char* nds_compute_host_gl_vendor()
{
    return g_vendor_name;
}

const char* nds_compute_host_gl_version()
{
    return g_api_version;
}
