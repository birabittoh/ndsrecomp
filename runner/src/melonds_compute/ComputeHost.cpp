// GPL-3.0-or-later; see ComputeHost.h and THIRD_PARTY_ATTRIBUTION.md.
#include "ComputeHost.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define SDL_MAIN_HANDLED
#if defined(NDS_HAVE_SDL3)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#else
#include <SDL.h>
#endif

#include "glad/glad.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "TextureUpscale.h"
#include "PresentShaders.h"

namespace
{
SDL_Window* g_window = nullptr;
SDL_GLContext g_context = nullptr;
bool g_owns_video = false;
bool g_owns_window = false;
bool g_active = false;
bool g_visible = false;
GLuint g_present_program = 0;
GLuint g_present_vao = 0;
GLuint g_fallback_texture[2] = {};
GLuint g_object_texture[2] = {};
GLuint g_raster_texture[2] = {};
GLuint g_hd_top_texture[2] = {};
GLuint g_hd_below_texture[2] = {};
int g_texture_width = 0;
unsigned g_present_buffer = 0;
NdsWindowMode g_window_mode = NdsWindowMode::Letterbox;
NdsScalingFilter g_scaling = NdsScalingFilter::Integer;
// The rect the last frame was drawn into; the readback observer reads it.
NdsPresentRect g_last_rect{};
// Bilinear: compose offscreen at the frame's own density, then resample.
GLuint g_blit_program = 0;
GLuint g_compose_fbo = 0;
GLuint g_compose_texture = 0;
int g_compose_width = 0;
int g_compose_height = 0;
GLuint g_overlay_texture = 0;
int g_overlay_texture_width = 0;
const uint32_t* g_overlay_pixels = nullptr;
unsigned g_overlay_width = 0;
GLuint g_inset_texture = 0;
int g_inset_texture_width = 0;
const uint32_t* g_inset_pixels = nullptr;
unsigned g_inset_width = 0;
int g_inset_percent = 0;

// GL identity, captured once the context is current. Fixed buffers rather
// than std::string: this is read from the diagnostics sampler, and the
// pointers must stay valid without imposing an allocation or a lifetime
// question on a periodic reader.
char g_gl_renderer[192] = {};
char g_gl_vendor[192] = {};
char g_gl_version[192] = {};

void copy_gl_string(char* dst, std::size_t cap, GLenum name)
{
    const char* src = reinterpret_cast<const char*>(glGetString(name));
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, cap, "%s", src);
}

void capture_gl_identity()
{
    copy_gl_string(g_gl_renderer, sizeof(g_gl_renderer), GL_RENDERER);
    copy_gl_string(g_gl_vendor, sizeof(g_gl_vendor), GL_VENDOR);
    copy_gl_string(g_gl_version, sizeof(g_gl_version), GL_VERSION);
}

#if defined(NDS_HAVE_SDL3)
bool sdl_ok(bool result)
{
    return result;
}
#else
bool sdl_ok(int result)
{
    return result == 0;
}
#endif

SDL_Window* create_gl_window(const char* title, int width, int height,
                             uint64_t flags)
{
#if defined(NDS_HAVE_SDL3)
    return SDL_CreateWindow(title, width, height,
                            static_cast<SDL_WindowFlags>(flags));
#else
    return SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, width, height,
                            static_cast<uint32_t>(flags));
#endif
}

bool sdl_init_video()
{
#if defined(NDS_HAVE_SDL3)
    return SDL_InitSubSystem(SDL_INIT_VIDEO);
#else
    return SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
#endif
}

bool sdl_make_current(SDL_Window* window, SDL_GLContext context)
{
    return sdl_ok(SDL_GL_MakeCurrent(window, context));
}

void sdl_destroy_context(SDL_GLContext context)
{
#if defined(NDS_HAVE_SDL3)
    SDL_GL_DestroyContext(context);
#else
    SDL_GL_DeleteContext(context);
#endif
}

void sdl_get_drawable_size(SDL_Window* window, int* width, int* height)
{
#if defined(NDS_HAVE_SDL3)
    SDL_GetWindowSizeInPixels(window, width, height);
#else
    SDL_GL_GetDrawableSize(window, width, height);
#endif
}

using nds_compute_present::kPresentVertex;
using nds_compute_present::kPresentFragment;
using nds_compute_present::kBlitFragment;

