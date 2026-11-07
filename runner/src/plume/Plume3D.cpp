// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// The DS 3D engine on Plume (see Plume3D.h). A port of melonDS's
// GPU3D_OpenGL.cpp (GPL-3.0-or-later, melonDS team): the same polygon setup,
// render keys, batching and the opaque / translucent / shadow passes with
// their exact stencil programs. What differs:
//
// - The raster is any size. Vertices are melonDS's 1/16 pixel HiresPosition
//   times a float raster/native ratio per axis, so a 1080 line window gets a
//   1080 line raster rather than 1152 (6x) or 960 (5x).
// - Fixed GL state becomes pipelines. Plume pipelines carry every state the
//   GL code toggles (depth func and mask, colour masks, blend, stencil func,
//   masks and ops); they are built on first use and kept, keyed by that
//   state, with the stencil reference left dynamic.
// - The final pass does edge marking, fog and the RGB6/alpha5 quantisation in
//   one shader that reads the raster (no blend constants). Edge marking
//   works here; melonDS GL's edge pass is disabled.
// - Lines become quads one DS pixel thick, so they need no line pipelines.
// - Nothing is read back: the faithful renderer serves every CPU consumer.
#include "Plume3D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "PlumeContext.h"
#include "GPU.h"

#include "shaders/ndsFullscreenVert.hlsl.spirv.h"
#include "shaders/ndsGxVertZ.hlsl.spirv.h"
#include "shaders/ndsGxVertW.hlsl.spirv.h"
#include "shaders/ndsGxOpaqueZ.hlsl.spirv.h"
#include "shaders/ndsGxOpaqueW.hlsl.spirv.h"
#include "shaders/ndsGxTransZ.hlsl.spirv.h"
#include "shaders/ndsGxTransW.hlsl.spirv.h"
#include "shaders/ndsGxShadowZ.hlsl.spirv.h"
#include "shaders/ndsGxShadowW.hlsl.spirv.h"
#include "shaders/ndsGxNull.hlsl.spirv.h"
#include "shaders/ndsGxFinal.hlsl.spirv.h"
#if defined(_WIN32)
#include "shaders/ndsFullscreenVert.hlsl.dxil.h"
#include "shaders/ndsGxVertZ.hlsl.dxil.h"
#include "shaders/ndsGxVertW.hlsl.dxil.h"
#include "shaders/ndsGxOpaqueZ.hlsl.dxil.h"
#include "shaders/ndsGxOpaqueW.hlsl.dxil.h"
#include "shaders/ndsGxTransZ.hlsl.dxil.h"
#include "shaders/ndsGxTransW.hlsl.dxil.h"
#include "shaders/ndsGxShadowZ.hlsl.dxil.h"
#include "shaders/ndsGxShadowW.hlsl.dxil.h"
#include "shaders/ndsGxNull.hlsl.dxil.h"
#include "shaders/ndsGxFinal.hlsl.dxil.h"
#define NDS_DXIL(name) name##BlobDXIL, sizeof(name##BlobDXIL)
#else
#define NDS_DXIL(name) nullptr, 0
#endif
#define NDS_SHADER(name, entry) \
    nds_plume_create_shader(name##BlobSPIRV, sizeof(name##BlobSPIRV), NDS_DXIL(name), entry)

namespace {
using namespace plume;
using melonDS::Polygon;
using melonDS::Vertex;

constexpr RenderFormat kColorFormat = RenderFormat::R8G8B8A8_UNORM;
constexpr RenderFormat kAttrFormat = RenderFormat::R8G8B8A8_UNORM;
constexpr RenderFormat kDepthCopyFormat = RenderFormat::R32_FLOAT;
constexpr RenderFormat kDepthFormat = RenderFormat::D32_FLOAT_S8_UINT;
constexpr RenderFormat kOutputFormat = RenderFormat::R8G8B8A8_UNORM;
constexpr uint32_t kSlots = 2;
// Ten vertices plus the centre per polygon, and a triangle per edge.
constexpr uint32_t kMaxVertices = 2048u * 11u;
constexpr uint32_t kMaxIndices = 2048u * 10u * 3u;
constexpr uint64_t kTexMemBytes = 1024u * 512u;
constexpr uint64_t kTexPalBytes = 1024u * 64u * 2u;
// 16x the widest adaptive surface; the GL renderers' own cap.
constexpr uint32_t kMaxRasterWidth = 448u * 16u;
constexpr uint32_t kMaxRasterHeight = 192u * 16u;

// Mirrors VSInput in gx.hlsl.
struct GxVertex {
    float x, y;
    uint32_t z, w, attr, texparam;
    uint8_t r, g, b, a;
    int16_t s, t;
    uint32_t texpal;
};
static_assert(sizeof(GxVertex) == 36);

// Mirrors GxConfig in gx.hlsl.
struct GxConfig {
    float screen_size[2];
    uint32_t disp_cnt;
    uint32_t alpha_ref;
    uint32_t toon[32];
    uint32_t edge[8];
    uint32_t fog_density[36];
    uint32_t fog_color;
    uint32_t fog_offset;
    uint32_t fog_shift;
    uint32_t edge_distance;
    uint32_t clear_poly_id;
    uint32_t clear_depth;
    uint32_t pad[2];
};
static_assert(sizeof(GxConfig) == 352);
// D3D12 constant buffer views come in 256 byte units.
constexpr uint64_t kConfigBytes = 512;

enum Program : uint8_t { kOpaque, kTrans, kShadow, kNull, kProgramCount };

// Every piece of GL state RenderSceneChunk toggles. Tracked like GL state
// (each setter changes only its own field) so the pass code below reads as
// the GL code does, and turned into a pipeline at each draw.
struct DrawState {
    uint8_t program = kOpaque;
    bool wbuffer = false;
    bool depth_test = true;
    bool depth_lequal = false;
    bool depth_write = true;
    bool color_write = true;
    uint8_t attr_mask = 0xF;
    bool blend = false;
    bool alpha_blend = false;
    RenderComparisonFunction stencil_func = RenderComparisonFunction::ALWAYS;
    uint8_t read_mask = 0xFF;
    uint8_t write_mask = 0xFF;
    RenderStencilOp fail_op = RenderStencilOp::KEEP;
    RenderStencilOp depth_fail_op = RenderStencilOp::KEEP;
    RenderStencilOp pass_op = RenderStencilOp::KEEP;

    uint64_t key() const {
        uint64_t k = program;
        k = (k << 1) | wbuffer;
        k = (k << 1) | depth_test;
        k = (k << 1) | depth_lequal;
        k = (k << 1) | depth_write;
        k = (k << 1) | color_write;
        k = (k << 4) | attr_mask;
        k = (k << 1) | blend;
        k = (k << 1) | alpha_blend;
        k = (k << 4) | uint64_t(stencil_func);
        k = (k << 8) | read_mask;
        k = (k << 8) | write_mask;
        k = (k << 4) | uint64_t(fail_op);
        k = (k << 4) | uint64_t(depth_fail_op);
        k = (k << 4) | uint64_t(pass_op);
        return k;
    }
};

struct RendererPolygon {
    const Polygon* poly = nullptr;
    uint32_t num_indices = 0;
    uint32_t indices_offset = 0;
    uint32_t render_key = 0;
};

struct Slot {
    std::unique_ptr<RenderCommandList> commands;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderBuffer> vertices;
    std::unique_ptr<RenderBuffer> indices;
    std::unique_ptr<RenderBuffer> config;
    std::unique_ptr<RenderBuffer> staging;   // texture then palette memory
    std::unique_ptr<RenderDescriptorSet> render_set;
    std::unique_ptr<RenderDescriptorSet> final_set;
    bool submitted = false;
};

struct Targets {
    std::unique_ptr<RenderTexture> color, attr, depth_copy, depth, output;
    std::unique_ptr<RenderFramebuffer> main, final;
    uint32_t width = 0, height = 0;
};

struct Renderer {
    bool ready = false;
    std::unique_ptr<RenderShader> shaders_vs[2];             // Z, W
    std::unique_ptr<RenderShader> shaders_ps[kNull][2];      // program x Z/W
    std::unique_ptr<RenderShader> fullscreen_vs, null_ps, final_ps;
    std::unique_ptr<RenderPipelineLayout> render_layout, final_layout;
    std::unique_ptr<RenderPipeline> final_pipeline;
    std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> pipelines;
    std::unique_ptr<RenderTexture> tex_mem, tex_pal;
    Targets targets;
    Slot slots[kSlots];
    uint32_t slot = 0;
    uint32_t want_width = 0, want_height = 0;
    uint64_t uploaded_generation = ~0ull;
    bool valid = false;
    uint64_t frames = 0;

    RendererPolygon polygons[2048];
    GxVertex vertex_data[kMaxVertices];
    uint32_t index_data[kMaxIndices];
    uint32_t num_vertices = 0, num_indices = 0;
    int num_polys = 0, num_opaque_polys = -1;
};

Renderer* g_renderer = nullptr;

RenderDescriptorSetDesc render_set_desc() {
    static const RenderDescriptorRange ranges[] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1),
    };
    return RenderDescriptorSetDesc(ranges, 3);
}

