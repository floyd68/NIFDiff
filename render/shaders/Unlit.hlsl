// Unlit.hlsl - unlit per-vertex-color line/triangle shader (VSMain/PSMain).
// Unlit, per-vertex-color line/triangle shader for the gltools.cpp port
// (grid/axes/bounding box - phase3_tools). Vertex layout: POSITION(float3) +
// COLOR(float4).

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
    float4 color    : COLOR0;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float4 color      : COLOR0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(gWorld, float4(input.position, 1.0f));
    output.positionCS = mul(gViewProj, worldPos);
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