GLuint compile_shader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;
    char log[2048] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[gpu3d] direct-present shader failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
}

GLuint link_program(const char* vertex_source, const char* fragment_source)
{
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[2048] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gpu3d] direct-present link failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool start_presenter()
{
    g_present_program = link_program(kPresentVertex, kPresentFragment);
    if (!g_present_program) return false;
    g_blit_program = link_program(kPresentVertex, kBlitFragment);
    if (!g_blit_program) return false;
    glGenFramebuffers(1, &g_compose_fbo);
    glGenTextures(1, &g_compose_texture);
    glGenTextures(1, &g_overlay_texture);
    glGenTextures(1, &g_inset_texture);
    glGenVertexArrays(1, &g_present_vao);
    glGenTextures(2, g_fallback_texture);
    glGenTextures(2, g_object_texture);
    glGenTextures(2, g_raster_texture);
    glGenTextures(2, g_hd_top_texture);
    glGenTextures(2, g_hd_below_texture);
    return true;
}

void stop_presenter()
{
    if (g_compose_fbo) glDeleteFramebuffers(1, &g_compose_fbo);
    if (g_compose_texture) glDeleteTextures(1, &g_compose_texture);
    if (g_blit_program) glDeleteProgram(g_blit_program);
    if (g_overlay_texture) glDeleteTextures(1, &g_overlay_texture);
    g_overlay_texture = 0;
    g_overlay_texture_width = 0;
    if (g_inset_texture) glDeleteTextures(1, &g_inset_texture);
    g_inset_texture = 0;
    g_inset_texture_width = 0;
    g_compose_fbo = 0;
    g_compose_texture = 0;
    g_blit_program = 0;
    g_compose_width = g_compose_height = 0;
    g_last_rect = {};
    glDeleteTextures(2, g_hd_below_texture);
    glDeleteTextures(2, g_hd_top_texture);
    glDeleteTextures(2, g_object_texture);
    glDeleteTextures(2, g_raster_texture);
    glDeleteTextures(2, g_fallback_texture);
    if (g_present_vao) glDeleteVertexArrays(1, &g_present_vao);
    if (g_present_program) glDeleteProgram(g_present_program);
    g_object_texture[0] = g_object_texture[1] = 0;
    g_raster_texture[0] = g_raster_texture[1] = 0;
    g_fallback_texture[0] = g_fallback_texture[1] = 0;
    g_hd_top_texture[0] = g_hd_top_texture[1] = 0;
    g_hd_below_texture[0] = g_hd_below_texture[1] = 0;
    g_present_vao = 0;
    g_present_program = 0;
    g_texture_width = 0;
    g_present_buffer = 0;
}

void configure_integer_texture(GLuint texture, int width)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, width, 192, 0,
                 GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
}

// The HD layer surfaces carry a 24-bit colour plus a packed 32-bit key, so
// they need a wider integer format than the RGBA8UI OBJ surface.
void configure_layer_texture(GLuint texture, int width)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32UI, width, 192, 0,
                 GL_RG_INTEGER, GL_UNSIGNED_INT, nullptr);
}

// (Re)size the offscreen compose target. RGBA8 holds the composed colour
// exactly as the default framebuffer would.
bool ensure_compose_target(int width, int height)
{
    if (g_compose_width == width && g_compose_height == height) return true;
    glBindTexture(GL_TEXTURE_2D, g_compose_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, g_compose_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_compose_texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                          GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
        std::fprintf(stderr,
                     "[gpu3d] bilinear compose target %dx%d incomplete\n",
                     width, height);
        g_compose_width = g_compose_height = 0;
        return false;
    }
    g_compose_width = width;
    g_compose_height = height;
    return true;
}

void release_host_objects()
{
    if (g_context && g_window) {
        sdl_make_current(g_window, g_context);
        stop_presenter();
    }
    if (g_context) nds_texture_upscale_shutdown();
    if (g_context) sdl_destroy_context(g_context);
    if (g_window && g_owns_window) SDL_DestroyWindow(g_window);
    if (g_owns_video) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    g_window = nullptr;
    g_context = nullptr;
    g_owns_video = false;
    g_owns_window = false;
    g_active = false;
    g_visible = false;
}
}

