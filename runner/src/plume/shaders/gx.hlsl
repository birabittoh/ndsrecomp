// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// The DS 3D engine on Plume: the polygon passes and the final pass, HLSL
// ports of melonDS's GPU3D_OpenGL_shaders.h (GPL-3.0-or-later, melonDS team)
// compiled ahead of time. One file, several entry points, so the polygon
// shaders and the final pass share GxConfig without an include that the
// shader build could not track.
//
// The raster has no fixed relation to the DS screen: vertices arrive in
// raster pixels (scaled from melonDS's 1/16 pixel HiresPosition by any real
// factor), and every pass works per raster pixel. Nothing assumes a whole
// multiple of 256x192.
//
// Render targets (all raster sized):
//   0 colour, RGBA8_UNORM, natural RGBA order (the GL renderer stores BGRA)
//   1 attributes, RGBA8_UNORM: r opaque polygon ID / 63, g opaque edge flag,
//     b fog flag, a 1
//   2 depth copy, R32_FLOAT, written wherever depth is, read by the final
//     pass (the depth/stencil target itself is never sampled)

struct GxConfig {
    float2 screenSize;   // raster size in pixels
    uint dispCnt;        // DISP3DCNT as latched for this frame
    uint alphaRef;       // RenderAlphaRef, 0..31
    uint4 toon[8];       // 32 BGR555 toon/highlight colours
    uint4 edge[2];       // 8 BGR555 edge colours
    uint4 fogDensity[9]; // the 34 entry density table, 0..127
    uint fogColor;       // RenderFogColor (BGR555 | alpha << 16)
    uint fogOffset;      // RenderFogOffset, already * 0x200
    uint fogShift;
    uint edgeDistance;   // outline width in raster pixels, at least 1
    uint clearPolyId;    // the clear plane's, for the screen border
    uint clearDepth;     // 24 bit
    uint2 pad;
};

[[vk::binding(2, 0)]] ConstantBuffer<GxConfig> config : register(b2);

uint FogDensity(uint index) {
    return config.fogDensity[index >> 2][index & 3];
}

uint EdgeColor(uint index) {
    return config.edge[index >> 2][index & 3];
}

float3 Bgr555(uint c) {
    return float3(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u)) / 31.0;
}

float3 ToonColor(int index) {
    const uint i = uint(clamp(index, 0, 31));
    return Bgr555(config.toon[i >> 2][i & 3]);
}

// ── Polygon passes ──────────────────────────────────────────────────────

[[vk::binding(0, 0)]] Texture2D<uint> texMem : register(t0);    // 1024x512 R8
[[vk::binding(1, 0)]] Texture2D<uint> texPal : register(t1);    // 1024x64 R16

// Plume maps no two-channel 32 bit integer format on Vulkan, so the integer
// words travel as one four-channel attribute.
struct VSInput {
    [[vk::location(0)]] float2 position : POSITION;   // raster pixels
    [[vk::location(1)]] uint4 words : TEXCOORD0;      // FinalZ, FinalW, polygon attr, TexParam
    [[vk::location(2)]] uint4 color : COLOR0;         // RGB 0..255, alpha 0..31
    [[vk::location(3)]] int2 texcoord : TEXCOORD1;    // 1/16 texel
    [[vk::location(4)]] uint texpal : TEXCOORD2;      // TexPalette
    [[vk::location(5)]] float4 edge : TEXCOORD3;      // zero along the outer edges
};

struct VSOutput {
    float4 position : SV_POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
    nointerpolation uint3 attr : TEXCOORD1;
    float z : TEXCOORD2;   // W-buffer depth, interpolated perspective correct
    noperspective float3 edge : TEXCOORD3;
};

