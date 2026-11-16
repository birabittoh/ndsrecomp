// GPL-3.0-or-later; see THIRD_PARTY_ATTRIBUTION.md.
//
// One oversized triangle covering clip space, generated from SV_VertexID, so
// no pass needs a vertex buffer or an input layout. The destination rect is
// the viewport the draw is issued under: letterbox, stretch, integer and the
// single-screen inset are all this same draw.
//
// Written for the D3D clip space convention; DXC is invoked with
// -fvk-invert-y for vertex shaders, which applies the Vulkan flip. uv (0, 0)
// is the top-left texel on both APIs, so every image here is top row first,
// exactly as the CPU frames are laid out (no GL-style flips anywhere).

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.uv = float2(float((id << 1) & 2), float(id & 2));
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