RenderDescriptorSetDesc final_set_desc() {
    static const RenderDescriptorRange ranges[] = {
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 0, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 1, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1),
        RenderDescriptorRange(RenderDescriptorRangeType::TEXTURE, 3, 1),
    };
    return RenderDescriptorSetDesc(ranges, 4);
}

RenderDevice& device() { return *nds_plume_context()->device; }

void wait_slot(Slot& slot) {
    if (!slot.submitted) return;
    nds_plume_context()->queue->waitForCommandFence(slot.fence.get());
    slot.submitted = false;
}

void wait_all(Renderer& r) {
    for (Slot& slot : r.slots) wait_slot(slot);
}

bool create_shaders(Renderer& r) {
    r.shaders_vs[0] = NDS_SHADER(ndsGxVertZ, "VSMainZ");
    r.shaders_vs[1] = NDS_SHADER(ndsGxVertW, "VSMainW");
    r.shaders_ps[kOpaque][0] = NDS_SHADER(ndsGxOpaqueZ, "PSOpaqueZ");
    r.shaders_ps[kOpaque][1] = NDS_SHADER(ndsGxOpaqueW, "PSOpaqueW");
    r.shaders_ps[kTrans][0] = NDS_SHADER(ndsGxTransZ, "PSTransZ");
    r.shaders_ps[kTrans][1] = NDS_SHADER(ndsGxTransW, "PSTransW");
    r.shaders_ps[kShadow][0] = NDS_SHADER(ndsGxShadowZ, "PSShadowZ");
    r.shaders_ps[kShadow][1] = NDS_SHADER(ndsGxShadowW, "PSShadowW");
    r.fullscreen_vs = NDS_SHADER(ndsFullscreenVert, "VSMain");
    r.null_ps = NDS_SHADER(ndsGxNull, "PSNull");
    r.final_ps = NDS_SHADER(ndsGxFinal, "PSFinal");
    for (auto& vs : r.shaders_vs) if (!vs) return false;
    for (auto& program : r.shaders_ps)
        for (auto& ps : program) if (!ps) return false;
    return r.fullscreen_vs && r.null_ps && r.final_ps;
}

bool create_layouts(Renderer& r) {
    const RenderDescriptorSetDesc render_desc = render_set_desc();
    RenderPipelineLayoutDesc desc;
    desc.descriptorSetDescs = &render_desc;
    desc.descriptorSetDescsCount = 1;
    desc.allowInputLayout = true;
    r.render_layout = device().createPipelineLayout(desc);
    const RenderDescriptorSetDesc final_desc = final_set_desc();
    RenderPipelineLayoutDesc fdesc;
    fdesc.descriptorSetDescs = &final_desc;
    fdesc.descriptorSetDescsCount = 1;
    r.final_layout = device().createPipelineLayout(fdesc);
    if (!r.render_layout || !r.final_layout) return false;

    RenderGraphicsPipelineDesc pdesc;
    pdesc.pipelineLayout = r.final_layout.get();
    pdesc.vertexShader = r.fullscreen_vs.get();
    pdesc.pixelShader = r.final_ps.get();
    pdesc.renderTargetFormat[0] = kOutputFormat;
    pdesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    pdesc.renderTargetCount = 1;
    pdesc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    pdesc.cullMode = RenderCullMode::NONE;
    r.final_pipeline = device().createGraphicsPipeline(pdesc);
    return r.final_pipeline != nullptr;
}

