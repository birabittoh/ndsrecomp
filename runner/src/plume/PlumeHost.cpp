// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// The Plume renderer host: the ComputeHost.h API over Plume, an RHI over
// Vulkan (Linux, Apple through MoltenVK, and Windows on request) and D3D12
// (Windows). It replaces the GL host (src/melonds_compute/ComputeHost.cpp) in
// NDS_ACCELERATED_RENDERER=PLUME builds, and owns the top window's
// presentation: the window carries a Vulkan surface (on Apple one made from
// the CAMetalLayer of an SDL Metal view) or a D3D12 swap chain, never a GL
// context.
//
// Shaders are HLSL in src/plume/shaders, compiled by DXC at build time
// (cmake/NdsPlume.cmake) and linked in as byte arrays; nothing is compiled at
// startup.
//
// Two ways a frame reaches the window. The CPU composite (native 3D from the
// integer soft renderer) is blitted with the GL presenter's filters: window
// mode and scaling (nds_single_layout, shared with the GL path and
// mouse-to-touch), the integer-mode box resolve, the single-screen inset and
// the frontend overlay. Above 1x the HD path composes instead: the 2D
// compositor's HD surfaces plus the Plume 3D raster (Plume3D.cpp), whose size
// this host picks from the rect it presents into, so 3D is drawn at the
// window's own resolution rather than at a whole multiple of 256x192. The
// OBJ-only direct path is never used here (nds_compute_host_presents_direct).
//
// Frames in flight: two slots, each with its own command list, fence, upload
// buffer and textures. A slot is reused only after its fence has signalled,
// so nothing the GPU may still read is ever overwritten.
#include "ComputeHost.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#include <dlfcn.h>
#endif

#include "plume_render_interface.h"

#include "gpu2d.h"
#include "gpu3d.h"
#include "Plume3D.h"
#include "PlumeContext.h"

#include "shaders/ndsFullscreenVert.hlsl.spirv.h"
#include "shaders/ndsBlitFrag.hlsl.spirv.h"
#include "shaders/ndsComposeFrag.hlsl.spirv.h"
#if defined(_WIN32)
#include "shaders/ndsFullscreenVert.hlsl.dxil.h"
#include "shaders/ndsBlitFrag.hlsl.dxil.h"
#include "shaders/ndsComposeFrag.hlsl.dxil.h"
#endif

namespace plume {
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
#if defined(__APPLE__)
void SetVulkanGetInstanceProcAddr(void* getInstanceProcAddr);
#endif
#if defined(_WIN32)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
}  // namespace plume

namespace {
using namespace plume;

// Adreno surfaces list no B8G8R8A8_UNORM; Plume then makes the swap chain
// with VK_FORMAT_UNDEFINED, which the pipelines below cannot draw into.
#if defined(__ANDROID__)
constexpr RenderFormat kSwapChainFormat = RenderFormat::R8G8B8A8_UNORM;
#else
constexpr RenderFormat kSwapChainFormat = RenderFormat::B8G8R8A8_UNORM;
#endif
// The frontend's frames are 0xAARRGGBB words: little endian bytes B, G, R, A.
constexpr RenderFormat kImageFormat = RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t kSwapChainBuffers = 3;
constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kNativeWidth = 256;
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
// Host only (the shader ignores it): with kBlitKeepAlpha, the image's colour
// is premultiplied by its alpha, drawn with (ONE, INV_SRC_ALPHA).
constexpr uint32_t kBlitPremultiplied = 4u;
struct BlitConstants {
    uint32_t flags;
    uint32_t pad;
    int32_t rect_size[2];
};

// Mirrors compose.hlsl.
struct ComposeConstants {
    uint32_t output_width;
    uint32_t render_xpos;
    uint32_t bldcnt;
    uint32_t master_bright;
    uint32_t eva;
    uint32_t evb;
    uint32_t evy;
    uint32_t priority_3d;
    uint32_t order_3d;
    uint32_t output_height;
    uint32_t backdrop;
    uint32_t backdrop_meta;
    uint32_t output_view_width;
};

// The HD surfaces (NdsGpu2dHdFrame), both in one RGBA32UI texture: top
// pair in xy, below pair in zw. Plume maps no RG32UI format on Vulkan.
constexpr RenderFormat kHdFormat = RenderFormat::R32G32B32A32_UINT;
constexpr uint32_t kHdRowPitch = kMaxWidth * 16u;   // 7168, a multiple of 256
constexpr uint64_t kHdImageBytes = uint64_t(kHdRowPitch) * kHeight;
// An overlay finer than the screen (text drawn at window density) has its
// own staging, made on first use.
constexpr uint32_t kOverlayMaxScale = 4;
constexpr uint32_t kOverlayRowPitch = kMaxWidth * kOverlayMaxScale * 4u;  // 7168
constexpr uint64_t kOverlayBytes =
    uint64_t(kOverlayRowPitch) * kHeight * kOverlayMaxScale;

// The images one present uploads: the shown LCD, the other one as the
// single-screen inset, the frontend overlay (stylus, notices, FPS), and an
// optional layer image drawn into a sub-rect of the main one.
enum Image : uint32_t { kMain, kInset, kOverlay, kLayer, kImageCount };

struct ImageSlot {
    std::unique_ptr<RenderTexture> texture;
    std::unique_ptr<RenderDescriptorSet> set;
    int width = 0;
    int height = 0;
};

// The offscreen HD composite, when it is not drawn straight into the rect:
// at the 3D raster's size, then resampled into the rect like any image.
struct ComposeTarget {
    std::unique_ptr<RenderTexture> texture;
    std::unique_ptr<RenderFramebuffer> framebuffer;
    std::unique_ptr<RenderDescriptorSet> set;   // for the blit that reads it
    uint32_t width = 0, height = 0;
};

struct FrameSlot {
    std::unique_ptr<RenderCommandList> commands;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderCommandSemaphore> acquire;
    // kImageCount images of kImageBytes each, at fixed offsets.
    std::unique_ptr<RenderBuffer> staging;
    ImageSlot images[kImageCount];
    // HD: the two layer surfaces, their staging, and the compose inputs.
    std::unique_ptr<RenderBuffer> hd_staging;
    std::unique_ptr<RenderTexture> hd_layers;
    int hd_width = 0;
    std::unique_ptr<RenderDescriptorSet> compose_set;
    ComposeTarget compose;
    // A fine overlay (g_overlay_height != kHeight): kOverlayBytes.
    std::unique_ptr<RenderBuffer> overlay_staging;
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
    std::unique_ptr<RenderPipeline> premultiplied_pipeline;
    std::unique_ptr<RenderPipelineLayout> compose_layout;
    std::unique_ptr<RenderShader> compose_shader;
    std::unique_ptr<RenderPipeline> compose_pipeline;
    std::unique_ptr<RenderSampler> nearest;
    std::unique_ptr<RenderSampler> linear;
    FrameSlot slots[kFramesInFlight];
    uint32_t slot = 0;
    const char* api = "";
};

Backend g_backend;
SDL_Window* g_window = nullptr;
#if defined(__APPLE__)
// Owns the CAMetalLayer the MoltenVK surface draws into; outlives the swap
// chain and goes with release_backend.
SDL_MetalView g_metal_view = nullptr;
#endif
bool g_active = false;
bool g_visible = false;
bool g_resize_pending = false;
// A resize failed for a reason other than a zero-sized window; logged once
// per failure, and again when it recovers.
bool g_resize_failing = false;
#if defined(__ANDROID__)
// Android takes the window's surface away whenever the app leaves the
// foreground, and the window gets a new ANativeWindow when it comes back.
// Between the two the old one is released and SDL's window property still
// points at it, so no swap chain may be made until the foreground event
// says the new one is in place.
bool g_backgrounded = false;
bool g_lifecycle_watch = false;
#endif
NdsWindowMode g_window_mode = NdsWindowMode::Letterbox;
NdsScalingFilter g_scaling = NdsScalingFilter::Integer;
const uint32_t* g_overlay_pixels = nullptr;
unsigned g_overlay_width = 0;
unsigned g_overlay_height = 0;
// nds_compute_host_set_layer: the layer (native 256 wide), the image
// without it (g_layer_width wide), and where the layer goes, as fractions
// of the main rect's native middle.
const uint32_t* g_layer_pixels = nullptr;
const uint32_t* g_layer_base = nullptr;
unsigned g_layer_width = 0;
float g_layer_rect[4] = {};
const uint32_t* g_inset_pixels = nullptr;
unsigned g_inset_width = 0;
int g_inset_percent = 0;
uint64_t g_presented = 0;
uint64_t g_dropped = 0;
// [display] resolution_scale (percent of the presented rect, 0 = fixed) and
// internal_resolution (the fixed multiplier): what the 3D raster is sized by.
unsigned g_raster_percent = 100;
unsigned g_raster_fixed = 1;
std::string g_cache_directory;
NdsPlumeContext g_context;
bool g_logged_hd = false;

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

void create_framebuffers_for(
    RenderSwapChain& chain,
    std::vector<std::unique_ptr<RenderFramebuffer>>& framebuffers,
    std::vector<std::unique_ptr<RenderCommandSemaphore>>& release_semaphores) {
    framebuffers.clear();
    for (uint32_t i = 0; i < chain.getTextureCount(); ++i) {
        const RenderTexture* color = chain.getTexture(i);
        RenderFramebufferDesc desc;
        desc.colorAttachments = &color;
        desc.colorAttachmentsCount = 1;
        framebuffers.push_back(g_backend.device->createFramebuffer(desc));
    }
    // One release semaphore per swap chain image: present waits on the one
    // its image's submission signalled.
    while (release_semaphores.size() < chain.getTextureCount())
        release_semaphores.push_back(g_backend.device->createCommandSemaphore());
}

void create_framebuffers() {
    create_framebuffers_for(*g_backend.swap_chain, g_backend.framebuffers,
                            g_backend.release_semaphores);
}

// A minimised window has a 0x0 client area: nothing to draw into, and a
// swap chain cannot take that size. Checked before the swap chain is
// touched, so it keeps its images and a restore at the same size needs no
// resize at all. Hidden windows (NDS_FRONTEND_HIDDEN checks) keep their size
// and still present.
bool window_presentable() {
    if (!g_window) return false;
    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) return false;
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(g_window, &width, &height)) return true;
    return width > 0 && height > 0;
}