VSOutput RenderVertex(VSInput input, bool wbuffer) {
    VSOutput output;
    const float w = float(input.words.y) / 65536.0;
    const float depth = float(input.words.x) / 16777216.0;
    // D3D clip space; the Vulkan Y flip is applied by -fvk-invert-y.
    const float2 ndc = float2(input.position.x / config.screenSize.x * 2.0 - 1.0,
                              1.0 - input.position.y / config.screenSize.y * 2.0);
    // Z-buffering interpolates depth linearly in screen space (z * w / w);
    // W-buffering writes the perspective-correct depth from the pixel shader.
    output.position = float4(ndc * w, wbuffer ? 0.0 : depth * w, w);
    output.color = float4(input.color) / float4(255.0, 255.0, 255.0, 31.0);
    output.texcoord = float2(input.texcoord) / 16.0;
    output.attr = uint3(input.words.zw, input.texpal);
    output.z = depth;
    output.edge = input.edge.xyz;
    return output;
}

VSOutput VSMainZ(VSInput input) { return RenderVertex(input, false); }
VSOutput VSMainW(VSInput input) { return RenderVertex(input, true); }

uint TexByte(int addr) {
    return texMem.Load(int3(addr & 0x3FF, addr >> 10, 0));
}

float4 PalColor(int addr) {
    const uint c = texPal.Load(int3(addr & 0x3FF, addr >> 10, 0));
    return float4(Bgr555(c), float(c >> 15));
}

int TexcoordWrap(int c, int maxc, int mode) {
    if ((mode & 1) != 0) {
        if ((mode & 4) != 0 && (c & maxc) != 0)
            return (maxc - 1) - (c & (maxc - 1));
        return c & (maxc - 1);
    }
    return clamp(c, 0, maxc - 1);
}

int2 WrapST(int4 st, int wrapmode) {
    return int2(TexcoordWrap(st.x, st.z, wrapmode), TexcoordWrap(st.y, st.w, wrapmode >> 1));
}

float4 FetchA3I5(int2 addr, int4 st, int wrapmode) {
    const int2 s = WrapST(st, wrapmode);
    const int pixel = int(TexByte(addr.x + s.y * st.z + s.x));
    int alpha = pixel & 0xE0;
    alpha = (alpha >> 3) + (alpha >> 6);
    const float4 color = PalColor((addr.y << 3) + (pixel & 0x1F));
    return float4(color.rgb, float(alpha) / 31.0);
}

float4 FetchI2(int2 addr, int4 st, int wrapmode, float alpha0) {
    const int2 s = WrapST(st, wrapmode);
    int pixel = int(TexByte(addr.x + ((s.y * st.z + s.x) >> 2)));
    pixel = (pixel >> (2 * (s.x & 3))) & 0x03;
    const float4 color = PalColor((addr.y << 2) + pixel);
    return float4(color.rgb, pixel > 0 ? 1.0 : alpha0);
}

float4 FetchI4(int2 addr, int4 st, int wrapmode, float alpha0) {
    const int2 s = WrapST(st, wrapmode);
    int pixel = int(TexByte(addr.x + ((s.y * st.z + s.x) >> 1)));
    pixel = (s.x & 1) != 0 ? (pixel >> 4) : (pixel & 0x0F);
    const float4 color = PalColor((addr.y << 3) + pixel);
    return float4(color.rgb, pixel > 0 ? 1.0 : alpha0);
}

float4 FetchI8(int2 addr, int4 st, int wrapmode, float alpha0) {
    const int2 s = WrapST(st, wrapmode);
    const int pixel = int(TexByte(addr.x + s.y * st.z + s.x));
    const float4 color = PalColor((addr.y << 3) + pixel);
    return float4(color.rgb, pixel > 0 ? 1.0 : alpha0);
}