RenderPipeline* pipeline_for(Renderer& r, const DrawState& s) {
    const uint64_t key = s.key();
    auto found = r.pipelines.find(key);
    if (found != r.pipelines.end()) return found->second.get();

    static const RenderInputSlot input_slot(0, sizeof(GxVertex));
    static const RenderInputElement input_elements[] = {
        RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, 0),
        RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32B32A32_UINT, 0, 8),
        RenderInputElement("COLOR", 0, 2, RenderFormat::R8G8B8A8_UINT, 0, 24),
        RenderInputElement("TEXCOORD", 1, 3, RenderFormat::R16G16_SINT, 0, 28),
        RenderInputElement("TEXCOORD", 2, 4, RenderFormat::R32_UINT, 0, 32),
    };

    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = r.render_layout.get();
    if (s.program == kNull) {
        desc.vertexShader = r.fullscreen_vs.get();
        desc.pixelShader = r.null_ps.get();
    } else {
        desc.vertexShader = r.shaders_vs[s.wbuffer].get();
        desc.pixelShader = r.shaders_ps[s.program][s.wbuffer].get();
        desc.inputSlots = &input_slot;
        desc.inputSlotsCount = 1;
        desc.inputElements = input_elements;
        desc.inputElementsCount = uint32_t(std::size(input_elements));
    }
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = RenderCullMode::NONE;
    desc.renderTargetCount = 3;
    desc.renderTargetFormat[0] = kColorFormat;
    desc.renderTargetFormat[1] = kAttrFormat;
    desc.renderTargetFormat[2] = kDepthCopyFormat;
    desc.depthTargetFormat = kDepthFormat;

    // GL blends every draw buffer; the attribute pixel always has alpha 1, so
    // its blend is a replace, and the depth copy is never blended.
    RenderBlendDesc color;
    color.blendEnabled = s.blend;
    color.srcBlend = s.alpha_blend ? RenderBlend::SRC_ALPHA : RenderBlend::ONE;
    color.dstBlend = s.alpha_blend ? RenderBlend::INV_SRC_ALPHA : RenderBlend::ZERO;
    color.blendOp = RenderBlendOperation::ADD;
    color.srcBlendAlpha = RenderBlend::ONE;
    color.dstBlendAlpha = RenderBlend::ONE;
    color.blendOpAlpha = RenderBlendOperation::MAX;
    color.renderTargetWriteMask = s.color_write ? uint8_t(RenderColorWriteEnable::ALL) : 0;
    desc.renderTargetBlend[0] = color;
    RenderBlendDesc attr = RenderBlendDesc::Copy();
    attr.renderTargetWriteMask = s.attr_mask;
    desc.renderTargetBlend[1] = attr;
    RenderBlendDesc depth_copy = RenderBlendDesc::Copy();
    depth_copy.renderTargetWriteMask =
        s.depth_write ? uint8_t(RenderColorWriteEnable::RED) : 0;
    desc.renderTargetBlend[2] = depth_copy;

    desc.depthEnabled = s.depth_test;
    desc.depthWriteEnabled = s.depth_test && s.depth_write;
    desc.depthFunction = s.depth_lequal ? RenderComparisonFunction::LESS_EQUAL
                                        : RenderComparisonFunction::LESS;
    desc.stencilEnabled = true;
    desc.stencilReadMask = s.read_mask;
    desc.stencilWriteMask = s.write_mask;
    desc.dynamicStencilReferenceEnabled = true;
    RenderStencilFaceDesc face;
    face.compareFunction = s.stencil_func;
    face.failOp = s.fail_op;
    face.depthFailOp = s.depth_fail_op;
    face.passOp = s.pass_op;
    desc.stencilFrontFace = face;
    desc.stencilBackFace = face;

    auto pipeline = device().createGraphicsPipeline(desc);
    RenderPipeline* result = pipeline.get();
    if (!pipeline) {
        std::fprintf(stderr, "[plume3d] pipeline creation failed (key %016llx)\n",
                     static_cast<unsigned long long>(key));
        return nullptr;
    }
    r.pipelines.emplace(key, std::move(pipeline));
    return result;
}

bool create_slots(Renderer& r) {
    for (Slot& slot : r.slots) {
        slot.commands = nds_plume_context()->queue->createCommandList();
        slot.fence = device().createCommandFence();
        slot.vertices = device().createBuffer(RenderBufferDesc::VertexBuffer(
            sizeof(r.vertex_data), RenderHeapType::UPLOAD));
        slot.indices = device().createBuffer(RenderBufferDesc::IndexBuffer(
            sizeof(r.index_data), RenderHeapType::UPLOAD));
        slot.config = device().createBuffer(
            RenderBufferDesc::UploadBuffer(kConfigBytes, RenderBufferFlag::CONSTANT));
        slot.staging = device().createBuffer(
            RenderBufferDesc::UploadBuffer(kTexMemBytes + kTexPalBytes));
        slot.render_set = device().createDescriptorSet(render_set_desc());
        slot.final_set = device().createDescriptorSet(final_set_desc());
        if (!slot.commands || !slot.fence || !slot.vertices || !slot.indices ||
            !slot.config || !slot.staging || !slot.render_set || !slot.final_set)
            return false;
        slot.render_set->setTexture(0, r.tex_mem.get(), RenderTextureLayout::SHADER_READ);
        slot.render_set->setTexture(1, r.tex_pal.get(), RenderTextureLayout::SHADER_READ);
        slot.render_set->setBuffer(2, slot.config.get(), kConfigBytes);
        slot.final_set->setBuffer(2, slot.config.get(), kConfigBytes);
    }
    return true;
}