bool nds_compute_host_start(SDL_Window* presentation_window)
{
    const NdsGpu3dRendererPolicy policy = nds_gpu3d_renderer_policy();
    if (policy == NdsGpu3dRendererPolicy::Soft) return true;
    if (g_active) return true;
    const bool required = policy == NdsGpu3dRendererPolicy::Compute;
    auto fail_or_fallback = [&](const char* reason) {
        nds_gpu3d_restore_soft_renderer();
        release_host_objects();
        if (required) return false;
        std::fprintf(stderr,
            "[gpu3d] OpenGL auto-selection failed (%s); "
            "using threaded software\n", reason);
        return true;
    };

    SDL_SetMainReady();
    g_owns_video = (SDL_WasInit(SDL_INIT_VIDEO) == 0);
    if (g_owns_video && !sdl_init_video()) {
        std::fprintf(stderr, "[gpu3d] SDL video init failed: %s\n",
                     SDL_GetError());
        return fail_or_fallback("SDL video initialization");
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, presentation_window ? 1 : 0);
    g_window = presentation_window;
    g_visible = presentation_window != nullptr;
    if (!g_window) {
        g_window = create_gl_window(
            "ndsrecomp compute context", 1, 1,
            SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        g_owns_window = true;
        if (!g_window) {
            std::fprintf(stderr, "[gpu3d] compute GL window failed: %s\n",
                         SDL_GetError());
            SDL_GL_ResetAttributes();
            return fail_or_fallback("OpenGL window creation");
        }
    }
    g_context = SDL_GL_CreateContext(g_window);
    SDL_GL_ResetAttributes();
    if (!g_context || !sdl_make_current(g_window, g_context)) {
        std::fprintf(stderr, "[gpu3d] compute GL 4.3 context failed: %s\n",
                     SDL_GetError());
        return fail_or_fallback("OpenGL 4.3 context creation");
    }
    if (!gladLoadGLLoader(
            reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) ||
        !GLAD_GL_VERSION_4_3) {
        std::fprintf(stderr, "[gpu3d] OpenGL 4.3 unavailable\n");
        return fail_or_fallback("OpenGL 4.3 unavailable");
    }
    capture_gl_identity();
    std::fprintf(stderr, "[gpu3d] OpenGL %s / %s\n",
                 nds_compute_host_gl_version(),
                 nds_compute_host_gl_renderer());
    // Must precede the renderer: the factor sizes every texture-array
    // allocation and the cache's free-list is derived from it. A failure here
    // resets the factor to 1 rather than aborting, so the renderer still
    // starts with native textures.
    nds_texture_upscale_init();
    if (!nds_gpu3d_use_compute_renderer()) {
        std::fprintf(stderr, "[gpu3d] compute renderer init failed\n");
        return fail_or_fallback("compute renderer initialization");
    }
    if (g_visible && !start_presenter()) {
        std::fprintf(stderr, "[gpu3d] direct presenter init failed\n");
        return fail_or_fallback("direct presenter initialization");
    }
    if (g_visible) SDL_GL_SetSwapInterval(0);
    g_active = true;
    std::fprintf(stderr, "[gpu3d] renderer: %s\n",
                 nds_gpu3d_compiled_accelerated_backend());
    return true;
}

bool nds_compute_host_make_current()
{
    return !g_active || sdl_make_current(g_window, g_context);
}

bool nds_compute_host_has_visible_context()
{
    return g_active && g_visible;
}

bool nds_compute_host_composes_3d()
{
    return true;
}

bool nds_compute_host_presents_direct()
{
    return true;
}

void nds_compute_host_set_raster_request(unsigned, unsigned)
{
    // The GL renderers take their whole-multiple scale from gpu3d.
}

void nds_compute_host_set_thin_outlines(bool)
{
    // melonDS's GL renderer draws no outlines above native.
}

void nds_compute_host_set_cache_directory(const char*)
{
    // The GL program binary cache has its own location (OpenGLSupport).
}

bool nds_compute_host_read_presented(std::vector<uint32_t>& pixels,
                                      unsigned& width, unsigned& height)
{
    width = height = 0;
    if (!g_active || !g_visible || !g_texture_width ||
        !sdl_make_current(g_window, g_context)) return false;
    int dw = 0, dh = 0;
    sdl_get_drawable_size(g_window, &dw, &dh);
    // The rect the last frame went into, clipped to the drawable (Original
    // can exceed a small window).
    struct { int x, y, width, height; } v{};
    v.x = std::max(g_last_rect.x, 0);
    v.y = std::max(g_last_rect.y, 0);
    v.width = std::min(g_last_rect.x + g_last_rect.w, dw) - v.x;
    v.height = std::min(g_last_rect.y + g_last_rect.h, dh) - v.y;
    if (dw <= 0 || dh <= 0 || v.width <= 0 || v.height <= 0 ||
        uint64_t(v.width) * v.height > 16000000u)
        return false;
    GLint old_fbo, old_pack, old_alignment, old_length, old_rows, old_pixels;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLint old_read_buffer;
    glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
    glReadBuffer(GL_FRONT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    std::vector<uint32_t> rgba(size_t(v.width) * v.height);
    glReadPixels(v.x, v.y, v.width, v.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const GLenum error = glGetError();
    glReadBuffer(old_read_buffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(old_fbo));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(old_pack));
    glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_pixels);
    if (error != GL_NO_ERROR) return false;
    pixels.resize(rgba.size());
    for (int y = 0; y < v.height; ++y) for (int x = 0; x < v.width; ++x) {
        const uint32_t p = rgba[size_t(v.height - 1 - y) * v.width + x];
        pixels[size_t(y) * v.width + x] = 0xFF000000u | ((p & 255u) << 16) |
            (p & 0xFF00u) | ((p >> 16) & 255u);
    }
    width = static_cast<unsigned>(v.width);
    height = static_cast<unsigned>(v.height);
    return true;
}

void nds_compute_host_set_overlay(const uint32_t* argb, unsigned width)
{
    g_overlay_pixels = width > 0 && width <= 448 ? argb : nullptr;
    g_overlay_width = g_overlay_pixels ? width : 0;
}

// The GL presenter has no layer pass; the frontend keeps the full composite.
bool nds_compute_host_supports_layer()
{
    return false;
}

void nds_compute_host_set_layer(const uint32_t*, const uint32_t*, unsigned,
                                float, float, float, float)
{
}

void nds_compute_host_set_inset(const uint32_t* argb, unsigned width,
                                int percent)
{
    const bool shown = argb && percent > 0 && width > 0 && width <= 448;
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
    if (!g_active || !g_visible || !g_present_program || !ticks)
        return false;
    const auto diagnostic_begin = ticks->diagnostic ? SDL_GetPerformanceCounter() : 0;
    if (!sdl_make_current(g_window, g_context)) return false;
    const int width = direct_frame ? direct_frame->width : fallback_width;
    if (width <= 0 || width > 448) return false;
    if (g_texture_width != width) {
        for (int i = 0; i < 2; ++i) {
            configure_integer_texture(g_fallback_texture[i], width);
            configure_integer_texture(g_object_texture[i], width);
            glBindTexture(GL_TEXTURE_2D, g_raster_texture[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, 1, 192, 0,
                         GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
            configure_layer_texture(g_hd_top_texture[i], width);
            configure_layer_texture(g_hd_below_texture[i], width);
        }
        g_texture_width = width;
    }
    const unsigned buffer = g_present_buffer;
    g_present_buffer ^= 1u;

    // HD outranks the OBJ-only direct path: it covers the same scenes plus
    // every one carrying BG layers, and it is the only path that can consume
    // the high-resolution 3D surface. It stands down whenever the compositor
    // did not emit surfaces for this frame (native width, engine B on top,
    // unsupported scene, or skybox repair).
    NdsGpu2dHdFrame hd{};
    const auto hires_texture=owned ? owned->hires_texture : nds_gpu3d_compute_output_texture_hires();
    const auto native_texture=owned ? owned->native_texture : nds_gpu3d_compute_output_texture();
    const auto scale=hires_texture ? (owned ? owned->scale : nds_gpu3d_internal_scale()) : 1u;
    const bool hd_active =
        hires_texture != 0u && (owned ? (hd=owned->hd,owned->has_hd) : nds_gpu2d_hd_frame(&hd)) &&
        hd.width == width && hd.top_pixels && hd.below_pixels;
    if (owned && hd_active) nds_gpu2d_note_owned_hd_presented();

    unsigned long long start = SDL_GetPerformanceCounter();
    if (ticks->diagnostic) ticks->setup = start - diagnostic_begin;
    if (hd_active) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, g_hd_top_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_RG_INTEGER, GL_UNSIGNED_INT, hd.top_pixels);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, g_hd_below_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_RG_INTEGER, GL_UNSIGNED_INT, hd.below_pixels);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    } else if (direct_frame) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_object_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                        direct_frame->object_pixels);
        if (direct_frame->raster_params) {
            glActiveTexture(GL_TEXTURE6);
            glBindTexture(GL_TEXTURE_2D, g_raster_texture[buffer]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 192,
                GL_RGBA_INTEGER, GL_UNSIGNED_INT, direct_frame->raster_params);
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    } else {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_fallback_texture[buffer]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, 192,
                        GL_BGRA_INTEGER, GL_UNSIGNED_BYTE, fallback_pixels);
    }
    ticks->upload += SDL_GetPerformanceCounter() - start;

    start = SDL_GetPerformanceCounter();
    int drawable_width = 0;
    int drawable_height = 0;
    sdl_get_drawable_size(g_window, &drawable_width, &drawable_height);
    const NdsSingleLayout layout = nds_single_layout(
        g_window_mode, g_scaling, drawable_width, drawable_height, width,
        192, g_inset_percent);
    const NdsPresentRect rect = layout.main;
    g_last_rect = rect;
    // GL's viewport origin is bottom-left.
    const int viewport_y = drawable_height - rect.y - rect.h;
    // Integer scaling composes straight into the rect while the 3D raster
    // is no denser than it: nearest at a whole factor is exact. A denser
    // raster (resolution_scale above 100%) composes offscreen and is box
    // resolved. Bilinear composes offscreen at the frame's own sample
    // density (the 3D raster's when it is denser than native, so HD detail
    // survives) and resamples that linearly. When the offscreen image would
    // be exactly the rect's size the resample is the identity, so skip it.
    const int density = (hd_active || direct_frame) ? int(scale) : 1;
    const int compose_width = width * density;
    const int compose_height = 192 * density;
    const bool supersampled =
        compose_width > rect.w || compose_height > rect.h;
    // Nearest composes straight into its exact rect too (point sampling is
    // what it asks for), box resolving only a raster at least twice as dense,
    // i.e. resolution_scale supersampling rather than the round-up remainder.
    const bool box_resolve =
        (g_scaling == NdsScalingFilter::Integer && supersampled) ||
        (g_scaling == NdsScalingFilter::Nearest &&
         compose_width >= 2 * rect.w && compose_height >= 2 * rect.h);
    const bool offscreen =
        (box_resolve || (g_scaling == NdsScalingFilter::Bilinear &&
                         (compose_width != rect.w ||
                          compose_height != rect.h))) &&
        ensure_compose_target(compose_width, compose_height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    if (offscreen) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_compose_fbo);
        glViewport(0, 0, compose_width, compose_height);
    } else {
        glViewport(rect.x, viewport_y, rect.w, rect.h);
    }
    glUseProgram(g_present_program);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_object_texture[buffer]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_fallback_texture[buffer]);
    glUniform1ui(0, direct_frame ? 1u : 0u);
    glUniform1ui(1, static_cast<GLuint>(width));
    glUniform1ui(11, hd_active ? 1u : 0u);
    glUniform1ui(13, direct_frame && direct_frame->raster_params && !hd_active ? 1u : 0u);
    glUniform1ui(14, antialiasing);
    if (hd_active) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, hires_texture);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, g_hd_top_texture[buffer]);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, g_hd_below_texture[buffer]);
        glUniform1ui(10, scale);
        glUniform1ui(12, hd.order_3d);
        glUniform1ui(2, hd.render_xpos);
        glUniform1ui(4, hd.bldcnt);
        glUniform1ui(5, hd.eva);
        glUniform1ui(6, hd.evb);
        glUniform1ui(7, hd.evy);
        glUniform1ui(8, hd.master_bright);
        glUniform1ui(9, hd.priority_3d);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    } else if (direct_frame) {
        // Sample the high-resolution surface only when one actually exists;
        // the accessor returns zero at scale 1 or under the soft renderer, in
        // which case the presenter stays on the native integer path.
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, native_texture);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, hires_texture);
        glUniform1ui(10, static_cast<GLuint>(scale));
        glUniform1ui(2, direct_frame->render_xpos);
        glUniform3ui(3,
            direct_frame->backdrop_color & 0x3Fu,
            (direct_frame->backdrop_color >> 8) & 0x3Fu,
            (direct_frame->backdrop_color >> 16) & 0x3Fu);
        glUniform1ui(4, direct_frame->bldcnt);
        glUniform1ui(5, direct_frame->eva);
        glUniform1ui(6, direct_frame->evb);
        glUniform1ui(7, direct_frame->evy);
        glUniform1ui(8, direct_frame->master_bright);
        glUniform1ui(9, direct_frame->priority_3d);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }
    glBindVertexArray(g_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (offscreen) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(rect.x, viewport_y, rect.w, rect.h);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, g_compose_texture);
        // Shrinking (a dense HD raster into a smaller rect) goes through the
        // mip chain so it averages instead of skipping samples; integer
        // scaling box-resolves the base level instead.
        const bool mipmapped = supersampled && !box_resolve;
        if (mipmapped) glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glUseProgram(g_blit_program);
        glUniform1ui(0, 0u);
        glUniform1ui(1, box_resolve ? 1u : 0u);
        glUniform2i(2, rect.w, rect.h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (g_inset_pixels && layout.inset.w > 0 && layout.inset.h > 0) {
        const NdsPresentRect& inset = layout.inset;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(inset.x, drawable_height - inset.y - inset.h, inset.w,
                   inset.h);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, g_inset_texture);
        const int inset_width = static_cast<int>(g_inset_width);
        if (g_inset_texture_width != inset_width) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, inset_width, 192, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            g_inset_texture_width = inset_width;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, inset_width, 192, GL_BGRA,
                        GL_UNSIGNED_BYTE, g_inset_pixels);
        // Point sampling a shrunk inset drops whole rows of text.
        const GLint filter = g_scaling == NdsScalingFilter::Bilinear ||
                                     inset.w < inset_width
            ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glUseProgram(g_blit_program);
        glUniform1ui(0, 1u);
        glUniform1ui(1, 0u);
        // The source's alpha byte is not meaningful: keep the window opaque.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (g_overlay_pixels) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(rect.x, viewport_y, rect.w, rect.h);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
        const int overlay_width = static_cast<int>(g_overlay_width);
        if (g_overlay_texture_width != overlay_width) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, overlay_width, 192, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            g_overlay_texture_width = overlay_width;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, overlay_width, 192, GL_BGRA,
                        GL_UNSIGNED_BYTE, g_overlay_pixels);
        const GLint filter = g_scaling == NdsScalingFilter::Bilinear
            ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(g_blit_program);
        glUniform1ui(0, 1u);
        glUniform1ui(1, 0u);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
    }
    ticks->draw += SDL_GetPerformanceCounter() - start;

    // Keep the qualified per-frame GL checks. A window-system swap failure
    // is reported separately by SDL3 rather than through glGetError.
    start = SDL_GetPerformanceCounter();