float4 FetchCompressed(int2 addr, int4 st, int wrapmode) {
    const int2 s = WrapST(st, wrapmode);
    addr.x += ((s.y & 0x3FC) * (st.z >> 2)) + (s.x & 0x3FC) + (s.y & 0x3);
    const int val = (int(TexByte(addr.x)) >> (2 * (s.x & 0x3))) & 0x3;

    int slot1addr = 0x20000 + ((addr.x & 0x1FFFC) >> 1);
    if (addr.x >= 0x40000) slot1addr += 0x10000;
    int palinfo = int(TexByte(slot1addr)) | (int(TexByte(slot1addr + 1)) << 8);

    int pal = (addr.y << 3) + ((palinfo & 0x3FFF) << 1);
    palinfo >>= 14;

    if (val == 0) return float4(PalColor(pal).rgb, 1.0);
    if (val == 1) return float4(PalColor(pal + 1).rgb, 1.0);
    if (val == 2) {
        if (palinfo == 1)
            return float4((PalColor(pal).rgb + PalColor(pal + 1).rgb) / 2.0, 1.0);
        if (palinfo == 3)
            return float4((PalColor(pal).rgb * 5.0 + PalColor(pal + 1).rgb * 3.0) / 8.0, 1.0);
        return float4(PalColor(pal + 2).rgb, 1.0);
    }
    if (palinfo == 2) return float4(PalColor(pal + 3).rgb, 1.0);
    if (palinfo == 3)
        return float4((PalColor(pal).rgb * 3.0 + PalColor(pal + 1).rgb * 5.0) / 8.0, 1.0);
    return float4(0.0, 0.0, 0.0, 0.0);
}

float4 FetchA5I3(int2 addr, int4 st, int wrapmode) {
    const int2 s = WrapST(st, wrapmode);
    const int pixel = int(TexByte(addr.x + s.y * st.z + s.x));
    const float4 color = PalColor((addr.y << 3) + (pixel & 0x07));
    return float4(color.rgb, float((pixel & 0xF8) >> 3) / 31.0);
}

float4 FetchDirect(int2 addr, int4 st, int wrapmode) {
    const int2 s = WrapST(st, wrapmode);
    const int base = addr.x + ((s.y * st.z + s.x) << 1);
    const int lo = int(TexByte(base));
    const int hi = int(TexByte(base + 1));
    return float4(float(lo & 0x1F) / 31.0,
                  float((lo >> 5) | ((hi & 0x03) << 3)) / 31.0,
                  float((hi & 0x7C) >> 2) / 31.0,
                  float(hi >> 7));
}

float4 TextureLookup(float2 st, uint3 attr) {
    const int texparam = int(attr.y);
    const int paladdr = int(attr.z);
    const float alpha0 = (texparam & (1 << 29)) != 0 ? 0.0 : 1.0;
    const int tw = int(8) << ((texparam >> 20) & 0x7);
    const int th = int(8) << ((texparam >> 23) & 0x7);
    const int4 full = int4(int2(st), tw, th);
    const int2 vramaddr = int2((texparam & 0xFFFF) << 3, paladdr);
    const int wrapmode = texparam >> 16;
    switch ((texparam >> 26) & 0x7) {
        case 1: return FetchA3I5(vramaddr, full, wrapmode);
        case 2: return FetchI2(vramaddr, full, wrapmode, alpha0);
        case 3: return FetchI4(vramaddr, full, wrapmode, alpha0);
        case 4: return FetchI8(vramaddr, full, wrapmode, alpha0);
        case 5: return FetchCompressed(vramaddr, full, wrapmode);
        case 6: return FetchA5I3(vramaddr, full, wrapmode);
        default: return FetchDirect(vramaddr, full, wrapmode);
    }
}