// Replaces the raster targets. Everything that may still sample the old
// output (the presenter's frames, this renderer's own) is drained first.
bool resize_targets(Renderer& r, uint32_t width, uint32_t height) {
    nds_plume_host_wait_idle();
    wait_all(r);
    r.valid = false;
    Targets& t = r.targets;
    t = Targets{};
    t.color = device().createTexture(RenderTextureDesc::ColorTarget(width, height, kColorFormat));
    t.attr = device().createTexture(RenderTextureDesc::ColorTarget(width, height, kAttrFormat));
    t.depth_copy = device().createTexture(
        RenderTextureDesc::ColorTarget(width, height, kDepthCopyFormat));
    t.depth = device().createTexture(RenderTextureDesc::DepthTarget(width, height, kDepthFormat));
    t.output = device().createTexture(
        RenderTextureDesc::ColorTarget(width, height, kOutputFormat));
    if (!t.color || !t.attr || !t.depth_copy || !t.depth || !t.output) {
        t = Targets{};
        return false;
    }
    const RenderTexture* main_colors[] = {t.color.get(), t.attr.get(), t.depth_copy.get()};
    t.main = device().createFramebuffer(RenderFramebufferDesc(main_colors, 3, t.depth.get()));
    const RenderTexture* final_colors[] = {t.output.get()};
    t.final = device().createFramebuffer(RenderFramebufferDesc(final_colors, 1));
    if (!t.main || !t.final) {
        t = Targets{};
        return false;
    }
    t.width = width;
    t.height = height;
    for (Slot& slot : r.slots) {
        slot.final_set->setTexture(0, t.color.get(), RenderTextureLayout::SHADER_READ);
        slot.final_set->setTexture(1, t.attr.get(), RenderTextureLayout::SHADER_READ);
        slot.final_set->setTexture(3, t.depth_copy.get(), RenderTextureLayout::SHADER_READ);
    }
    std::fprintf(stderr, "[plume3d] 3D raster %ux%u\n", width, height);
    return true;
}

// ── Polygon setup (GLRenderer::SetupPolygon / SetupVertex / BuildPolygons) ──

void setup_polygon(RendererPolygon* rp, const Polygon* polygon) {
    rp->poly = polygon;
    rp->render_key = (polygon->Attr >> 14) & 0x1;
    if (!polygon->IsShadowMask) {
        if (polygon->Translucent) {
            rp->render_key |= polygon->IsShadow ? 0x20000 : 0x10000;
            rp->render_key |= (polygon->Attr >> 10) & 0x2;
            rp->render_key |= (polygon->Attr >> 13) & 0x4;
            rp->render_key |= (polygon->Attr & 0x3F000000) >> 16;
            if ((polygon->Attr & 0x001F0000) == 0x001F0000) rp->render_key |= 0x4000;
        } else {
            if ((polygon->Attr & 0x001F0000) == 0) rp->render_key |= 0x2;
            rp->render_key |= (polygon->Attr & 0x3F000000) >> 16;
        }
    } else {
        rp->render_key |= 0x30000;
    }
}

struct Scale {
    float x, y;   // raster pixels per 1/16 DS pixel
};

GxVertex make_vertex(const Polygon* poly, int vid, const Vertex* vtx, uint32_t vtxattr,
                     Scale scale) {
    GxVertex v;
    v.x = float(vtx->HiresPosition[0]) * scale.x;
    v.y = float(vtx->HiresPosition[1]) * scale.y;
    v.z = uint32_t(poly->FinalZ[vid]);
    v.w = uint32_t(poly->FinalW[vid]);
    v.r = uint8_t(vtx->FinalColor[0] >> 1);
    v.g = uint8_t(vtx->FinalColor[1] >> 1);
    v.b = uint8_t(vtx->FinalColor[2] >> 1);
    v.a = uint8_t((poly->Attr >> 16) & 0x1F);
    v.s = vtx->TexCoords[0];
    v.t = vtx->TexCoords[1];
    v.attr = vtxattr;
    v.texparam = poly->TexParam;
    v.texpal = poly->TexPalette;
    return v;
}