// Every frame is drawn at the window's current size: apply a pending or
// required resize before acquiring. A resize replaces the swap chain images,
// which a frame still in flight may be drawing into, so it drains the queue.
// False when there is nothing to present into this frame.
bool resize_if_needed() {
    if (!window_presentable()) return false;
    RenderSwapChain& chain = *g_backend.swap_chain;
    if (!g_resize_pending && !chain.needsResize()) return true;
    g_resize_pending = false;
    wait_all_slots();
    g_backend.framebuffers.clear();
    if (!chain.resize()) {
        // Retried next frame. resize() reads the window size first, and a
        // zero one (minimised between SDL's state and the OS's) is no error.
        g_resize_pending = true;
        const bool zero_sized = chain.getWidth() == 0 || chain.getHeight() == 0;
        if (!zero_sized && !g_resize_failing) {
            g_resize_failing = true;
            std::fprintf(stderr, "[plume] swap chain resize to %ux%u failed "
                                 "(0x%llx); retrying every frame\n",
                         chain.getWidth(), chain.getHeight(),
                         static_cast<unsigned long long>(chain.getLastError()));
        }
        return false;
    }
    if (g_resize_failing) {
        g_resize_failing = false;
        std::fprintf(stderr, "[plume] swap chain resize recovered (%ux%u)\n",
                     chain.getWidth(), chain.getHeight());
    }
    if (chain.isEmpty()) return false;
    create_framebuffers();
    return !g_backend.framebuffers.empty();
}

std::unique_ptr<RenderShader> create_shader(const void* spirv, size_t spirv_size,
                                            const void* dxil, size_t dxil_size,
                                            const char* entry) {
    return nds_plume_create_shader(spirv, spirv_size, dxil, dxil_size, entry);
}

RenderDescriptorSetDesc compose_set_desc() {
    static const RenderDescriptorRange ranges[] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
    };
    return RenderDescriptorSetDesc(ranges, 2);
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
    const void* compose_dxil = ndsComposeFragBlobDXIL;
    const size_t compose_dxil_size = sizeof(ndsComposeFragBlobDXIL);
#else
    const void* vert_dxil = nullptr;
    const size_t vert_dxil_size = 0;
    const void* blit_dxil = nullptr;
    const size_t blit_dxil_size = 0;
    const void* compose_dxil = nullptr;
    const size_t compose_dxil_size = 0;
#endif
    g_backend.vertex_shader = create_shader(
        ndsFullscreenVertBlobSPIRV, sizeof(ndsFullscreenVertBlobSPIRV),
        vert_dxil, vert_dxil_size, "VSMain");
    g_backend.blit_shader = create_shader(
        ndsBlitFragBlobSPIRV, sizeof(ndsBlitFragBlobSPIRV),
        blit_dxil, blit_dxil_size, "PSMain");
    g_backend.compose_shader = create_shader(
        ndsComposeFragBlobSPIRV, sizeof(ndsComposeFragBlobSPIRV),
        compose_dxil, compose_dxil_size, "PSMain");
    if (!g_backend.vertex_shader || !g_backend.blit_shader ||
        !g_backend.compose_shader) {
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
    // The fixed-size layer is premultiplied (nds_compute_host_set_layer).
    desc.renderTargetBlend[0].srcBlend = RenderBlend::ONE;
    g_backend.premultiplied_pipeline =
        g_backend.device->createGraphicsPipeline(desc);
    if (!g_backend.opaque_pipeline || !g_backend.blended_pipeline ||
        !g_backend.premultiplied_pipeline)
        return false;

    // The HD composite writes opaque swap-chain-format pixels, into the rect
    // or into an offscreen target of the same format.
    const RenderDescriptorSetDesc compose_desc = compose_set_desc();
    const RenderPushConstantRange compose_constants(
        0, 0, 0, sizeof(ComposeConstants), RenderShaderStageFlag::PIXEL);
    RenderPipelineLayoutDesc compose_layout_desc;
    compose_layout_desc.pushConstantRanges = &compose_constants;
    compose_layout_desc.pushConstantRangesCount = 1;
    compose_layout_desc.descriptorSetDescs = &compose_desc;
    compose_layout_desc.descriptorSetDescsCount = 1;
    g_backend.compose_layout = g_backend.device->createPipelineLayout(compose_layout_desc);
    if (!g_backend.compose_layout) return false;
    desc.pipelineLayout = g_backend.compose_layout.get();
    desc.pixelShader = g_backend.compose_shader.get();
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    g_backend.compose_pipeline = g_backend.device->createGraphicsPipeline(desc);
    if (!g_backend.compose_pipeline) return false;

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
        slot.hd_staging = g_backend.device->createBuffer(
            RenderBufferDesc::UploadBuffer(kHdImageBytes));
        slot.compose_set = g_backend.device->createDescriptorSet(compose_set_desc());
        slot.compose.set = g_backend.device->createDescriptorSet(image_set_desc());
        if (!slot.hd_staging || !slot.compose_set || !slot.compose.set) return false;
    }
    return true;
}