float4 FinalColor(VSOutput input) {
    float4 vcol = input.color;
    const int polyattr = int(input.attr.x);
    const int blendmode = (polyattr >> 4) & 0x3;
    const bool highlight = (config.dispCnt & 2u) != 0u;
    if (blendmode == 2 && !highlight) vcol.rgb = ToonColor(int(vcol.r * 31.0));
    else if (blendmode == 2) vcol.rgb = vcol.rrr;

    float4 col;
    if (((input.attr.y >> 26) & 0x7u) == 0u || (config.dispCnt & 1u) == 0u) {
        col = vcol;
    } else {
        const float4 tcol = TextureLookup(input.texcoord, input.attr);
        if ((blendmode & 1) != 0) {
            col.rgb = tcol.rgb * tcol.a + vcol.rgb * (1.0 - tcol.a);   // decal
            col.a = vcol.a;
        } else {
            col = vcol * tcol;                                         // modulate
        }
    }
    if (blendmode == 2 && highlight)
        col.rgb = min(col.rgb + ToonColor(int(input.color.r * 31.0)), 1.0);
    return col;
}

// The hardware alpha test (GPU3D_Soft: alpha <= RenderAlphaRef is dropped).
bool AlphaTested(float alpha) {
    return uint(round(alpha * 31.0)) <= config.alphaRef;
}

struct PSOutput {
    float4 color : SV_Target0;
    float4 attr : SV_Target1;
    float depth : SV_Target2;
};

struct PSOutputW {
    float4 color : SV_Target0;
    float4 attr : SV_Target1;
    float depth : SV_Target2;
    float fragDepth : SV_Depth;
};

// The hardware's edge flags: whether the pixel lies on the polygon's outline,
// within one outline width (edgeDistance raster pixels) of an outer edge. The
// weights are affine in screen space, so weight over gradient is the distance
// in pixels; a weight that is zero throughout (a line polygon) is all edge.
// Texels inside the polygon that border its transparent holes are not edges,
// so a font quad's glyphs are not outlined.
bool OnOutline(float3 edge) {
    const float3 grad = sqrt(ddx(edge) * ddx(edge) + ddy(edge) * ddy(edge));
    const float width = float(max(config.edgeDistance, 1u));
    return any(edge <= 0.0) || any(edge < grad * width);
}

PSOutput Opaque(VSOutput input, float depth) {
    const bool outline = OnOutline(input.edge);
    const float4 col = FinalColor(input);
    if (col.a < 30.5 / 31.0 || AlphaTested(col.a)) discard;
    PSOutput output;
    output.color = col;
    output.attr = float4(float((input.attr.x >> 24) & 0x3Fu) / 63.0, outline ? 1.0 : 0.0,
                         float((input.attr.x >> 15) & 1u), 1.0);
    output.depth = depth;
    return output;
}

PSOutput Translucent(VSOutput input, float depth) {
    const float4 col = FinalColor(input);
    if (col.a < 0.5 / 31.0 || col.a >= 30.5 / 31.0 || AlphaTested(col.a)) discard;
    PSOutput output;
    output.color = col;
    // Only b is ever written (by the fog mask): clearing the fog flag where
    // the polygon has none, which ANDs it as the hardware does.
    output.attr = float4(0.0, 0.0, 0.0, 1.0);
    output.depth = depth;
    return output;
}

PSOutput ShadowMask(VSOutput input, float depth) {
    PSOutput output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.attr = float4(0.0, 0.0, 0.0, 1.0);
    output.depth = depth;
    return output;
}

PSOutputW WithDepth(PSOutput output, float depth) {
    PSOutputW result;
    result.color = output.color;
    result.attr = output.attr;
    result.depth = output.depth;
    result.fragDepth = depth;
    return result;
}

PSOutput PSOpaqueZ(VSOutput input) { return Opaque(input, input.position.z); }
PSOutput PSTransZ(VSOutput input) { return Translucent(input, input.position.z); }
PSOutput PSShadowZ(VSOutput input) { return ShadowMask(input, input.position.z); }
PSOutputW PSOpaqueW(VSOutput input) { return WithDepth(Opaque(input, input.z), input.z); }
PSOutputW PSTransW(VSOutput input) { return WithDepth(Translucent(input, input.z), input.z); }
PSOutputW PSShadowW(VSOutput input) { return WithDepth(ShadowMask(input, input.z), input.z); }

// Stencil-only fullscreen draws (clearing the shadow bit): no colour output.
void PSNull() {}