void build_polygons(Renderer& r, Scale scale) {
    uint32_t vidx = 0, iidx = 0;
    auto push_vertex = [&](const GxVertex& v) { r.vertex_data[vidx++] = v; };
    auto push_triangle = [&](uint32_t a, uint32_t b, uint32_t c, RendererPolygon& rp) {
        r.index_data[iidx++] = a;
        r.index_data[iidx++] = b;
        r.index_data[iidx++] = c;
        rp.num_indices += 3;
    };
    // One DS pixel, in raster pixels: the thickness of a line polygon.
    const float pixel = scale.y * 16.0f;

    for (int i = 0; i < r.num_polys; ++i) {
        RendererPolygon& rp = r.polygons[i];
        const Polygon* poly = rp.poly;
        rp.indices_offset = iidx;
        rp.num_indices = 0;
        const uint32_t first = vidx;
        uint32_t vtxattr = poly->Attr & 0x1F00C8F0;
        if (poly->FacingView) vtxattr |= 1u << 8;
        if (poly->WBuffer) vtxattr |= 1u << 9;

        if (poly->Type == 1) {
            // A line: its first two distinct vertices, as a quad one DS pixel
            // thick around the segment.
            int ends[2] = {-1, -1};
            int found = 0;
            for (uint32_t j = 0; j < poly->NumVertices && found < 2; ++j) {
                const Vertex* vtx = poly->Vertices[j];
                if (found == 1 &&
                    vtx->FinalPosition[0] == poly->Vertices[ends[0]]->FinalPosition[0] &&
                    vtx->FinalPosition[1] == poly->Vertices[ends[0]]->FinalPosition[1])
                    continue;
                ends[found++] = int(j);
            }
            if (found < 2) continue;
            GxVertex a = make_vertex(poly, ends[0], poly->Vertices[ends[0]], vtxattr, scale);
            GxVertex b = make_vertex(poly, ends[1], poly->Vertices[ends[1]], vtxattr, scale);
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len <= 0.0f) continue;
            const float nx = -dy / len * pixel * 0.5f, ny = dx / len * pixel * 0.5f;
            GxVertex q[4] = {a, a, b, b};
            q[0].x += nx; q[0].y += ny;
            q[1].x -= nx; q[1].y -= ny;
            q[2].x -= nx; q[2].y -= ny;
            q[3].x += nx; q[3].y += ny;
            for (const GxVertex& v : q) push_vertex(v);
            push_triangle(first, first + 1, first + 2, rp);
            push_triangle(first, first + 2, first + 3, rp);
        } else if (poly->NumVertices == 3) {
            for (int j = 0; j < 3; ++j)
                push_vertex(make_vertex(poly, j, poly->Vertices[j], vtxattr, scale));
            push_triangle(first, first + 1, first + 2, rp);
        } else {
            // GLRenderer's "better polygons": a fan around a perspective
            // correct centre vertex, which melonDS enables above 1x.
            const uint32_t n = poly->NumVertices;
            float cx = 0, cy = 0, cz = 0, cw = 0, cr = 0, cg = 0, cb = 0, cs = 0, ct = 0;
            for (uint32_t j = 0; j < n; ++j) {
                const Vertex* vtx = poly->Vertices[j];
                cx += float(vtx->HiresPosition[0]);
                cy += float(vtx->HiresPosition[1]);
                const float fw = float(poly->FinalW[j]) * float(n);
                cw += 1.0f / fw;
                if (poly->WBuffer) cz += float(poly->FinalZ[j]) / fw;
                else cz += float(poly->FinalZ[j]);
                cr += float(vtx->FinalColor[0] >> 1) / fw;
                cg += float(vtx->FinalColor[1] >> 1) / fw;
                cb += float(vtx->FinalColor[2] >> 1) / fw;
                cs += float(vtx->TexCoords[0]) / fw;
                ct += float(vtx->TexCoords[1]) / fw;
            }
            cw = 1.0f / cw;
            if (poly->WBuffer) cz *= cw;
            else cz /= float(n);
            GxVertex c = make_vertex(poly, 0, poly->Vertices[0], vtxattr, scale);
            c.x = cx / float(n) * scale.x;
            c.y = cy / float(n) * scale.y;
            c.z = uint32_t(cz);
            c.w = uint32_t(cw);
            c.r = uint8_t(cr * cw);
            c.g = uint8_t(cg * cw);
            c.b = uint8_t(cb * cw);
            c.s = int16_t(cs * cw);
            c.t = int16_t(ct * cw);
            push_vertex(c);
            for (uint32_t j = 0; j < n; ++j) {
                push_vertex(make_vertex(poly, int(j), poly->Vertices[j], vtxattr, scale));
                if (j >= 1) push_triangle(first, vidx - 2, vidx - 1, rp);
            }
            push_triangle(first, vidx - 1, first + 1, rp);
        }
    }
    r.num_vertices = vidx;
    r.num_indices = iidx;
}

// ── The passes (GLRenderer::RenderSceneChunk) ───────────────────────────

class Scene {
public:
    Scene(Renderer& r, RenderCommandList& commands, bool wbuffer)
        : r_(r), commands_(commands) { s_.wbuffer = wbuffer; }

