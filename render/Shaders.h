// Shaders.h - HLSL replacements for src/res/shaders/*.vert+*.frag (Phase 3).
//
// Scope note: the original renderer ships 8 GLSL 1.20 shader pairs selected
// per-material by Renderer::setupProgram()'s .prog condition matching
// (skinning/tangent/glow/parallax/emit-multi permutations of essentially one
// lit-textured surface shader). A lite viewer whose job is bind-pose mesh
// comparison (no skinning playback, see SceneBuilder.h's scope note) does
// not need per-permutation shader variants - one reasonably capable lit
// shader plus one unlit line shader (for the ported gltools.cpp grid/axes,
// phase3_tools) covers every mesh this parser hands the renderer.
//
// HLSL source is embedded as string literals rather than shipped as loose
// .hlsl files so the exe has no runtime dependency on a shaders/ directory
// next to it; D3DCompiler (see D3D11Renderer.cpp) compiles them once at
// startup via D3DCompile.
#pragma once

namespace nsk
{

// Vertex layout: POSITION(float3) + NORMAL(float3) + TEXCOORD(float2) + COLOR(float4).
// Matches the interleaved buffer D3D11Renderer builds from NifGeometry's
// structure-of-arrays (positions/normals/uvs/colors), defaulting missing
// channels to (0,0,1)/(0,0)/(1,1,1,1) respectively.
// row_major matches Matrix4's plain row-major float[4][4] C++ layout
// (see NifTypes.h Matrix4::data()/toColumnMajor()) exactly, so matrices are
// uploaded as-is with no CPU-side transpose. Combined with mul(matrix,
// vector) below (not mul(vector, matrix)), this reproduces the same
// M*v column-vector convention Camera.h's viewMatrix()/projectionMatrix()
// and Transform::toMatrix4() already use on the CPU side.
inline const char* kLitShaderHLSL = R"(
cbuffer PerFrame : register(b0)
{
    row_major float4x4 gViewProj;
    float3   gLightDir;   // world-space, pointing FROM the surface TOWARD the light
    float    gAmbient;
    float3   gEyePos;
    float    gBrightness;
};

cbuffer PerObject : register(b1)
{
    row_major float4x4 gWorld;
    float4   gTintColor;   // emissive tint (rgb) + alpha (a) from BSLightingShaderProperty/NiMaterialProperty
    float4   gSpecColor;   // specular color (rgb), specular power placeholder (a, unused by this simple shader)
    int      gHasTexture;
    float3   gPad0;
};

Texture2D    gDiffuseTex : register(t0);
SamplerState gSampler    : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 normalWS   : NORMAL;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR0;
    float3 positionWS : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(gWorld, float4(input.position, 1.0f));
    output.positionWS = worldPos.xyz;
    output.positionCS = mul(gViewProj, worldPos);
    output.normalWS = normalize(mul(gWorld, float4(input.normal, 0.0f)).xyz);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normalWS);
    float ndotl = max(dot(n, normalize(gLightDir)), 0.0f);
    float diffuse = gAmbient + (1.0f - gAmbient) * ndotl;

    float3 viewDir = normalize(gEyePos - input.positionWS);
    float3 halfVec = normalize(normalize(gLightDir) + viewDir);
    float spec = pow(max(dot(n, halfVec), 0.0f), 32.0f) * ndotl;

    float4 baseColor = float4(1, 1, 1, 1);
    if (gHasTexture != 0)
        baseColor = gDiffuseTex.Sample(gSampler, input.uv);
    baseColor *= input.color;

    float3 lit = baseColor.rgb * diffuse * gBrightness + gTintColor.rgb + gSpecColor.rgb * spec;
    float alpha = baseColor.a * gTintColor.a;
    return float4(lit, alpha);
}
)";

// Unlit, per-vertex-color line/triangle shader for the gltools.cpp port
// (grid/axes/bounding box - phase3_tools). Vertex layout: POSITION(float3) +
// COLOR(float4).
inline const char* kUnlitShaderHLSL = R"(
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
)";

} // namespace nsk
