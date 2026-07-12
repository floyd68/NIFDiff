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
// Feature parity note: this covers res/shaders/sk_default.frag's core
// surface model (diffuse + vertex color + tangent-space normal map +
// specular color/strength/glossiness with the normal map's alpha channel as
// the specular mask, per Bethesda convention + glow/emissive map) but not
// its environment/cube map, backlight map, rim/soft-light (LightMask),
// height/parallax offset, or filmic tonemap - those are driven by
// BSLightingShaderProperty fields NifDocument.cpp does not parse yet (see
// its parseBSLightingShaderProperty scope note) and are lower-value for a
// static bind-pose comparison view than normal/specular/glow.
//
// HLSL source is embedded as string literals rather than shipped as loose
// .hlsl files so the exe has no runtime dependency on a shaders/ directory
// next to it; D3DCompiler (see D3D11Renderer.cpp) compiles them once at
// startup via D3DCompile.
#pragma once

namespace nsk
{

// Vertex layout: POSITION(float3) + NORMAL(float3) + TEXCOORD(float2) +
// COLOR(float4) + TANGENT(float3). Matches the interleaved buffer
// D3D11Renderer builds from NifGeometry's structure-of-arrays (positions/
// normals/uvs/colors/tangents), defaulting missing channels to
// (0,0,1)/(0,0)/(1,1,1,1)/(1,0,0) respectively (a missing tangent is only
// ever sampled when the material also has no normal map - see
// D3D11Renderer.cpp's GetOrCreateGpuMesh).
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
    float4   gTintColor;    // emissive/glow tint (rgb, already * Emissive Multiple) + alpha (a)
    float4   gSpecColor;    // specular color (rgb) + glossiness/specular power (a)
    float    gSpecStrength; // BSLightingShaderProperty's Specular Strength
    int      gHasTexture;
    int      gHasNormalMap;
    int      gHasGlowMap;
};

Texture2D    gDiffuseTex : register(t0);
Texture2D    gNormalTex  : register(t1);
Texture2D    gGlowTex    : register(t2);
SamplerState gSampler    : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float3 tangent  : TANGENT;
};

struct PSInput
{
    float4 positionCS : SV_POSITION;
    float3 normalWS   : NORMAL;
    float2 uv         : TEXCOORD0;
    float4 color      : COLOR0;
    float3 positionWS : TEXCOORD1;
    float3 tangentWS  : TANGENT;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(gWorld, float4(input.position, 1.0f));
    output.positionWS = worldPos.xyz;
    output.positionCS = mul(gViewProj, worldPos);
    output.normalWS = normalize(mul(gWorld, float4(input.normal, 0.0f)).xyz);
    output.tangentWS = normalize(mul(gWorld, float4(input.tangent, 0.0f)).xyz);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 n = normalize(input.normalWS);

    // Tangent-space normal mapping. The bitangent is reconstructed via
    // cross product rather than sampled from a stored per-vertex bitangent
    // (NifDocument.cpp only keeps the tangent - see NifGeometry::tangents'
    // comment on that tradeoff). specMask defaults to 1 (no attenuation)
    // when there is no normal map to sample a mask from.
    float specMask = 1.0f;
    if (gHasNormalMap != 0)
    {
        float3 t = normalize(input.tangentWS - n * dot(input.tangentWS, n)); // re-orthogonalize vs. interpolated normal
        float3 b = cross(n, t);
        float4 nmSample = gNormalTex.Sample(gSampler, input.uv);
        float3 tangentNormal = normalize(nmSample.rgb * 2.0f - 1.0f);
        n = normalize(tangentNormal.x * t + tangentNormal.y * b + tangentNormal.z * n);
        specMask = nmSample.a; // Bethesda convention: normal map alpha channel = specular mask
    }

    float ndotl = max(dot(n, normalize(gLightDir)), 0.0f);
    float diffuse = gAmbient + (1.0f - gAmbient) * ndotl;

    float3 viewDir = normalize(gEyePos - input.positionWS);
    float3 halfVec = normalize(normalize(gLightDir) + viewDir);
    float glossiness = max(gSpecColor.a, 1.0f);
    float spec = pow(max(dot(n, halfVec), 0.0f), glossiness) * ndotl * specMask * gSpecStrength;

    float4 baseColor = float4(1, 1, 1, 1);
    if (gHasTexture != 0)
        baseColor = gDiffuseTex.Sample(gSampler, input.uv);
    baseColor *= input.color;

    float3 glow = gTintColor.rgb;
    if (gHasGlowMap != 0)
        glow *= gGlowTex.Sample(gSampler, input.uv).rgb;

    float3 lit = baseColor.rgb * diffuse * gBrightness + glow + gSpecColor.rgb * spec;
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