RenderWindow render_window_for(SDL_Window* window) {
#if defined(_WIN32)
    return static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr));
#elif defined(__ANDROID__)
    // Plume's Vulkan backend makes a VK_KHR_android_surface from it.
    return static_cast<ANativeWindow*>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
        nullptr));
#elif defined(PLUME_SDL3_VULKAN_ENABLED)
    return window;
#elif defined(__APPLE__)
    // Plume's Vulkan backend makes a VK_EXT_metal_surface from the layer and
    // reads the drawable size from the NSWindow.
    if (!g_metal_view) g_metal_view = SDL_Metal_CreateView(window);
    void* const ns_window = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        nullptr);
    void* const layer = g_metal_view ? SDL_Metal_GetLayer(g_metal_view) : nullptr;
    return RenderWindow{ns_window, layer};
#else
    (void)window;
    return RenderWindow{};
#endif
}

bool render_window_valid(const RenderWindow& window) {
#if defined(_WIN32) || defined(__ANDROID__) || defined(PLUME_SDL3_VULKAN_ENABLED)
    return window != nullptr;
#elif defined(__APPLE__)
    return window.window != nullptr && window.view != nullptr;
#else
    (void)window;
    return false;
#endif
}

#if defined(__APPLE__)
// volk looks for the Vulkan loader or MoltenVK under bare names (found
// through DYLD_* paths, /usr/local/lib and /usr/lib) and as frameworks. That
// misses a MoltenVK shipped beside the runner or in its app bundle, and
// Homebrew on Apple Silicon (/opt/homebrew/lib). Point Plume at the first of
// those that loads, for a retry after volk's own search came up empty.
bool use_fallback_vulkan_library() {
    std::vector<std::string> candidates;
    if (const char* base = SDL_GetBasePath()) {
        candidates.push_back(std::string(base) + "libMoltenVK.dylib");
        candidates.push_back(std::string(base) + "../Frameworks/libMoltenVK.dylib");
    }
    candidates.push_back("/opt/homebrew/lib/libvulkan.1.dylib");
    candidates.push_back("/opt/homebrew/lib/libMoltenVK.dylib");
    candidates.push_back("/usr/local/lib/libMoltenVK.dylib");
    for (const std::string& path : candidates) {
        void* const library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library) continue;
        void* const get_instance_proc_addr = dlsym(library, "vkGetInstanceProcAddr");
        if (!get_instance_proc_addr) {
            dlclose(library);
            continue;
        }
        // Stays loaded for the life of the process, as volk's own would.
        std::fprintf(stderr, "[plume] Vulkan from %s\n", path.c_str());
        SetVulkanGetInstanceProcAddr(get_instance_proc_addr);
        return true;
    }
    return false;
}
#endif

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
#elif defined(__ANDROID__) || defined(PLUME_SDL3_VULKAN_ENABLED)
    g_backend.render_interface = CreateVulkanInterface();
    g_backend.api = "Vulkan";
    return g_backend.render_interface != nullptr;
#elif defined(__APPLE__)
    g_backend.render_interface = CreateVulkanInterface();
    if (!g_backend.render_interface && use_fallback_vulkan_library())
        g_backend.render_interface = CreateVulkanInterface();
    if (!g_backend.render_interface)
        std::fprintf(stderr, "[plume] no Vulkan driver: install MoltenVK "
                             "(Vulkan SDK or `brew install molten-vk`) or put "
                             "libMoltenVK.dylib beside the runner\n");
    g_backend.api = "Vulkan";
    return g_backend.render_interface != nullptr;
#else
    return false;
#endif
}

bool create_swap_chain(const RenderWindow& render_window) {
    g_backend.swap_chain = g_backend.queue->createSwapChain(RenderSwapChainDesc(
        render_window, kSwapChainFormat, kSwapChainBuffers));
    if (!g_backend.swap_chain) return false;
    // Vsync is always on, with no setting to turn it off. The frontend paces
    // itself from the audio queue, so Vulkan prefers MAILBOX: the flip lands
    // on a vblank (no tearing) but a present never blocks for a whole
    // refresh. Without MAILBOX it falls back to FIFO.
    g_backend.swap_chain->setVsyncEnabled(true);
    return true;
}

#if defined(__ANDROID__)
// The swap chain owns the VkSurfaceKHR, so dropping it lets go of the old
// ANativeWindow. A frame in flight may still draw into its images.
void release_swap_chain() {
    if (!g_backend.swap_chain) return;
    wait_all_slots();
    g_backend.framebuffers.clear();
    g_backend.swap_chain.reset();
    g_resize_pending = false;
    g_resize_failing = false;
}

// After a return to the foreground: a new swap chain on the window's new
// surface, sized by the next resize_if_needed. False while there is still
// nothing to make one on; the frame is skipped and the next one retries.
bool rebuild_swap_chain() {
    if (g_backgrounded || !g_window) return false;
    const RenderWindow render_window = render_window_for(g_window);
    if (!render_window_valid(render_window)) return false;
    if (!create_swap_chain(render_window)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr, "[plume] no swap chain on the new surface; "
                                 "retrying every frame\n");
        }
        return false;
    }
    g_resize_pending = true;
    std::fprintf(stderr, "[plume] swap chain rebuilt on the new surface\n");
    return true;
}

// SDL runs the lifecycle on the thread that pumps events, which is the one
// that presents, so this never races a frame. The background events arrive
// just before the pump starts blocking until the app is resumed, and SDL
// resumes only once the new surface exists.
bool SDLCALL lifecycle_watch(void*, SDL_Event* event) {
    switch (event->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            if (!g_backgrounded) {
                g_backgrounded = true;
                release_swap_chain();
                std::fprintf(stderr, "[plume] surface lost (background)\n");
            }
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            g_backgrounded = false;
            break;
        default:
            break;
    }
    return true;
}

// The bottom LCD on a second display (nds_compute_host_set_bottom_window):
// its own swap chain and frame slots on the same device and queue, recorded
// and submitted on the same thread as the top window's frames.
struct BottomTarget {
    ANativeWindow* window = nullptr;
    std::unique_ptr<RenderSwapChain> swap_chain;
    std::vector<std::unique_ptr<RenderFramebuffer>> framebuffers;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> release_semaphores;
    FrameSlot slots[kFramesInFlight];
    bool slots_ready = false;
    uint32_t slot = 0;
    bool resize_pending = false;
    bool resize_failing = false;
    uint64_t presented = 0;
    uint64_t dropped = 0;
    // Present timing over the current log window: answers whether this
    // panel's present ever blocks the frame loop.
    uint64_t window_ticks = 0;
    uint64_t window_max_ticks = 0;
};
BottomTarget g_bottom;

void wait_bottom_slots() {
    for (FrameSlot& slot : g_bottom.slots) wait_slot(slot);
}

// Lets go of the bottom swap chain (and with it the VkSurfaceKHR on the
// surface) once nothing in flight draws into it.
void release_bottom_chain() {
    if (g_backend.queue) wait_bottom_slots();
    g_bottom.framebuffers.clear();
    if (g_bottom.swap_chain)
        std::fprintf(stderr, "[plume] bottom swap chain released (%llu frames, "
                             "%llu dropped)\n",
                     static_cast<unsigned long long>(g_bottom.presented),
                     static_cast<unsigned long long>(g_bottom.dropped));
    g_bottom.swap_chain.reset();
    g_bottom.window = nullptr;
    g_bottom.slot = 0;
    g_bottom.resize_pending = false;
    g_bottom.resize_failing = false;
    g_bottom.presented = g_bottom.dropped = 0;
    g_bottom.window_ticks = g_bottom.window_max_ticks = 0;
}

