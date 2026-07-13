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
// Feature parity note: this now covers the full Skyrim/SE shader family -
// sk_default.frag (diffuse + vertex color + tangent-space normal map +
// specular with the normal map alpha as its mask + Own_Emit-gated emissive/
// glow map + environment cube map with env-mask-or-normal-alpha masking +
// back lighting + rim/soft lighting + height/parallax UV offset + skin/hair
// tint + UV scale/offset + filmic tonemap), sk_msn.frag (model-space
// normals with the G/B swizzle, external specular map, face detail/tint
// mask overlays), sk_multilayer.frag (inner-layer parallax with fresnel
// inner/outer mix and its own reflection strength) and sk_effectshader.frag
// (BSEffectShaderProperty: falloff, greyscale-to-palette color/alpha,
// weapon blood) - all folded into one HLSL ubershader branched on a feature
// bitmask, where NifSkope compiles them as separate GLSL programs selected
// by .prog condition files. FO4 (bsVersion 130) files parse their inline
// BSLightingShaderProperty defaults (Smoothness approximated as a specular
// exponent) and render through the same shader; the dedicated fo4_*.frag
// PBR variants and the BGSM/BGEM external material loader they really
// depend on (NifSkope's material.cpp) are not ported.
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
// Feature bitmask for PerObject gFlags - keep in sync with
// D3D11Renderer.cpp's kLit* constants.
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
//   2048  specular enabled (SLSF1 bit 0)
inline const char* kLitShaderHLSL = R"(
cbuffer PerFrame : register(b0)
{
    row_major float4x4 gViewProj;
    float3   gLightDir;   // world-space, pointing FROM the surface TOWARD the light
    float    gAmbient;    // A in sk_default.frag terms (light ambient, grayscale)
    float3   gEyePos;
    float    gBrightness; // D in sk_default.frag terms (light diffuse, grayscale)
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

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(gWorld, float4(input.position, 1.0f));
    output.positionWS = worldPos.xyz;
    output.positionCS = mul(gViewProj, worldPos);
    output.normalWS = normalize(mul(gWorld, float4(input.normal, 0.0f)).xyz);
    output.tangentWS = normalize(mul(gWorld, float4(input.tangent, 0.0f)).xyz);
    output.uv = input.uv * gUvTransform.xy + gUvTransform.zw;
    output.color = input.color;
    return output;
}

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

// Photoshop-style overlay blend, verbatim from sk_msn.frag's overlay()
// (face detail/tint mask compositing).
float OverlayCh(float base, float blend)
{
    return (base < 0.5f) ? (2.0f * base * blend)
                         : (1.0f - 2.0f * (1.0f - blend) * (1.0f - base));
}
float3 Overlay(float3 ba, float3 bl)
{
    return float3(OverlayCh(ba.r, bl.r), OverlayCh(ba.g, bl.g), OverlayCh(ba.b, bl.b));
}

// sk_multilayer.frag's ParallaxOffsetAndDepth, verbatim: inner layer's
// texture coordinate (xy) + transmission depth (z) from the tangent-space
// view vector and (sampled) tangent-space normal.
float3 ParallaxOffsetAndDepth(float2 texCoord, float2 innerScale, float3 viewTS, float3 normalTS, float layerThickness)
{
    float3 reflectionTS = reflect(-viewTS, normalTS);
    float3 transTS = float3(reflectionTS.xy, -reflectionTS.z);
    float transDist = layerThickness / max(abs(transTS.z), 1e-4f);
    float2 texelSize = float2(1.0f / (1024.0f * innerScale.x), 1.0f / (1024.0f * innerScale.y));
    float2 offsetCoord = texCoord + texelSize * transDist * transTS.xy;
    return float3(offsetCoord, transDist);
}

