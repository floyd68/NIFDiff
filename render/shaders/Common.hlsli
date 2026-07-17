// Common.hlsli - the shared "contract" for NIFDiff mesh shaders.
//
// Everything a user-written mesh shader needs to interoperate with the
// renderer lives here: the PerFrame/PerObject constant buffers RenderDevice
// uploads, the bound texture/sampler registers, the vertex input layout and
// the VS->PS interpolants, plus the Tonemap helper the stock shader ends
// with. Custom shaders (see README's "User-editable shaders") should start
// with:
//
//     #include "Common.hlsli"
//
// and implement VSMain/PSMain against these declarations. Do NOT change the
// register assignments or cbuffer layouts here - they mirror RenderDevice's
// C++ structs field for field.

// Vertex layout: POSITION(float3) + NORMAL(float3) + TEXCOORD(float2) +
// COLOR(float4) + TANGENT(float3). Matches the interleaved buffer
// D3D11Renderer builds from NifGeometry's structure-of-arrays (positions/
// normals/uvs/colors/tangents), defaulting missing channels to
// (0,0,1)/(0,0)/(1,1,1,1)/(1,0,0) respectively (a missing tangent is only
// ever sampled when the material also has no normal map - see
// RenderDevice.cpp's GetOrCreateGpuMesh).
// row_major matches Matrix4's plain row-major float[4][4] C++ layout
// (see NifTypes.h Matrix4::data()/toColumnMajor()) exactly, so matrices are
// uploaded as-is with no CPU-side transpose. Combined with mul(matrix,
// vector) below (not mul(vector, matrix)), this reproduces the same
// M*v column-vector convention Camera.h's viewMatrix()/projectionMatrix()
// and Transform::toMatrix4() already use on the CPU side.
// Feature bitmask for PerObject gFlags - keep in sync with
// RenderDevice.cpp's kLit* constants.
//   1     diffuse texture bound         4096    model-space normals (sk_msn path)
//   2     normal map bound              8192    t7 holds the MSN specular map (not backlight)
//   4     glow map (ST 2 + SLSF2:6)     16384   face detail mask in t3 (ST 4)
//   8     own-emit (SLSF1 bit 22)       32768   face tint mask in t8 (ST 4)
//   16    soft lighting (SLSF2:25)      65536   multilayer parallax, inner map in t8 (ST 11)
//   32    rim lighting (SLSF2:26)       131072  BSEffectShaderProperty (sk_effectshader path)
//   64    backlight map (SLSF2:27)      262144  effect: use falloff
//   128   environment cube map bound    524288  effect: greyscale-to-palette color (palette in t8)
//   256   env mask texture bound        1048576 effect: greyscale-to-palette alpha
//   512   height/parallax map           2097152 effect: weapon blood
//   1024  skin/hair tint color         4194304 alpha test (NiAlphaProperty bit 9) - clip vs gAlphaTest
//   2048  specular enabled (SLSF1 bit 0) 8388608 Community Shaders True PBR path (slots reinterpreted)
//                                        16777216 PBR: t7 holds the subsurface color map
//                                        33554432 complex material alpha is a real height field (CM parallax on)
//                                        67108864 UI toggle: treat complex materials as vanilla env masks
//                                        134217728 UI channel: ignore vertex colors (rgb whitened in VS, alpha kept)
//                                        268435456 UI channel: lighting off - raw textured surface
cbuffer PerFrame : register(b0)
{
    row_major float4x4 gViewProj;
    float3   gLightDir;   // world-space, pointing FROM the surface TOWARD the light
    float    gAmbient;    // A in sk_default.frag terms (light ambient, grayscale)
    float3   gEyePos;
    float    gBrightness; // D in sk_default.frag terms (light diffuse, grayscale)
    float    gParallaxScale; // vanilla/_m parallax HeightScale (UI slider; PBR keeps its authored displacement_scale)
    float3   gPadFrame;
};

cbuffer PerObject : register(b1)
{
    row_major float4x4 gWorld;
    float4   gGlowColor;     // lighting: emissive * Emissive Multiple (rgb) + material Alpha (a)
                             // effect:   Base Color (rgba)
    float4   gSpecColor;     // specular color (rgb) + glossiness/specular power (a)
    float4   gUvTransform;   // UV Scale (xy) + UV Offset (zw)
    float4   gTintColor;     // Skin/Hair Tint Color (rgb) + multilayer outer reflection strength (w)
    float4   gParams;        // x = Specular Strength (lighting) / Base Color Scale (effect),
                             // y = Lighting Effect 1 (soft wrap), z = Lighting Effect 2 (rim power),
                             // w = Environment Map Scale
    float4   gInnerParams;   // multilayer: Inner Texture Scale (xy), Inner Thickness (z), Outer Refraction (w)
    float4   gFalloffParams; // effect: Falloff Start Angle, Stop Angle, Start Opacity, Stop Opacity
    uint     gFlags;         // feature bitmask - see table above
    float    gAlphaTest;     // alpha-test threshold (NiAlphaProperty Threshold / 255), used when flag 4194304 is set
    float2   gPad;
};

Texture2D    gDiffuseTex   : register(t0);
Texture2D    gNormalTex    : register(t1);
Texture2D    gGlowTex      : register(t2);
Texture2D    gHeightTex    : register(t3); // height map OR face detail mask
TextureCube  gCubeTex      : register(t4);
Texture2D    gEnvMaskTex   : register(t5);
Texture2D    gLightMaskTex : register(t6);
Texture2D    gBacklightTex : register(t7); // backlight map OR MSN specular map
Texture2D    gAuxTex       : register(t8); // face tint mask / multilayer inner map / effect greyscale palette
TextureCube  gIblCube      : register(t9); // procedural sky/ground cube (RenderDevice::BuildIblCubemap): PBR ambient specular
SamplerState gSampler      : register(s0);

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

// Uncharted-2-style filmic operator, verbatim from sk_default.frag's
// tonemap() (it normalizes by tonemap(1) at the call site, ditto here).
float3 Tonemap(float3 x)
{
    float A = 0.15f;
    float B = 0.50f;
    float C = 0.10f;
    float D = 0.20f;
    float E = 0.02f;
    float F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