#if defined(NDS_HAVE_SDL3)
    const bool swapped = SDL_GL_SwapWindow(g_window);
#else
    SDL_GL_SwapWindow(g_window);
    const bool swapped = true;
#endif
    ticks->swap += SDL_GetPerformanceCounter() - start;
    if (!swapped) return false;
    const auto diagnostic_error = ticks->diagnostic ? SDL_GetPerformanceCounter() : 0;
    const bool ok = glGetError() == GL_NO_ERROR;
    if (ticks->diagnostic)
        ticks->error = SDL_GetPerformanceCounter() - diagnostic_error;
    return ok;
}

void nds_compute_host_stop()
{
    if (!g_active) {
        release_host_objects();
        return;
    }
    sdl_make_current(g_window, g_context);
    nds_gpu3d_restore_soft_renderer();
    release_host_objects();
}

bool nds_compute_host_active()
{
    return g_active;
}

NdsGlProc nds_compute_host_gl_proc(const char* name)
{
    if (!g_context || !name || !*name) return nullptr;
    return reinterpret_cast<NdsGlProc>(SDL_GL_GetProcAddress(name));
}

const char* nds_compute_host_gl_renderer()
{
    return g_gl_renderer;
}

const char* nds_compute_host_gl_vendor()
{
    return g_gl_vendor;
}

const char* nds_compute_host_gl_version()
{
    return g_gl_version;
}
