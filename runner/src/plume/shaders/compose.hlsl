// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// HD composite: the Plume port of kPresentFragment's hdMode path
// (src/melonds_compute/PresentShaders.h). The 2D compositor resolves the DS
// layer stack with the 3D layer removed and hands over the top two survivors
// per native pixel (NdsGpu2dHdFrame); this shader re-inserts the 3D layer
// from the Plume raster and finishes the DS blend in integer arithmetic.
//
// The 2D surfaces are native (width x 192) and the 3D raster is whatever
// size the host chose, with no whole-multiple relation between the two: both
// are addressed from the same uv, so a 5.625x raster composes exactly like a
// 6x one. RenderXPos is a native-pixel scroll, applied here in raster pixels.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> tex3d : register(t0);   // RGB6/A5 normalised
// Both HD layer surfaces in one RGBA32UI texture: top in xy, below in zw
// (Plume maps no RG32UI format on Vulkan).
[[vk::binding(1, 0)]] Texture2D<uint4> texLayers : register(t1);

[[vk::push_constant]]
struct Constants {
    uint outputWidth;
    uint renderXPos;
    uint bldcnt;
    uint masterBright;
    uint eva;
    uint evb;
    uint evy;
    uint priority3d;
    uint order3d;
    uint pad0;
    uint pad1;
    uint pad2;
} constants;

uint3 Blend16(uint3 a, uint3 b, uint ca, uint cb) {
    return min((a * ca + b * cb + 8u) >> 4, 63u);
}

uint3 Blend5(uint3 a, uint3 b, uint alpha) {
    const uint ca = alpha + 1u;
    if (ca == 32u) return a;
    return min((a * ca + b * (32u - ca) + 16u) >> 5, 63u);
}

uint3 Brighten(uint3 c, uint factor, uint bias) {
    return min(c + (((63u - c) * factor + bias) >> 4), 63u);
}

uint3 Darken(uint3 c, uint factor, uint bias) {
    return c - ((c * factor + bias) >> 4);
}

struct Layer {
    uint3 color;
    uint target;
    bool effects;
    uint alpha;    // 0 = no alpha blend, 1..16 = OBJ alpha, 255 = EVA/EVB
    uint alpha5;   // only the 3D layer: its own 5 bit coverage
    uint priority;
    uint order;
};

Layer UnpackLayer(uint2 texel) {
    Layer l;
    l.color = uint3(texel.x & 63u, (texel.x >> 8) & 63u, (texel.x >> 16) & 63u);
    l.target = texel.y & 63u;
    l.effects = (texel.y & 128u) != 0u;
    l.alpha = (texel.y >> 8) & 255u;
    l.alpha5 = 0u;
    l.priority = (texel.y >> 16) & 255u;
    l.order = (texel.y >> 24) & 255u;
    return l;
}

bool Ahead(Layer a, Layer b) {
    return a.priority < b.priority || (a.priority == b.priority && a.order < b.order);
}

// Mirrors compose() in gpu2d.cpp, including the 3D layer's forced per-pixel
// 5 bit blend whenever the pixel behind it is a second target.
uint3 ComposeLayers(Layer top, Layer below) {
    const uint bldcnt = constants.bldcnt;
    const uint target2 = below.target << 8;
    const uint mode = (bldcnt >> 6) & 3u;
    if (top.alpha5 != 0u) {
        if ((bldcnt & target2) != 0u) return Blend5(top.color, below.color, top.alpha5);
        if (top.effects && (bldcnt & top.target) != 0u) {
            if (mode == 2u) return Brighten(top.color, constants.evy, 8u);
            if (mode == 3u) return Darken(top.color, constants.evy, 7u);
        }
        return top.color;
    }
    if (top.alpha != 0u && (bldcnt & target2) != 0u) {
        const uint eva1 = top.alpha == 255u ? constants.eva : top.alpha;
        const uint evb1 = top.alpha == 255u ? constants.evb : 16u - eva1;
        return Blend16(top.color, below.color, eva1, evb1);
    }
    if (top.effects && (bldcnt & top.target) != 0u) {
        if (mode == 1u && (bldcnt & target2) != 0u)
            return Blend16(top.color, below.color, constants.eva, constants.evb);
        if (mode == 2u) return Brighten(top.color, constants.evy, 8u);
        if (mode == 3u) return Darken(top.color, constants.evy, 7u);
    }
    return top.color;
}

float4 PSMain(VSOutput input) : SV_TARGET {
    uint nativeW, nativeH, hiW, hiH;
    texLayers.GetDimensions(nativeW, nativeH);
    tex3d.GetDimensions(hiW, hiH);
    const float2 nativeSize = float2(nativeW, nativeH);
    const float2 hiSize = float2(hiW, hiH);
    const int2 dst = int2(clamp(input.uv * nativeSize, 0.0, nativeSize - 1.0));
    const int2 dstHi = int2(clamp(input.uv * hiSize, 0.0, hiSize - 1.0));

    // Raster pixels per native pixel, horizontally.
    const float ratio = hiSize.x / float(max(constants.outputWidth, 1u));
    int sx = dstHi.x;
    bool off = false;
    const uint xpos = constants.renderXPos;
    if (xpos != 0u) {
        if ((xpos & 0x100u) != 0u) {
            const int shift = int(round(float(min(int(constants.outputWidth), 512 - int(xpos))) * ratio));
            off = dstHi.x < shift;
            sx = dstHi.x - shift;
        } else {
            sx = dstHi.x + int(round(float(xpos) * ratio));
            off = sx >= int(hiW);
        }
    }

    const uint4 layers = texLayers.Load(int3(dst, 0));
    const uint2 topTexel = layers.xy;
    Layer top = UnpackLayer(topTexel);
    Layer below = UnpackLayer(layers.zw);
    if (!off && (topTexel.y & 64u) == 0u) {
        const float4 hi = tex3d.Load(int3(sx, dstHi.y, 0));
        const uint a5 = uint(round(hi.a * 31.0));
        if (a5 != 0u) {
            Layer l3;
            l3.color = uint3(round(hi.rgb * 63.0));
            l3.target = 1u;          // BG0 / 3D is BLDCNT first target 0
            l3.effects = top.effects;
            l3.alpha = 0u;
            l3.alpha5 = a5;
            l3.priority = constants.priority3d;
            l3.order = constants.order3d;
            // (priority, order) is a total order, so inserting last yields
            // the pair the CPU path picks.
            if (Ahead(l3, top)) { below = top; top = l3; }
            else if (Ahead(l3, below)) { below = l3; }
        }
    }
    uint3 c = ComposeLayers(top, below);
    const uint bm = constants.masterBright >> 14;
    const uint bf = min(constants.masterBright & 31u, 16u);
    if (bm == 1u) c = Brighten(c, bf, 0u);
    else if (bm == 2u) c = Darken(c, bf, 15u);
    return float4(float3(c) / 63.0, 1.0);
}