    DrawState& state() { return s_; }
    void program(Program p) { s_.program = p; }
    void depth_func(bool lequal) { s_.depth_lequal = lequal; }
    void depth_mask(bool write) { s_.depth_write = write; }
    void color_mask(bool write) { s_.color_write = write; }
    void attr_mask(bool r, bool g, bool b, bool a) {
        s_.attr_mask = uint8_t((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
    }
    void blend(bool enabled) { s_.blend = enabled; }
    void stencil_func(RenderComparisonFunction func, uint32_t ref, uint32_t mask) {
        s_.stencil_func = func;
        ref_ = ref & 0xFF;
        s_.read_mask = uint8_t(mask);
    }
    void stencil_op(RenderStencilOp fail, RenderStencilOp depth_fail, RenderStencilOp pass) {
        s_.fail_op = fail;
        s_.depth_fail_op = depth_fail;
        s_.pass_op = pass;
    }
    void stencil_mask(uint32_t mask) { s_.write_mask = uint8_t(mask); }

    int single(int i) {
        draw(r_.polygons[i].indices_offset, r_.polygons[i].num_indices);
        return 1;
    }

    int batch(int i) {
        const uint32_t key = r_.polygons[i].render_key;
        int count = 0;
        uint32_t indices = 0;
        for (int end = i; end < r_.num_polys && r_.polygons[end].render_key == key; ++end) {
            ++count;
            indices += r_.polygons[end].num_indices;
        }
        draw(r_.polygons[i].indices_offset, indices);
        return count;
    }

    // glClear(GL_STENCIL_BUFFER_BIT) under the current stencil write mask.
    void clear_stencil() {
        const DrawState saved = s_;
        const uint32_t saved_ref = ref_;
        s_.program = kNull;
        s_.depth_test = false;
        s_.depth_write = false;
        s_.color_write = false;
        s_.attr_mask = 0;
        s_.blend = false;
        s_.stencil_func = RenderComparisonFunction::ALWAYS;
        s_.fail_op = s_.depth_fail_op = s_.pass_op = RenderStencilOp::REPLACE;
        ref_ = 0;
        bind();
        commands_.drawInstanced(3, 1, 0, 0);
        s_ = saved;
        ref_ = saved_ref;
    }

    bool failed() const { return failed_; }

private:
    void bind() {
        RenderPipeline* pipeline = pipeline_for(r_, s_);
        if (!pipeline) { failed_ = true; return; }
        if (pipeline != bound_) {
            commands_.setPipeline(pipeline);
            bound_ = pipeline;
        }
        // Vulkan drops a dynamic reference when a pipeline is bound.
        commands_.setStencilReference(ref_);
    }

    void draw(uint32_t first, uint32_t count) {
        if (count == 0) return;
        bind();
        if (failed_) return;
        commands_.drawIndexedInstanced(count, 1, first, 0, 0);
    }

    Renderer& r_;
    RenderCommandList& commands_;
    DrawState s_;
    uint32_t ref_ = 0;
    const RenderPipeline* bound_ = nullptr;
    bool failed_ = false;
};

using Cmp = RenderComparisonFunction;
using Op = RenderStencilOp;

void render_scene(Renderer& r, Scene& gl, const melonDS::GPU3D& gpu3d) {
    const bool fog = (gpu3d.RenderDispCnt & (1 << 7)) != 0;

    // Pass 1: opaque pixels.
    gl.program(kOpaque);
    gl.attr_mask(true, true, fog, false);
    gl.depth_func(false);
    gl.depth_mask(true);
    for (int i = 0; i < r.num_polys;) {
        const RendererPolygon& rp = r.polygons[i];
        if (rp.poly->IsShadowMask || rp.poly->Translucent) { ++i; continue; }
        gl.depth_func((rp.poly->Attr & (1 << 14)) != 0);
        gl.stencil_func(Cmp::ALWAYS, (rp.poly->Attr >> 24) & 0x3F, 0xFF);
        gl.stencil_op(Op::KEEP, Op::KEEP, Op::REPLACE);
        gl.stencil_mask(0xFF);
        i += gl.batch(i);
    }

    gl.blend(true);
    gl.state().alpha_blend = (gpu3d.RenderDispCnt & (1 << 3)) != 0;

    if (r.num_opaque_polys > -1) {
        // Pass 2: translucent pixels against a clear plane of alpha zero
        // must be drawn with blending disabled.
        if ((gpu3d.RenderClearAttr1 & 0x001F0000) == 0) {
            gl.blend(false);
            for (int i = 0; i < r.num_polys;) {
                const RendererPolygon& rp = r.polygons[i];
                const uint32_t attr = rp.poly->Attr;
                if (rp.poly->IsShadowMask) {
                    gl.program(kShadow);
                    gl.blend(false);
                    gl.color_mask(false);
                    gl.attr_mask(false, false, false, false);
                    gl.depth_mask(false);
                    gl.depth_func(false);
                    gl.stencil_func(Cmp::EQUAL, 0xFF, 0xFF);
                    gl.stencil_op(Op::KEEP, Op::INVERT, Op::KEEP);
                    gl.stencil_mask(0x01);
                    i += gl.batch(i);
                } else if (rp.poly->Translucent) {
                    const bool need_opaque = (attr & 0x001F0000) == 0x001F0000;
                    const uint32_t polyid = (attr >> 24) & 0x3F;
                    gl.depth_func((attr & (1 << 14)) != 0);
                    if (need_opaque) {
                        gl.program(kOpaque);
                        gl.blend(false);
                        gl.color_mask(true);
                        gl.attr_mask(true, true, fog, false);
                        gl.stencil_func(Cmp::ALWAYS, polyid, 0xFF);
                        gl.stencil_op(Op::KEEP, Op::KEEP, Op::REPLACE);
                        gl.stencil_mask(0xFF);
                        gl.depth_mask(true);
                        gl.single(i);
                    }
                    gl.program(kTrans);
                    const bool trans_fog = !(attr & (1 << 15)) && fog;
                    if (rp.poly->IsShadow) {
                        // A shadow against the clear plane only passes when its
                        // polygon ID matches the clear plane's.
                        const uint32_t clear_id = (gpu3d.RenderClearAttr1 >> 24) & 0x3F;
                        if (polyid != clear_id) { ++i; continue; }
                        gl.blend(true);
                        gl.color_mask(true);
                        gl.attr_mask(false, false, trans_fog, false);
                        gl.stencil_func(Cmp::EQUAL, 0xFE, 0xFF);
                        gl.stencil_op(Op::KEEP, Op::KEEP, Op::INVERT);
                        gl.stencil_mask(~(0x40 | polyid));
                        gl.depth_mask((attr & (1 << 11)) != 0);
                        i += need_opaque ? gl.single(i) : gl.batch(i);
                    } else {
                        gl.color_mask(true);
                        gl.attr_mask(false, false, trans_fog, false);
                        gl.stencil_func(Cmp::EQUAL, 0xFF, 0xFE);
                        gl.stencil_op(Op::KEEP, Op::KEEP, Op::INVERT);
                        gl.stencil_mask(~(0x40 | polyid));
                        gl.depth_mask((attr & (1 << 11)) != 0);
                        i += need_opaque ? gl.single(i) : gl.batch(i);
                    }
                } else {
                    ++i;
                }
            }
            gl.blend(true);
            gl.stencil_mask(0xFF);
        }

        // Pass 3: translucent pixels.
        for (int i = 0; i < r.num_polys;) {
            const RendererPolygon& rp = r.polygons[i];
            const uint32_t attr = rp.poly->Attr;
            if (rp.poly->IsShadowMask) {
                gl.stencil_mask(0x80);
                gl.clear_stencil();
                gl.program(kShadow);
                gl.blend(false);
                gl.color_mask(false);
                gl.attr_mask(false, false, false, false);
                gl.depth_mask(false);
                gl.depth_func(false);
                gl.stencil_func(Cmp::ALWAYS, 0x80, 0x80);
                gl.stencil_op(Op::KEEP, Op::REPLACE, Op::KEEP);
                i += gl.batch(i);
            } else if (rp.poly->Translucent) {
                const bool need_opaque = (attr & 0x001F0000) == 0x001F0000;
                const uint32_t polyid = (attr >> 24) & 0x3F;
                gl.depth_func((attr & (1 << 14)) != 0);
                if (need_opaque) {
                    gl.program(kOpaque);
                    gl.blend(false);
                    gl.color_mask(true);
                    gl.attr_mask(true, true, fog, false);
                    gl.stencil_func(Cmp::ALWAYS, polyid, 0xFF);
                    gl.stencil_op(Op::KEEP, Op::KEEP, Op::REPLACE);
                    gl.stencil_mask(0xFF);
                    gl.depth_mask(true);
                    gl.single(i);
                }
                gl.program(kTrans);
                const bool trans_fog = !(attr & (1 << 15)) && fog;
                if (rp.poly->IsShadow) {
                    gl.blend(false);
                    gl.color_mask(false);
                    gl.attr_mask(false, false, false, false);
                    gl.depth_mask(false);
                    gl.stencil_func(Cmp::EQUAL, polyid, 0x3F);
                    gl.stencil_op(Op::KEEP, Op::KEEP, Op::ZERO);
                    gl.stencil_mask(0x80);
                    gl.single(i);

                    gl.blend(true);
                    gl.color_mask(true);
                    gl.attr_mask(false, false, trans_fog, false);
                    gl.stencil_func(Cmp::EQUAL, 0xC0 | polyid, 0x80);
                    gl.stencil_op(Op::KEEP, Op::KEEP, Op::REPLACE);
                    gl.stencil_mask(0x7F);
                    gl.depth_mask((attr & (1 << 11)) != 0);
                    i += gl.single(i);
                } else {
                    gl.blend(true);
                    gl.color_mask(true);
                    gl.attr_mask(false, false, trans_fog, false);
                    gl.stencil_func(Cmp::NOT_EQUAL, 0x40 | polyid, 0x7F);
                    gl.stencil_op(Op::KEEP, Op::KEEP, Op::REPLACE);
                    gl.stencil_mask(0x7F);
                    gl.depth_mask((attr & (1 << 11)) != 0);
                    i += need_opaque ? gl.single(i) : gl.batch(i);
                }
            } else {
                ++i;
            }
        }
    }
}

void fill_config(GxConfig& c, const melonDS::GPU3D& g, const Targets& t, uint32_t render_width) {
    std::memset(&c, 0, sizeof(c));
    c.screen_size[0] = float(t.width);
    c.screen_size[1] = float(t.height);
    c.disp_cnt = g.RenderDispCnt;
    c.alpha_ref = g.RenderAlphaRef;
    for (int i = 0; i < 32; ++i) c.toon[i] = g.RenderToonTable[i];
    for (int i = 0; i < 8; ++i) c.edge[i] = g.RenderEdgeTable[i];
    for (int i = 0; i < 34; ++i) c.fog_density[i] = g.RenderFogDensityTable[i];
    c.fog_color = g.RenderFogColor;
    c.fog_offset = g.RenderFogOffset;
    c.fog_shift = g.RenderFogShift;
    // One DS pixel: the outline width and the neighbour distance it tests.
    const float per_pixel = std::min(float(t.width) / float(render_width),
                                     float(t.height) / 192.0f);
    c.edge_distance = std::max(1u, uint32_t(std::lround(per_pixel)));
    c.clear_poly_id = (g.RenderClearAttr1 >> 24) & 0x3F;
    c.clear_depth = ((g.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
}

}  // namespace

bool nds_plume3d_start() {
    if (g_renderer) return true;
    if (!nds_plume_context()) return false;
    auto* r = new Renderer();
    auto fail = [&](const char* what) {
        std::fprintf(stderr, "[plume3d] %s failed\n", what);
        delete r;
        return false;
    };
    if (!create_shaders(*r)) return fail("shader creation");
    if (!create_layouts(*r)) return fail("pipeline layout creation");
    r->tex_mem = device().createTexture(
        RenderTextureDesc::Texture2D(1024, 512, 1, RenderFormat::R8_UINT));
    r->tex_pal = device().createTexture(
        RenderTextureDesc::Texture2D(1024, 64, 1, RenderFormat::R16_UINT));
    if (!r->tex_mem || !r->tex_pal) return fail("texture memory creation");
    if (!create_slots(*r)) return fail("frame slot creation");
    r->ready = true;
    g_renderer = r;
    std::fprintf(stderr, "[plume3d] 3D renderer ready (any raster size)\n");
    return true;
}

void nds_plume3d_stop() {
    if (!g_renderer) return;
    wait_all(*g_renderer);
    delete g_renderer;
    g_renderer = nullptr;
}

bool nds_plume3d_ready() {
    return g_renderer && g_renderer->ready;
}

void nds_plume3d_request_size(uint32_t width, uint32_t height) {
    if (!g_renderer) return;
    g_renderer->want_width = std::clamp(width, 64u, kMaxRasterWidth);
    g_renderer->want_height = std::clamp(height, 48u, kMaxRasterHeight);
}

void nds_plume3d_invalidate() {
    if (g_renderer) g_renderer->valid = false;
}

RenderTexture* nds_plume3d_output(uint32_t* width, uint32_t* height) {
    if (!g_renderer || !g_renderer->valid || !g_renderer->targets.output) return nullptr;
    if (width) *width = g_renderer->targets.width;
    if (height) *height = g_renderer->targets.height;
    return g_renderer->targets.output.get();
}

void nds_plume3d_render(const melonDS::GPU& gpu, Polygon* const* polygons, uint32_t count,
                        uint64_t texture_generation) {
    if (!g_renderer) return;
    Renderer& r = *g_renderer;
    const melonDS::GPU3D& g3 = gpu.GPU3D;
    // Clear-bitmap mode is not implemented by the polygon passes (melonDS GL
    // falls back to software for it too); the CPU composite carries it.
    if (g3.RenderDispCnt & (1u << 14)) { r.valid = false; return; }
    if (r.want_width == 0 || r.want_height == 0) return;
    if (r.targets.width != r.want_width || r.targets.height != r.want_height) {
        if (!resize_targets(r, r.want_width, r.want_height)) {
            std::fprintf(stderr, "[plume3d] %ux%u raster allocation failed\n",
                         r.want_width, r.want_height);
            r.want_width = r.want_height = 0;
            return;
        }
    }

    Slot& slot = r.slots[r.slot];
    wait_slot(slot);
    const Targets& t = r.targets;
    const uint32_t render_width = g3.GetRenderWidth();

    // Render list.
    r.num_polys = 0;
    r.num_opaque_polys = -1;
    for (uint32_t i = 0; i < count && i < 2048; ++i) {
        const Polygon* poly = polygons[i];
        if (poly->Degenerate) continue;
        setup_polygon(&r.polygons[r.num_polys], poly);
        if (r.num_opaque_polys < 0 && poly->Translucent) r.num_opaque_polys = r.num_polys;
        ++r.num_polys;
    }
    const Scale scale{float(t.width) / float(render_width) / 16.0f,
                      float(t.height) / 192.0f / 16.0f};
    build_polygons(r, scale);

    if (void* p = slot.vertices->map()) {
        std::memcpy(p, r.vertex_data, size_t(r.num_vertices) * sizeof(GxVertex));
        slot.vertices->unmap();
    }
    if (void* p = slot.indices->map()) {
        std::memcpy(p, r.index_data, size_t(r.num_indices) * sizeof(uint32_t));
        slot.indices->unmap();
    }
    GxConfig config;
    fill_config(config, g3, t, render_width);
    if (void* p = slot.config->map()) {
        std::memcpy(p, &config, sizeof(config));
        slot.config->unmap();
    }
    const bool upload = texture_generation != r.uploaded_generation;
    if (upload) {
        if (auto* p = static_cast<uint8_t*>(slot.staging->map())) {
            std::memcpy(p, gpu.VRAMFlat_Texture, kTexMemBytes);
            std::memcpy(p + kTexMemBytes, gpu.VRAMFlat_TexPal, kTexPalBytes);
            slot.staging->unmap();
        }
        r.uploaded_generation = texture_generation;
    }

    RenderCommandList& commands = *slot.commands;
    commands.begin();
    if (upload) {
        const RenderTextureBarrier to_copy[] = {
            RenderTextureBarrier(r.tex_mem.get(), RenderTextureLayout::COPY_DEST),
            RenderTextureBarrier(r.tex_pal.get(), RenderTextureLayout::COPY_DEST),
        };
        commands.barriers(RenderBarrierStage::COPY, to_copy, 2);
        commands.copyTextureRegion(
            RenderTextureCopyLocation::Subresource(r.tex_mem.get()),
            RenderTextureCopyLocation::PlacedFootprint(
                slot.staging.get(), RenderFormat::R8_UINT, 1024, 512, 1, 1024, 0));
        commands.copyTextureRegion(
            RenderTextureCopyLocation::Subresource(r.tex_pal.get()),
            RenderTextureCopyLocation::PlacedFootprint(
                slot.staging.get(), RenderFormat::R16_UINT, 1024, 64, 1, 1024, kTexMemBytes));
    }
    const RenderTextureBarrier to_draw[] = {
        RenderTextureBarrier(r.tex_mem.get(), RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(r.tex_pal.get(), RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(t.color.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(t.attr.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(t.depth_copy.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(t.depth.get(), RenderTextureLayout::DEPTH_WRITE),
    };
    commands.barriers(RenderBarrierStage::GRAPHICS, to_draw, uint32_t(std::size(to_draw)));
    commands.setFramebuffer(t.main.get());
    commands.setViewports(RenderViewport(0.0f, 0.0f, float(t.width), float(t.height)));
    commands.setScissors(RenderRect(0, 0, int32_t(t.width), int32_t(t.height)));

    // Clear plane (GLRenderer's clear shader, as attachment clears).
    {
        const uint32_t a1 = g3.RenderClearAttr1;
        const float cr = float(a1 & 0x1F) / 31.0f;
        const float cg = float((a1 >> 5) & 0x1F) / 31.0f;
        const float cb = float((a1 >> 10) & 0x1F) / 31.0f;
        const float ca = float((a1 >> 16) & 0x1F) / 31.0f;
        const float fog = float((a1 >> 15) & 1);
        const float polyid = float((a1 >> 24) & 0x3F) / 63.0f;
        const float depth = float(config.clear_depth) / 16777216.0f;
        commands.clearColor(0, RenderColor(cr, cg, cb, ca));
        commands.clearColor(1, RenderColor(polyid, 0.0f, fog, 1.0f));
        commands.clearColor(2, RenderColor(depth, 0.0f, 0.0f, 0.0f));
        commands.clearDepthStencil(true, true, depth, 0xFF);
    }

    bool failed = false;
    if (r.num_polys > 0) {
        commands.setGraphicsPipelineLayout(r.render_layout.get());
        commands.setGraphicsDescriptorSet(slot.render_set.get(), 0);
        static const RenderInputSlot input_slot(0, sizeof(GxVertex));
        const RenderVertexBufferView vertex_view(
            RenderBufferReference(slot.vertices.get()),
            uint32_t(r.num_vertices * sizeof(GxVertex)));
        commands.setVertexBuffers(0, &vertex_view, 1, &input_slot);
        const RenderIndexBufferView index_view(
            RenderBufferReference(slot.indices.get()),
            uint32_t(r.num_indices * sizeof(uint32_t)), RenderFormat::R32_UINT);
        commands.setIndexBuffer(&index_view);
        Scene scene(r, commands, r.polygons[0].poly->WBuffer);
        render_scene(r, scene, g3);
        failed = scene.failed();
    }

    // Final pass into the output the presenter samples.
    const RenderTextureBarrier to_final[] = {
        RenderTextureBarrier(t.color.get(), RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(t.attr.get(), RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(t.depth_copy.get(), RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(t.output.get(), RenderTextureLayout::COLOR_WRITE),
    };
    commands.barriers(RenderBarrierStage::GRAPHICS, to_final, uint32_t(std::size(to_final)));
    commands.setFramebuffer(t.final.get());
    commands.setViewports(RenderViewport(0.0f, 0.0f, float(t.width), float(t.height)));
    commands.setScissors(RenderRect(0, 0, int32_t(t.width), int32_t(t.height)));
    commands.setGraphicsPipelineLayout(r.final_layout.get());
    commands.setGraphicsDescriptorSet(slot.final_set.get(), 0);
    commands.setPipeline(r.final_pipeline.get());
    commands.drawInstanced(3, 1, 0, 0);
    commands.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(t.output.get(), RenderTextureLayout::SHADER_READ));
    commands.end();

    const RenderCommandList* submit = slot.commands.get();
    nds_plume_context()->queue->executeCommandLists(&submit, 1, nullptr, 0, nullptr, 0,
                                                    slot.fence.get());
    slot.submitted = true;
    r.slot = (r.slot + 1u) % kSlots;
    r.valid = !failed;
    if (++r.frames == 1 || r.frames % 3600 == 0)
        std::fprintf(stderr, "[plume3d] frame %llu: %d polygons, %u vertices, %zu pipelines%s\n",
                     static_cast<unsigned long long>(r.frames), r.num_polys, r.num_vertices,
                     r.pipelines.size(), failed ? ", a pipeline failed" : "");
}