void release_bottom() {
    release_bottom_chain();
    for (FrameSlot& slot : g_bottom.slots) slot = FrameSlot{};
    g_bottom.release_semaphores.clear();
    g_bottom.slots_ready = false;
}
#endif

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
    const RenderWindow render_window = render_window_for(window);
    if (!render_window_valid(render_window)) {
        std::fprintf(stderr, "[plume] the presentation window has no native "
                             "handle: %s\n", SDL_GetError());
        return false;
    }
    if (!create_swap_chain(render_window)) {
        std::fprintf(stderr, "[plume] no swap chain on the presentation window\n");
        return false;
    }
    // A swap chain has no images until its first resize. A window that
    // starts minimised has no size to give it yet: the first frame presented
    // after the restore resizes it.
    if (!g_backend.swap_chain->resize()) {
        const bool zero_sized = g_backend.swap_chain->getWidth() == 0 ||
                                g_backend.swap_chain->getHeight() == 0;
        if (!zero_sized) {
            std::fprintf(stderr, "[plume] initial swap chain resize failed\n");
            return false;
        }
        g_resize_pending = true;
    }
    if (!g_backend.swap_chain->isEmpty()) create_framebuffers();
#if defined(__ANDROID__)
    if (!g_lifecycle_watch) g_lifecycle_watch = SDL_AddEventWatch(lifecycle_watch, nullptr);
    g_backgrounded = false;
#endif
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
#if defined(__ANDROID__)
    release_bottom();
    if (g_lifecycle_watch) SDL_RemoveEventWatch(lifecycle_watch, nullptr);
    g_lifecycle_watch = false;
    g_backgrounded = false;
#endif
    if (g_backend.queue) wait_all_slots();
    nds_plume3d_stop();
    g_context = NdsPlumeContext{};
    for (FrameSlot& slot : g_backend.slots) slot = FrameSlot{};
    g_backend.framebuffers.clear();
    g_backend.release_semaphores.clear();
    g_backend.swap_chain.reset();
    g_backend.blended_pipeline.reset();
    g_backend.premultiplied_pipeline.reset();
    g_backend.opaque_pipeline.reset();
    g_backend.compose_pipeline.reset();
    g_backend.compose_shader.reset();
    g_backend.compose_layout.reset();
    g_backend.blit_shader.reset();
    g_backend.vertex_shader.reset();
    g_backend.layout.reset();
    g_backend.nearest.reset();
    g_backend.linear.reset();
    g_backend.queue.reset();
    g_backend.device.reset();
    g_backend.render_interface.reset();
#if defined(__APPLE__)
    if (g_metal_view) SDL_Metal_DestroyView(g_metal_view);
    g_metal_view = nullptr;
#endif
    g_backend.slot = 0;
    g_backend.api = "";
    g_window = nullptr;
    g_active = false;
    g_visible = false;
    g_resize_pending = false;
    g_resize_failing = false;
}

