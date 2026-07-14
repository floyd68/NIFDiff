// Highlight.hlsl - selection-highlight wireframe shader (VSMain/PSMain).
// Selection-highlight shader: re-draws one mesh's triangles in wireframe
// with a constant color over the shaded render (the NifSkope click-to-select
// wireframe overlay). Uses the same 5-element vertex layout as the lit
// shader (so the cached GpuMesh vertex buffer binds unchanged; only POSITION
// is consumed) and the unlit pass's matrix-only constant buffers.

cbuffer PerFrame : register(b0)
{
    row_major float4x4 gViewProj;
};

cbuffer PerObject : register(b1)
{
    row_major float4x4 gWorld;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float3 tangent  : TANGENT;
};

float4 VSMain(VSInput input) : SV_POSITION
{
    return mul(gViewProj, mul(gWorld, float4(input.position, 1.0f)));
}

float4 PSMain() : SV_TARGET
{
    return float4(0.35f, 1.0f, 0.45f, 1.0f); // selection wireframe color
}
