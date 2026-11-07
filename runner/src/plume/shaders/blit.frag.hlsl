// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// Resample a finished image into the viewport. The Plume port of kBlitFragment
// (src/melonds_compute/PresentShaders.h), used for the main screen, the
// single-screen inset and the frontend overlay.
//
// The sampler decides nearest or bilinear. kBoxResolve instead averages every
// source texel whose centre falls inside the output pixel's footprint: the
// supersample resolve for integer scaling, where the source is a whole
// multiple of the rect and the mean is an exact box filter. kKeepAlpha is the
// overlay, alpha blended by its pipeline; every other image is opaque by
// construction and its alpha byte means nothing, so the window gets 1.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

static const uint kBoxResolve = 1u;
static const uint kKeepAlpha = 2u;

[[vk::push_constant]]
struct Constants {
    uint flags;
    uint pad;
    int2 rectSize;
} constants;

float4 PSMain(VSOutput input) : SV_TARGET {
    float4 color;
    if ((constants.flags & kBoxResolve) == 0u) {
        color = sourceTexture.Sample(sourceSampler, input.uv);
    } else {
        uint width, height;
        sourceTexture.GetDimensions(width, height);
        const int2 srcSize = int2(width, height);
        const float2 ratio = float2(srcSize) / float2(constants.rectSize);
        const float2 p = floor(input.uv * float2(constants.rectSize));
        const int2 lo = int2(ceil(p * ratio - 0.5));
        int2 hi = max(int2(ceil((p + 1.0) * ratio - 0.5)), lo + 1);
        hi = min(min(hi, lo + 16), srcSize);
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        for (int y = lo.y; y < hi.y; ++y)
            for (int x = lo.x; x < hi.x; ++x)
                sum += sourceTexture.Load(int3(x, y, 0));
        color = sum / float((hi.x - lo.x) * (hi.y - lo.y));
    }
    if ((constants.flags & kKeepAlpha) == 0u)
        color.a = 1.0;
    return color;
}