// Copies `width` x 192 ARGB rows into the slot's staging area for `image`,
// and (re)creates the slot's texture if the width changed. Safe without a
// wait: this slot's previous frame has retired, and the other slot has
// textures of its own.
bool stage_image(FrameSlot& slot, uint8_t* staging, Image image,
                 const uint32_t* pixels, int width) {
    ImageSlot& target = slot.images[image];
    if (target.width != width || target.height != int(kHeight) ||
        !target.texture) {
        target.texture = g_backend.device->createTexture(
            RenderTextureDesc::Texture2D(uint32_t(width), kHeight, 1, kImageFormat));
        if (!target.texture) return false;
        target.width = width;
        target.height = int(kHeight);
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

// A fine overlay (any size up to kOverlayMaxScale times the screen), into
// its own staging.
bool stage_fine_overlay(FrameSlot& slot, const uint32_t* pixels,
                        unsigned overlay_width, unsigned overlay_height) {
    ImageSlot& target = slot.images[kOverlay];
    const int width = int(overlay_width), height = int(overlay_height);
    if (!slot.overlay_staging) {
        slot.overlay_staging = g_backend.device->createBuffer(
            RenderBufferDesc::UploadBuffer(kOverlayBytes));
        if (!slot.overlay_staging) return false;
    }
    if (target.width != width || target.height != height || !target.texture) {
        target.texture = g_backend.device->createTexture(
            RenderTextureDesc::Texture2D(uint32_t(width), uint32_t(height), 1,
                                         kImageFormat));
        if (!target.texture) return false;
        target.width = width;
        target.height = height;
        target.set->setTexture(0, target.texture.get(),
                               RenderTextureLayout::SHADER_READ);
    }
    auto* dst = static_cast<uint8_t*>(slot.overlay_staging->map());
    if (!dst) return false;
    for (int y = 0; y < height; ++y)
        std::memcpy(dst + size_t(y) * kOverlayRowPitch,
                    pixels + size_t(y) * width, size_t(width) * 4u);
    slot.overlay_staging->unmap();
    return true;
}

void record_fine_overlay_upload(FrameSlot& slot) {
    RenderCommandList& commands = *slot.commands;
    const ImageSlot& image = slot.images[kOverlay];
    RenderTexture* texture = image.texture.get();
    commands.barriers(RenderBarrierStage::COPY,
                      RenderTextureBarrier(texture, RenderTextureLayout::COPY_DEST));
    commands.copyTextureRegion(
        RenderTextureCopyLocation::Subresource(texture),
        RenderTextureCopyLocation::PlacedFootprint(
            slot.overlay_staging.get(), kImageFormat, uint32_t(image.width),
            uint32_t(image.height), 1, kOverlayRowPitch / 4u, 0));
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
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
// The part of `rect` inside the framebuffer, as a scissor; false if none.
bool clip_rect(const NdsPresentRect& rect, uint32_t fb_width, uint32_t fb_height,
               RenderRect* out) {
    if (rect.w <= 0 || rect.h <= 0) return false;
    const int32_t left = std::max(rect.x, 0);
    const int32_t top = std::max(rect.y, 0);
    const int32_t right = std::min<int32_t>(rect.x + rect.w, int32_t(fb_width));
    const int32_t bottom = std::min<int32_t>(rect.y + rect.h, int32_t(fb_height));
    if (right <= left || bottom <= top) return false;
    *out = RenderRect(left, top, right, bottom);
    return true;
}

// `clip`, when given, cuts the image drawn into `rect` to that rect.
void record_blit(FrameSlot& slot, RenderDescriptorSet* set, const NdsPresentRect& rect,
                 bool linear, uint32_t flags, uint32_t fb_width,
                 uint32_t fb_height, const NdsPresentRect* clip = nullptr) {
    RenderRect scissor;
    if (!clip_rect(rect, fb_width, fb_height, &scissor)) return;
    if (clip) {
        RenderRect cut;
        if (!clip_rect(*clip, fb_width, fb_height, &cut)) return;
        scissor.left = std::max(scissor.left, cut.left);
        scissor.top = std::max(scissor.top, cut.top);
        scissor.right = std::min(scissor.right, cut.right);
        scissor.bottom = std::min(scissor.bottom, cut.bottom);
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top)
            return;
    }
    RenderCommandList& commands = *slot.commands;
    commands.setGraphicsPipelineLayout(g_backend.layout.get());
    set->setSampler(1, linear ? g_backend.linear.get()
                              : g_backend.nearest.get());
    commands.setPipeline(
        (flags & kBlitKeepAlpha) == 0u    ? g_backend.opaque_pipeline.get()
        : (flags & kBlitPremultiplied) != 0u
            ? g_backend.premultiplied_pipeline.get()
            : g_backend.blended_pipeline.get());
    commands.setGraphicsDescriptorSet(set, 0);
    const BlitConstants constants{flags, 0u, {rect.w, rect.h}};
    commands.setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
    commands.setViewports(RenderViewport(float(rect.x), float(rect.y),
                                         float(rect.w), float(rect.h)));
    commands.setScissors(scissor);
    commands.drawInstanced(3, 1, 0, 0);
}

void record_blit(FrameSlot& slot, Image image, const NdsPresentRect& rect,
                 bool linear, uint32_t flags, uint32_t fb_width,
                 uint32_t fb_height, const NdsPresentRect* clip = nullptr) {
    record_blit(slot, slot.images[image].set.get(), rect, linear, flags,
                fb_width, fb_height, clip);
}

// Interleaves the two HD layer surfaces into this slot's staging texture.
bool stage_hd(FrameSlot& slot, const NdsGpu2dHdFrame& hd) {
    const int width = hd.width;
    if (slot.hd_width != width || !slot.hd_layers) {
        slot.hd_layers = g_backend.device->createTexture(
            RenderTextureDesc::Texture2D(uint32_t(width), kHeight, 1, kHdFormat));
        if (!slot.hd_layers) {
            std::fprintf(stderr, "[plume] HD surface creation failed (%dx%u)\n",
                         width, kHeight);
            slot.hd_width = 0;
            return false;
        }
        slot.hd_width = width;
    }
    auto* staging = static_cast<uint8_t*>(slot.hd_staging->map());
    if (!staging) return false;
    for (uint32_t y = 0; y < kHeight; ++y) {
        auto* dst = reinterpret_cast<uint32_t*>(staging + size_t(y) * kHdRowPitch);
        const uint32_t* top = hd.top_pixels + size_t(y) * width * 2u;
        const uint32_t* below = hd.below_pixels + size_t(y) * width * 2u;
        for (int x = 0; x < width; ++x) {
            dst[4 * x + 0] = top[2 * x];
            dst[4 * x + 1] = top[2 * x + 1];
            dst[4 * x + 2] = below[2 * x];
            dst[4 * x + 3] = below[2 * x + 1];
        }
    }
    slot.hd_staging->unmap();
    return true;
}

void record_hd_upload(FrameSlot& slot) {
    RenderCommandList& commands = *slot.commands;
    RenderTexture* texture = slot.hd_layers.get();
    commands.barriers(RenderBarrierStage::COPY,
                      RenderTextureBarrier(texture, RenderTextureLayout::COPY_DEST));
    commands.copyTextureRegion(
        RenderTextureCopyLocation::Subresource(texture),
        RenderTextureCopyLocation::PlacedFootprint(
            slot.hd_staging.get(), kHdFormat, uint32_t(slot.hd_width), kHeight, 1,
            kHdRowPitch / 16u, 0));
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(texture, RenderTextureLayout::SHADER_READ));
}

bool ensure_compose_target(FrameSlot& slot, uint32_t width, uint32_t height) {
    ComposeTarget& target = slot.compose;
    if (target.texture && target.width == width && target.height == height) return true;
    target.framebuffer.reset();
    target.texture = g_backend.device->createTexture(
        RenderTextureDesc::ColorTarget(width, height, kSwapChainFormat));
    if (!target.texture) {
        std::fprintf(stderr, "[plume] compose target creation failed (%ux%u)\n",
                     width, height);
        target.width = target.height = 0;
        return false;
    }
    const RenderTexture* color = target.texture.get();
    RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    target.framebuffer = g_backend.device->createFramebuffer(desc);
    if (!target.framebuffer) {
        target.texture.reset();
        target.width = target.height = 0;
        return false;
    }
    target.width = width;
    target.height = height;
    return true;
}

// The HD compose draw under whatever framebuffer, viewport and scissor are
// set: the output pixel grid is the viewport, whatever its size.
void record_compose(FrameSlot& slot, RenderTexture* raster, const NdsGpu2dHdFrame& hd,
                    int width) {
    RenderCommandList& commands = *slot.commands;
    slot.compose_set->setTexture(0, raster, RenderTextureLayout::SHADER_READ);
    slot.compose_set->setTexture(1, slot.hd_layers.get(), RenderTextureLayout::SHADER_READ);
    commands.setGraphicsPipelineLayout(g_backend.compose_layout.get());
    commands.setPipeline(g_backend.compose_pipeline.get());
    commands.setGraphicsDescriptorSet(slot.compose_set.get(), 0);
    ComposeConstants constants{};
    constants.output_width = uint32_t(width);
    constants.render_xpos = hd.render_xpos;
    constants.bldcnt = hd.bldcnt;
    constants.master_bright = hd.master_bright;
    constants.eva = hd.eva;
    constants.evb = hd.evb;
    constants.evy = hd.evy;
    constants.priority_3d = hd.priority_3d;
    constants.order_3d = hd.order_3d;
    constants.output_height = hd.height;
    constants.output_view_width = hd.view_width;
    constants.backdrop = hd.backdrop;
    constants.backdrop_meta = hd.backdrop_meta;
    commands.setGraphicsPushConstants(0, &constants, 0, sizeof(constants));
    commands.drawInstanced(3, 1, 0, 0);
}

// The 3D raster for a presented rect: resolution_scale percent of the rect
// itself, any size, or the fixed internal_resolution multiple of native.
void request_raster(const NdsPresentRect& rect, int width, int height) {
    uint32_t raster_w, raster_h;
    if (g_raster_percent > 0) {
        raster_w = uint32_t((uint64_t(std::max(rect.w, 1)) * g_raster_percent + 50) / 100);
        raster_h = uint32_t((uint64_t(std::max(rect.h, 1)) * g_raster_percent + 50) / 100);
    } else {
        raster_w = uint32_t(width) * g_raster_fixed;
        raster_h = uint32_t(height) * g_raster_fixed;
    }
    // Outlines are one presented pixel wide: as many raster pixels as the
    // raster has per pixel of the rect. A DS pixel scaled up with the 3D is
    // too wide for content the title draws below DS scale.
    const uint32_t rect_h = uint32_t(std::max(rect.h, 1));
    const uint32_t edge = std::max(1u, (raster_h + rect_h / 2) / rect_h);
    nds_plume3d_request_size(raster_w, raster_h, edge);
}
}  // namespace

const NdsPlumeContext* nds_plume_context()
{
    return g_context.device ? &g_context : nullptr;
}

void nds_plume_host_wait_idle()
{
    if (!g_backend.queue) return;
    wait_all_slots();
#if defined(__ANDROID__)
    wait_bottom_slots();
#endif
}

std::unique_ptr<RenderShader> nds_plume_create_shader(
    const void* spirv, size_t spirv_size, const void* dxil, size_t dxil_size,
    const char* entry)
{
    if (!g_backend.render_interface || !g_backend.device) return nullptr;
    const RenderShaderFormat format =
        g_backend.render_interface->getCapabilities().shaderFormat;
    if (format == RenderShaderFormat::SPIRV)
        return g_backend.device->createShader(spirv, spirv_size, entry, format);
    if (format == RenderShaderFormat::DXIL && dxil)
        return g_backend.device->createShader(dxil, dxil_size, entry, format);
    return nullptr;
}

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
        g_context.render_interface = g_backend.render_interface.get();
        g_context.device = g_backend.device.get();
        g_context.queue = g_backend.queue.get();
        // Without it the host still presents; above 1x it just has no HD
        // raster and keeps blitting the CPU composite.
        if (!nds_plume3d_start(g_cache_directory.empty()
                                   ? std::string()
                                   : g_cache_directory + "/plume-pipelines.bin"))
            std::fprintf(stderr, "[plume] 3D renderer unavailable; presenting "
                                 "the CPU composite only\n");
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
    // The HD path composes from the Plume raster. At 1x the CPU composite
    // still carries the 3D layer, from the soft renderer.
    return g_active && g_visible && nds_plume3d_ready();
}