// ── Final pass: edge marking, fog, RGB6/alpha5 ──────────────────────────
//
// GPU3D_Soft's ScanlineFinalPass per raster pixel, in its integer domain, and
// the quantisation melonDS's NativeHiResFS applies. The output carries the
// DS's 6 bit colour and 5 bit alpha normalised by 63 and 31, so the
// presenter's round() recovers the exact integers.

[[vk::binding(0, 0)]] Texture2D<float4> colorBuffer : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> attrBuffer : register(t1);
[[vk::binding(3, 0)]] Texture2D<float> depthBuffer : register(t3);

struct FullscreenOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

uint DepthAt(int2 p) {
    return uint(depthBuffer.Load(int3(p, 0)) * 16777216.0);
}

// The soft renderer surrounds its buffers with a clear-plane border, so a
// polygon touching the screen edge is outlined there too.
bool EdgeAgainst(int2 p, uint polyid, uint depth) {
    const int2 size = int2(config.screenSize);
    uint otherId = config.clearPolyId;
    uint otherDepth = config.clearDepth;
    if (all(p >= int2(0, 0)) && all(p < size)) {
        otherId = uint(round(attrBuffer.Load(int3(p, 0)).r * 63.0));
        otherDepth = DepthAt(p);
    }
    return polyid != otherId && depth < otherDepth;
}

uint3 Rgb6(uint c) {
    uint3 v = uint3((c << 1) & 0x3Eu, (c >> 4) & 0x3Eu, (c >> 9) & 0x3Eu);
    return v + uint3(v != uint3(0u, 0u, 0u));
}

float4 PSFinal(FullscreenOutput input) : SV_Target {
    const int2 p = int2(input.position.xy);
    const uint4 bytes = uint4(round(saturate(colorBuffer.Load(int3(p, 0))) * 255.0));
    uint3 rgb = bytes.rgb >> 2u;
    uint alpha = bytes.a >> 3u;
    const float4 attr = attrBuffer.Load(int3(p, 0));
    const uint depth = DepthAt(p);

    if ((config.dispCnt & (1u << 5)) != 0u && attr.g > 0.5) {
        // Edge marking: an opaque pixel on its polygon's outline whose
        // neighbour one outline width away has another polygon ID and lies behind it.
        const uint polyid = uint(round(attr.r * 63.0));
        const int d = int(max(config.edgeDistance, 1u));
        if (EdgeAgainst(p + int2(-d, 0), polyid, depth) ||
            EdgeAgainst(p + int2(d, 0), polyid, depth) ||
            EdgeAgainst(p + int2(0, -d), polyid, depth) ||
            EdgeAgainst(p + int2(0, d), polyid, depth)) {
            const uint3 edge = Rgb6(EdgeColor(polyid >> 3));
            // Antialiasing breaks the edge pixel's coverage to half.
            rgb = (config.dispCnt & (1u << 4)) != 0u ? (rgb + edge + 1u) >> 1 : edge;
        }
    }

    if ((config.dispCnt & (1u << 7)) != 0u && attr.b > 0.5) {
        uint id = 0u, frac = 0u;
        if (depth >= config.fogOffset) {
            uint z = ((depth - config.fogOffset) >> 2) << config.fogShift;
            id = z >> 17;
            if (id >= 32u) id = 32u;
            else frac = z & 0x1FFFFu;
        }
        uint density = (FogDensity(id) * (0x20000u - frac) +
                        FogDensity(id + 1u) * frac) >> 17;
        if (density >= 127u) density = 128u;
        if ((config.dispCnt & (1u << 6)) == 0u)
            rgb = (Rgb6(config.fogColor) * density + rgb * (128u - density)) >> 7;
        alpha = (((config.fogColor >> 16) & 0x1Fu) * density + alpha * (128u - density)) >> 7;
    }

    return float4(float3(rgb) / 63.0, float(alpha) / 31.0);
}