// Ports of NifSkope's Skyrim/SE fragment shaders, selected per material by
// gFlags the way Renderer::setupProgram's .prog condition matching selects
// GLSL programs: sk_effectshader.frag (flag 131072), and otherwise the lit
// family - sk_default.frag with the sk_msn.frag (model-space normals, flag
// 4096) and sk_multilayer.frag (flag 65536) variations folded in as
// branches, restructured for world-space lighting (the GLSL originals work
// in tangent space via a per-vertex TBN; same math, different frame).
// Original .frag line references in comments.
float4 PSMain(PSInput input) : SV_TARGET
{
    float3 nGeom = normalize(input.normalWS);
    float3 viewDir = normalize(gEyePos - input.positionWS);
    float3 lightDir = normalize(gLightDir);

    // --- sk_effectshader.frag --------------------------------------------
    if (gFlags & 131072)
    {
        float4 baseMap = gDiffuseTex.Sample(gSampler, input.uv);   // effect frag line 43

        // Falloff (effect frag lines 59-68): the GLSL's abs(E.b) is the
        // tangent-space view z = dot(view, normal) here.
        float falloff = 1.0f;
        if (gFlags & 262144)
        {
            falloff = smoothstep(gFalloffParams.y, gFalloffParams.x, abs(dot(viewDir, nGeom)));
            falloff = lerp(max(gFalloffParams.w, 0.0f), min(gFalloffParams.z, 1.0f), falloff);
        }

        float alphaMult = gGlowColor.a * gGlowColor.a;             // effect frag line 70

        float3 rgb = baseMap.rgb;
        float a = baseMap.a;
        if (gFlags & 2097152)                                      // weapon blood, lines 75-78
        {
            rgb = float3(1, 0, 0) * baseMap.r;
            a = baseMap.a * baseMap.g;
        }
        rgb *= input.color.rgb * gGlowColor.rgb;                    // line 80
        a *= input.color.a * falloff * alphaMult;                    // line 81

        if (gFlags & 524288)                                        // greyscale -> palette color, lines 83-90
        {
            float2 lu = float2(saturate(baseMap.g), saturate(input.color.g * falloff * gGlowColor.r));
            rgb = gAuxTex.Sample(gSampler, lu).rgb;
        }
        if (gFlags & 1048576)                                       // greyscale -> palette alpha, lines 92-96
        {
            float2 lu = float2(saturate(baseMap.a), saturate(input.color.a * falloff * alphaMult));
            a = gAuxTex.Sample(gSampler, lu).a;
        }

        if (gFlags & 4194304)                                        // NiAlphaProperty alpha test
            clip(a - gAlphaTest);
        return float4(rgb * gParams.x, a);                           // lines 98-99 (gParams.x = Base Color Scale)
    }

    // --- sk_default / sk_msn / sk_multilayer ------------------------------

    // Tangent frame (bitangent reconstructed - see NifGeometry::tangents).
    float3 t = normalize(input.tangentWS - nGeom * dot(input.tangentWS, nGeom));
    float3 b = cross(nGeom, t);
    float3 viewTS = float3(dot(viewDir, t), dot(viewDir, b), dot(viewDir, nGeom));

    // Height/parallax UV offset (sk_default lines 78-81).
    float2 uv = input.uv;
    if (gFlags & 512)
    {
        float height = gHeightTex.Sample(gSampler, uv).r;
        uv += viewTS.xy * (height * 0.08f - 0.04f);
    }

    float4 baseMap = (gFlags & 1) ? gDiffuseTex.Sample(gSampler, uv) : float4(1, 1, 1, 1);
    float4 normalMap = gNormalTex.Sample(gSampler, uv);

    // Normal resolution: tangent-space map (sk_default line 87), or
    // model-space map with the G/B swizzle (sk_msn lines 90-94; sk_msn's
    // view-matrix transform becomes a world transform in this frame).
    float3 n = nGeom;
    float3 tangentNormal = float3(0, 0, 1);
    float specMask = 1.0f;
    if (gFlags & 2)
    {
        if (gFlags & 4096)
        {
            float3 msn = normalMap.rgb * 2.0f - 1.0f;
            n = normalize(mul(gWorld, float4(msn.rbg, 0.0f)).xyz);
        }
        else
        {
            tangentNormal = normalize(normalMap.rgb * 2.0f - 1.0f);
            n = normalize(tangentNormal.x * t + tangentNormal.y * b + tangentNormal.z * nGeom);
        }
        specMask = normalMap.a;
    }
    // MSN external specular map (sk_msn lines 126-129): t7 holds it, unless
    // backlight claimed the slot (mutually exclusive by the renderer).
    if (gFlags & 8192)
        specMask = gBacklightTex.Sample(gSampler, uv).r;

    float NdotL = max(dot(n, lightDir), 0.0f);
    float EdotN = max(dot(n, viewDir), 0.0f);
    float NdotNegL = max(dot(n, -lightDir), 0.0f);
    float3 halfVec = normalize(lightDir + viewDir);
    float NdotH = max(dot(n, halfVec), 0.0f);

    // A/D: sk_* get these from the GL light; here they are the UI's
    // ambient/brightness settings as grayscale light colors.
    float3 A = gAmbient.xxx;
    float3 D = gBrightness.xxx;

    float3 diffuse = A + D * NdotL;

    // Albedo: multilayer's fresnel inner/outer mix (sk_multilayer lines
    // 105-144), or the plain base * vertex color of the other shaders.
    float3 albedo;
    if (gFlags & 65536)
    {
        float innerMapAlpha = gAuxTex.Sample(gSampler, uv).a;       // multilayer line 109
        float3 mixedNormal = lerp(float3(0, 0, 1), tangentNormal, saturate(gInnerParams.w)); // line 124
        float3 parallax = ParallaxOffsetAndDepth(uv, gInnerParams.xy, viewTS, mixedNormal,
            gInnerParams.z * innerMapAlpha);                          // line 125
        float3 inner = gAuxTex.Sample(gSampler, parallax.xy * gInnerParams.xy).rgb * input.color.rgb;
        float3 outer = baseMap.rgb * input.color.rgb;
        float outerMix = max(1.0f - EdotN, baseMap.a);                // line 143
        albedo = lerp(inner, outer, outerMix);
    }
    else
    {
        albedo = baseMap.rgb * input.color.rgb;                      // sk_default line 104
    }

    // Environment cube map (sk_default lines 109-122 / multilayer 148-160):
    // multilayer scales by its own outer reflection strength.
    if (gFlags & 128)
    {
        float reflScale = (gFlags & 65536) ? gTintColor.w : gParams.w;
        float3 reflectedWS = reflect(-viewDir, n);
        float3 cube = gCubeTex.Sample(gSampler, reflectedWS).rgb * reflScale;
        if (gFlags & 256)
            cube *= gEnvMaskTex.Sample(gSampler, uv).r;
        else
            cube *= normalMap.a;
        albedo += cube;
    }

    // Emissive & glow (sk_default lines 125-132), gated on SLSF1_Own_Emit.
    float3 emissive = float3(0, 0, 0);
    if (gFlags & 8)
    {
        emissive = gGlowColor.rgb;
        if (gFlags & 4)
            emissive *= gGlowTex.Sample(gSampler, uv).rgb;
    }

    // Specular (sk_default lines 135-136), gated on SLSF1_Specular.
    float3 spec = float3(0, 0, 0);
    if (gFlags & 2048)
    {
        float glossiness = max(gSpecColor.a, 1.0f);
        spec = saturate(gSpecColor.rgb * gParams.x * specMask * pow(NdotH, glossiness));
        spec *= D;
    }

    // Back lighting (sk_default lines 138-144).
    if (gFlags & 64)
    {
        float3 backlight = gBacklightTex.Sample(gSampler, uv).rgb * NdotNegL;
        emissive += backlight * D;
    }

    // Rim + soft lighting share the LightMask texture (sk_default lines 146-167).
    if (gFlags & (16 | 32))
    {
        float3 mask = gLightMaskTex.Sample(gSampler, uv).rgb;
        if (gFlags & 32)
        {
            float3 rim = mask * pow((1.0f - EdotN).xxx, gParams.zzz);
            rim *= smoothstep(-0.2f, 1.0f, dot(-lightDir, viewDir));
            emissive += rim * D;
        }
        if (gFlags & 16)
        {
            float wrap = (dot(n, lightDir) + gParams.y) / (1.0f + gParams.y);
            float3 soft = max(wrap, 0.0f) * mask * smoothstep(1.0f, 0.0f, NdotL);
            soft *= sqrt(saturate(gParams.y));
            emissive += soft * D;
        }
    }

    // Face detail/tint mask overlays (sk_msn lines 166-186).
    if (gFlags & 16384)
        albedo = Overlay(albedo, gHeightTex.Sample(gSampler, uv).rgb);
    if (gFlags & 32768)
        albedo = Overlay(albedo, gAuxTex.Sample(gSampler, uv).rgb);
    if (gFlags & 16384)
        albedo += albedo;

    // Skin/hair tint (sk_default lines 169-171).
    if (gFlags & 1024)
        albedo *= gTintColor.rgb;

    float3 color = albedo * (diffuse + emissive) + spec;
    color = Tonemap(color) / Tonemap(float3(1, 1, 1));

    float alpha = input.color.a * baseMap.a * gGlowColor.a;
    if (gFlags & 4194304)                       // NiAlphaProperty alpha test (GL_ALPHA_TEST equivalent)
        clip(alpha - gAlphaTest);
    return float4(color, alpha);
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