bool nds_compute_host_presents_direct()
{
    // The OBJ-only direct path needs a native 3D surface on the GPU, which
    // this host never has: native 3D is the soft renderer's, on the CPU.
    return false;
}

void nds_compute_host_set_cache_directory(const char* directory)
{
    g_cache_directory = directory ? directory : "";
}

void nds_compute_host_set_raster_request(unsigned percent, unsigned fixed_scale)
{
    g_raster_percent = percent;
    g_raster_fixed = std::max(fixed_scale, 1u);
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

void nds_compute_host_set_overlay(const uint32_t* argb, unsigned width,
                                  unsigned height)
{
    const bool ok = width > 0 && height > 0 &&
                    width <= kMaxWidth * kOverlayMaxScale &&
                    height <= kHeight * kOverlayMaxScale;
    g_overlay_pixels = ok ? argb : nullptr;
    g_overlay_width = ok ? width : 0;
    g_overlay_height = ok ? height : 0;
}

unsigned nds_compute_host_overlay_max_scale()
{
    return kOverlayMaxScale;
}

bool nds_compute_host_supports_layer()
{
    return true;
}

void nds_compute_host_set_layer(const uint32_t* layer, const uint32_t* base,
                                unsigned width, float x, float y, float w,
                                float h)
{
    const bool shown = layer && base && width > 0 && width <= kMaxWidth &&
                       w > 0.0f && h > 0.0f;
    g_layer_pixels = shown ? layer : nullptr;
    g_layer_base = shown ? base : nullptr;
    g_layer_width = shown ? width : 0;
    g_layer_rect[0] = x;
    g_layer_rect[1] = y;
    g_layer_rect[2] = w;
    g_layer_rect[3] = h;
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

void nds_compute_host_set_fast_forward(bool enabled)
{
    // D3D12 presents with a sync interval of 1, which blocks for a whole
    // refresh, so fast-forward could not pass the display rate. Vulkan's
    // MAILBOX never blocks, and changing its mode would rebuild the swap
    // chain, so it is left alone.
    if (!g_backend.swap_chain || std::strcmp(g_backend.api, "D3D12") != 0)
        return;
    g_backend.swap_chain->setVsyncEnabled(!enabled);
}

bool nds_compute_host_present_top(const unsigned int* fallback_pixels,
                                  unsigned short fallback_width,
                                  const NdsGpu2dDirectFrame* direct_frame,
                                  NdsComputePresentTicks* ticks,
                                  unsigned antialiasing,
                                  const NdsComputePresentationFrame* owned)
{
    // No direct frames are produced for this host (presents_direct is false).
    // Antialiasing is the GL presenter's extra taps into an integer raster;
    // here resolution_scale above 100% renders the denser raster itself and
    // the box resolve integrates it.
    (void)direct_frame;
    (void)antialiasing;
    if (!g_active || !g_visible || !ticks || !fallback_pixels) return false;
    const int width = fallback_width;
    if (width <= 0 || width > int(kMaxWidth)) return false;
    const auto diagnostic_begin = ticks->diagnostic ? SDL_GetPerformanceCounter() : 0;

    // Minimised (zero-sized surface) or a resize still failing: nothing to
    // present to, and not an error. The frame is skipped, not dropped. So is
    // one on Android with no surface yet after a return to the foreground.
#if defined(__ANDROID__)
    if (!g_backend.swap_chain && !rebuild_swap_chain()) return true;
#endif
    if (!resize_if_needed()) return true;
    RenderSwapChain& chain = *g_backend.swap_chain;
    if (chain.isEmpty() || g_backend.framebuffers.empty()) return true;
    const uint32_t fb_width = chain.getWidth();
    const uint32_t fb_height = chain.getHeight();
    // A taller view (Ver+) is that many lines; its 2D the middle 192.
    const int view_h = std::max(int(kHeight),
        nds_main_view_height_flag().load(std::memory_order_relaxed));
    // A view wider than the image (its 3D lines full) is that many columns,
    // the image its middle ones.
    const int view_w = std::max(width,
        nds_main_view_width_flag().load(std::memory_order_relaxed));
    const NdsSingleLayout layout = nds_single_layout(
        g_window_mode, g_scaling, int(fb_width), int(fb_height), view_w,
        view_h, g_inset_percent, nds_stack_vertically_flag().load());
    const NdsPresentRect& rect = layout.main;
    const NdsPresentRect band = nds_view_band(rect, view_w, width, view_h);
    // Size the next 3D frame by this rect: from here on 3D follows the
    // window pixel for pixel, whatever its size.
    request_raster(rect, view_w, view_h);

    // HD: the compositor's layer surfaces for this frame plus a Plume raster.
    NdsGpu2dHdFrame hd{};
    uint32_t raster_w = 0, raster_h = 0;
    RenderTexture* raster = nds_gpu3d_internal_scale() > 1u
        ? nds_plume3d_output(&raster_w, &raster_h) : nullptr;
    const bool have_hd = owned ? (hd = owned->hd, owned->has_hd)
                               : (raster && nds_gpu2d_hd_frame(&hd));
    const bool hd_active = raster && have_hd && hd.width == width &&
                           hd.top_pixels && hd.below_pixels;
    if (owned && hd_active) nds_gpu2d_note_owned_hd_presented();
    if (hd_active && !g_logged_hd) {
        g_logged_hd = true;
        std::fprintf(stderr, "[plume] HD compose: 3D raster %ux%u into a %dx%d rect\n",
                     raster_w, raster_h, rect.w, rect.h);
    }

    FrameSlot& slot = g_backend.slots[g_backend.slot];
    unsigned long long start = SDL_GetPerformanceCounter();
    if (ticks->diagnostic) ticks->setup = start - diagnostic_begin;
    auto* staging = static_cast<uint8_t*>(slot.staging->map());
    if (!staging) return false;
    const bool inset = g_inset_pixels != nullptr;
    const bool overlay = g_overlay_pixels != nullptr;
    const bool fine_overlay = overlay && (g_overlay_height != kHeight ||
                                          g_overlay_width > kMaxWidth);
    // The layer goes with an image of the same width that lacks it.
    const bool layer = g_layer_pixels != nullptr && int(g_layer_width) == width;
    const uint32_t* main_pixels = layer ? g_layer_base : fallback_pixels;
    bool staged = hd_active || stage_image(slot, staging, kMain, main_pixels, width);
    if (staged && layer)
        staged = stage_image(slot, staging, kLayer, g_layer_pixels, int(kNativeWidth));
    if (staged && inset)
        staged = stage_image(slot, staging, kInset, g_inset_pixels, int(g_inset_width));
    if (staged && overlay && !fine_overlay)
        staged = stage_image(slot, staging, kOverlay, g_overlay_pixels,
                             int(g_overlay_width));
    slot.staging->unmap();
    if (staged && fine_overlay)
        staged = stage_fine_overlay(slot, g_overlay_pixels, g_overlay_width,
                                    g_overlay_height);
    if (staged && hd_active) staged = stage_hd(slot, hd);
    if (!staged) return false;
    ticks->upload += SDL_GetPerformanceCounter() - start;

    // The HD composite is drawn at the raster's own density: straight into
    // the rect when the raster is exactly the rect's size (the default),
    // otherwise offscreen and then resampled like any image.
    const bool compose_direct =
        hd_active && int(raster_w) == rect.w && int(raster_h) == rect.h;
    const bool compose_offscreen = hd_active && !compose_direct &&
        ensure_compose_target(slot, raster_w, raster_h);
    if (hd_active && !compose_direct && !compose_offscreen) return false;

    // Acquired last: every failure above returns before an image is taken,
    // because an acquired image that is never presented stays out of the
    // swap chain, and once all of them are, acquire blocks for good.
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

    start = SDL_GetPerformanceCounter();
    RenderTexture* backbuffer = chain.getTexture(image_index);
    RenderCommandList& commands = *slot.commands;
    commands.begin();
    if (!hd_active) record_upload(slot, kMain);
    if (inset) record_upload(slot, kInset);
    if (overlay && !fine_overlay) record_upload(slot, kOverlay);
    if (fine_overlay) record_fine_overlay_upload(slot);
    if (layer) record_upload(slot, kLayer);
    if (hd_active) record_hd_upload(slot);
    if (compose_offscreen) {
        RenderTexture* target = slot.compose.texture.get();
        commands.barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
        commands.setFramebuffer(slot.compose.framebuffer.get());
        commands.setViewports(RenderViewport(0.0f, 0.0f, float(raster_w), float(raster_h)));
        commands.setScissors(RenderRect(0, 0, int32_t(raster_w), int32_t(raster_h)));
        record_compose(slot, raster, hd, width);
        commands.barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(target, RenderTextureLayout::SHADER_READ));
        slot.compose.set->setTexture(0, target, RenderTextureLayout::SHADER_READ);
    }
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
    commands.setFramebuffer(g_backend.framebuffers[image_index].get());
    commands.setViewports(RenderViewport(0.0f, 0.0f, float(fb_width), float(fb_height)));
    commands.setScissors(RenderRect(0, 0, int32_t(fb_width), int32_t(fb_height)));
    commands.clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

    const bool bilinear = g_scaling == NdsScalingFilter::Bilinear;
    if (compose_direct) {
        RenderRect scissor;
        if (clip_rect(rect, fb_width, fb_height, &scissor)) {
            commands.setViewports(RenderViewport(float(rect.x), float(rect.y),
                                                 float(rect.w), float(rect.h)));
            commands.setScissors(scissor);
            record_compose(slot, raster, hd, width);
        }
    } else if (compose_offscreen) {
        // A denser raster is box resolved (every raster texel whose centre
        // falls in the output pixel, any ratio); a sparser one is filtered.
        const bool denser = int(raster_w) > rect.w || int(raster_h) > rect.h;
        record_blit(slot, slot.compose.set.get(), rect, bilinear && !denser,
                    denser ? kBlitBoxResolve : 0u, fb_width, fb_height);
    } else {
        // The GL presenter's filter choice, at the CPU composite's own
        // density: nearest at the exact rect, bilinear resampled, and a box
        // resolve where integer (or nearest at 2x and up) would otherwise
        // skip source texels.
        // A taller view's CPU frame is its 2D band only (its 3D is the
        // squeezed one): drawn there, the rows around it left black.
        const bool supersampled = width > band.w || int(kHeight) > band.h;
        const bool box_resolve =
            (g_scaling == NdsScalingFilter::Integer && supersampled) ||
            (g_scaling == NdsScalingFilter::Nearest && width >= 2 * band.w &&
             int(kHeight) >= 2 * band.h);
        record_blit(slot, kMain, band, bilinear && !box_resolve,
                    box_resolve ? kBlitBoxResolve : 0u, fb_width, fb_height);
    }
    if (inset && layout.inset.w > 0 && layout.inset.h > 0) {
        // Point sampling a shrunk inset drops whole rows of text.
        const bool linear = bilinear || layout.inset.w < int(g_inset_width);
        // Over the main screen its alpha is how solid each pixel is.
        const bool over =
            nds_inset_overlay_flag().load(std::memory_order_relaxed) > 0;
        const NdsInsetPieces& pieces = nds_inset_pieces();
        if (over && pieces.count > 0) {
            // Each piece: the whole image placed so that the piece's part
            // lands on its rect, cut to that rect.
            for (int i = 0; i < pieces.count; ++i) {
                NdsPresentRect clip{}, view{};
                if (nds_inset_piece_rects(rect, view_w, view_h,
                                          int(g_inset_width), int(kHeight),
                                          pieces.piece[i], &clip, &view))
                    record_blit(slot, kInset, view,
                                bilinear || view.w < int(g_inset_width),
                                kBlitKeepAlpha, fb_width, fb_height, &clip);
            }
        } else {
            record_blit(slot, kInset, layout.inset, linear,
                        over ? kBlitKeepAlpha : 0u, fb_width, fb_height);
        }
    }
    // The fixed-size UI goes over the inset too, so it is never hidden by it.
    if (layer) {
        // Size and position each rounded to whole drawable pixels, so a
        // layer drawn at an integer magnification stays pixel exact
        // wherever it sits.
        const auto pixels = [](int size, float fraction) {
            return int(std::lround(double(size) * fraction));
        };
        // The layer's native 256 columns, centred in a wider image.
        const int native_x = int(int64_t(band.w) * ((width - int(kNativeWidth)) / 2) / width);
        const NdsPresentRect native{band.x + native_x, band.y,
                                    std::max(int(int64_t(band.w) * kNativeWidth / width), 1),
                                    band.h};
        const NdsPresentRect layer_rect{
            native.x + pixels(native.w, g_layer_rect[0]),
            native.y + pixels(native.h, g_layer_rect[1]),
            std::max(pixels(native.w, g_layer_rect[2]), 1),
            std::max(pixels(native.h, g_layer_rect[3]), 1)};
        // The main filter, on a premultiplied image. No box resolve.
        record_blit(slot, kLayer, layer_rect, bilinear,
                    kBlitKeepAlpha | kBlitPremultiplied, fb_width, fb_height);
    }
    if (overlay)
        record_blit(slot, kOverlay, band, bilinear, kBlitKeepAlpha, fb_width,
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

#if defined(__ANDROID__)
namespace {
// The bottom swap chain at the surface's current size; false when there is
// nothing to present to this frame.
bool bottom_resize_if_needed() {
    RenderSwapChain& chain = *g_bottom.swap_chain;
    if (!g_bottom.resize_pending && !chain.needsResize())
        return !g_bottom.framebuffers.empty();
    g_bottom.resize_pending = false;
    wait_bottom_slots();
    g_bottom.framebuffers.clear();
    if (!chain.resize()) {
        g_bottom.resize_pending = true;
        const bool zero_sized = chain.getWidth() == 0 || chain.getHeight() == 0;
        if (!zero_sized && !g_bottom.resize_failing) {
            g_bottom.resize_failing = true;
            std::fprintf(stderr, "[plume] bottom swap chain resize to %ux%u "
                                 "failed (0x%llx); retrying every frame\n",
                         chain.getWidth(), chain.getHeight(),
                         static_cast<unsigned long long>(chain.getLastError()));
        }
        return false;
    }
    g_bottom.resize_failing = false;
    if (chain.isEmpty()) return false;
    create_framebuffers_for(chain, g_bottom.framebuffers,
                            g_bottom.release_semaphores);
    std::fprintf(stderr, "[plume] bottom swap chain %ux%u, %u images, vsync %s\n",
                 chain.getWidth(), chain.getHeight(), chain.getTextureCount(),
                 chain.isVsyncEnabled() ? "on (MAILBOX if offered, else FIFO)"
                                        : "off");
    return !g_bottom.framebuffers.empty();
}

bool create_bottom_slots() {
    for (FrameSlot& slot : g_bottom.slots) {
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
}  // namespace

void nds_compute_host_set_bottom_window(ANativeWindow* window)
{
    if (window == g_bottom.window) return;
    release_bottom_chain();
    if (!window) return;
    if (!g_active || !g_backend.device || !g_backend.queue) {
        std::fprintf(stderr, "[plume] no device for the second display\n");
        return;
    }
    if (!g_bottom.slots_ready) {
        g_bottom.slots_ready = create_bottom_slots();
        if (!g_bottom.slots_ready) {
            std::fprintf(stderr, "[plume] bottom frame slot creation failed\n");
            for (FrameSlot& slot : g_bottom.slots) slot = FrameSlot{};
            return;
        }
    }
    g_bottom.swap_chain = g_backend.queue->createSwapChain(RenderSwapChainDesc(
        window, kSwapChainFormat, kSwapChainBuffers));
    if (!g_bottom.swap_chain) {
        std::fprintf(stderr, "[plume] no swap chain on the second display's "
                             "surface\n");
        return;
    }
    g_bottom.swap_chain->setVsyncEnabled(true);
    g_bottom.window = window;
    g_bottom.resize_pending = true;
    std::fprintf(stderr, "[plume] bottom swap chain made on the second display "
                         "(%dx%d surface)\n",
                 ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
}

bool nds_compute_host_has_bottom_window()
{
    return g_active && g_bottom.swap_chain != nullptr;
}

bool nds_compute_host_present_bottom(const uint32_t* pixels, unsigned width,
                                     const uint32_t* overlay,
                                     unsigned overlay_width,
                                     unsigned overlay_height,
                                     NdsComputePresentTicks* ticks)
{
    if (!g_active || !g_bottom.swap_chain || !pixels) return false;
    if (width == 0 || width > kMaxWidth) return false;
    const uint64_t begin = SDL_GetPerformanceCounter();
    if (!bottom_resize_if_needed()) return true;
    RenderSwapChain& chain = *g_bottom.swap_chain;
    if (chain.isEmpty()) return true;
    const uint32_t fb_width = chain.getWidth();
    const uint32_t fb_height = chain.getHeight();
    const NdsPresentRect rect = nds_present_rect(
        g_window_mode, g_scaling, int(fb_width), int(fb_height), int(width),
        int(kHeight));

    FrameSlot& slot = g_bottom.slots[g_bottom.slot];
    const bool have_overlay = overlay && overlay_width > 0 && overlay_height > 0 &&
                              overlay_width <= kMaxWidth * kOverlayMaxScale &&
                              overlay_height <= kHeight * kOverlayMaxScale;
    const bool fine_overlay = have_overlay && (overlay_height != kHeight ||
                                               overlay_width > kMaxWidth);
    auto* staging = static_cast<uint8_t*>(slot.staging->map());
    if (!staging) return false;
    bool staged = stage_image(slot, staging, kMain, pixels, int(width));
    if (staged && have_overlay && !fine_overlay)
        staged = stage_image(slot, staging, kOverlay, overlay, int(overlay_width));
    slot.staging->unmap();
    if (staged && fine_overlay)
        staged = stage_fine_overlay(slot, overlay, overlay_width, overlay_height);
    if (!staged) return false;
    if (ticks) ticks->upload += SDL_GetPerformanceCounter() - begin;

    // As on the top window: acquired only once nothing else can fail.
    uint32_t image_index = 0;
    if (!chain.acquireTexture(slot.acquire.get(), &image_index) ||
        image_index >= g_bottom.framebuffers.size()) {
        g_bottom.resize_pending = true;
        if (++g_bottom.dropped == 1)
            std::fprintf(stderr, "[plume] bottom swap chain acquire failed; "
                                 "frames are dropped until it recovers\n");
        return true;
    }

    uint64_t start = SDL_GetPerformanceCounter();
    RenderTexture* backbuffer = chain.getTexture(image_index);
    RenderCommandList& commands = *slot.commands;
    commands.begin();
    record_upload(slot, kMain);
    if (have_overlay && !fine_overlay) record_upload(slot, kOverlay);
    if (fine_overlay) record_fine_overlay_upload(slot);
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(backbuffer, RenderTextureLayout::COLOR_WRITE));
    commands.setFramebuffer(g_bottom.framebuffers[image_index].get());
    commands.setViewports(RenderViewport(0.0f, 0.0f, float(fb_width), float(fb_height)));
    commands.setScissors(RenderRect(0, 0, int32_t(fb_width), int32_t(fb_height)));
    commands.clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    // The top window's CPU-composite filters.
    const bool bilinear = g_scaling == NdsScalingFilter::Bilinear;
    const bool supersampled = int(width) > rect.w || int(kHeight) > rect.h;
    const bool box_resolve =
        (g_scaling == NdsScalingFilter::Integer && supersampled) ||
        (g_scaling == NdsScalingFilter::Nearest && int(width) >= 2 * rect.w &&
         int(kHeight) >= 2 * rect.h);
    record_blit(slot, kMain, rect, bilinear && !box_resolve,
                box_resolve ? kBlitBoxResolve : 0u, fb_width, fb_height);
    if (have_overlay)
        record_blit(slot, kOverlay, rect, bilinear, kBlitKeepAlpha, fb_width,
                    fb_height);
    commands.barriers(RenderBarrierStage::NONE,
                      RenderTextureBarrier(backbuffer, RenderTextureLayout::PRESENT));
    commands.end();

    const RenderCommandList* submit = slot.commands.get();
    RenderCommandSemaphore* wait = slot.acquire.get();
    RenderCommandSemaphore* signal = g_bottom.release_semaphores[image_index].get();
    g_backend.queue->executeCommandLists(&submit, 1, &wait, 1, &signal, 1,
                                         slot.fence.get());
    slot.submitted = true;
    if (ticks) ticks->draw += SDL_GetPerformanceCounter() - start;

    start = SDL_GetPerformanceCounter();
    if (!chain.present(image_index, &signal, 1)) {
        g_bottom.resize_pending = true;
        if (++g_bottom.dropped == 1)
            std::fprintf(stderr, "[plume] bottom present failed (0x%llx)\n",
                         static_cast<unsigned long long>(chain.getLastError()));
    }
    g_bottom.slot = (g_bottom.slot + 1u) % kFramesInFlight;
    wait_slot(g_bottom.slots[g_bottom.slot]);
    const uint64_t present_ticks = SDL_GetPerformanceCounter() - start;
    if (ticks) ticks->swap += present_ticks;

    const uint64_t total = SDL_GetPerformanceCounter() - begin;
    g_bottom.window_ticks += total;
    g_bottom.window_max_ticks = std::max(g_bottom.window_max_ticks, total);
    if (++g_bottom.presented % 600 == 0) {
        const double ms = 1000.0 / double(SDL_GetPerformanceFrequency());
        std::fprintf(stderr, "[plume] bottom: %llu frames, %llu dropped, last 600 "
                             "took %.2f ms avg, %.2f ms max\n",
                     static_cast<unsigned long long>(g_bottom.presented),
                     static_cast<unsigned long long>(g_bottom.dropped),
                     double(g_bottom.window_ticks) * ms / 600.0,
                     double(g_bottom.window_max_ticks) * ms);
        g_bottom.window_ticks = g_bottom.window_max_ticks = 0;
    }
    return true;
}
#endif

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
