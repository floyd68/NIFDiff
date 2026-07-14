// Lit.hlsl - single lit ubershader (VSMain/PSMain, vs_5_0/ps_5_0).
//
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
// HLSL is compiled at BUILD time by fxc (see the "Shader precompilation"
// section of the top-level CMakeLists.txt): each entry point becomes a
// generated header with a bytecode array that D3D11Renderer.cpp embeds
// directly, so the exe still has no runtime shaders/ dependency and pays
// no D3DCompile cost at startup (was ~260ms per viewport - StartupTrace).

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
TextureCube  gIblCube      : register(t9); // procedural sky/ground cube (D3D11Renderer::BuildIblCubemap): PBR ambient specular
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
    // Vertex-color channel toggle: whiten the rgb here so every consumer
    // downstream (legacy albedo, PBR albedo, effect shaders) is covered in
    // one place; vertex ALPHA is kept - it carries blending, not tint.
    output.color = (gFlags & 134217728) ? float4(1.0f, 1.0f, 1.0f, input.color.a) : input.color;
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

// Parallax occlusion mapping + height-field self-shadowing, implemented
// from the standard public technique (layered relief march with a linear
// refinement step; Tatarchuk, "Practical Parallax Occlusion Mapping For
// Highly Detailed Surface Rendering", GDC 2006, and the many tutorials
// derived from it). An earlier revision of these two functions was ported
// from Community Shaders' GPL-3.0 ExtendedMaterials.hlsli; this is an
// independent rewrite against the public technique - only the call-site
// contracts (heightInAlpha, the HeightScale slider semantics, dx/dy for
// the mip) carry over.
//
// The relief spans kParallaxDepth * scale in height units, CENTRED on the
// geometric surface (mid-grey height = zero offset, half raised / half
// recessed) - the way the Skyrim parallax ecosystem authors its _p maps
// and complex-material alpha heights, which also keeps the UI slider's
// existing calibration.
//
// The GPU-hang history here (farmhouse01.nif TDR'd real hardware -
// DXGI_ERROR_DEVICE_HUNG via GetDeviceRemovedReason(), reproduced in a
// Release build with no D3D debug layer involved) turned out to be a
// resource-valued ternary at ParallaxSoftShadow's call site (`cond ?
// gEnvMaskTex : gHeightTex`), not this march's cost or control flow -
// see the fix note at that call site below. [unroll] is kept anyway
// (bisection along the way confirmed a bounded march, unrolled or not,
// is cheap here) since it's strictly safer: no dynamic branch, no
// SampleGrad (illegal/costly under divergent control flow to begin with).
static const float kParallaxDepth = 0.1f;

float HeightMipFromGrad(float2 dx, float2 dy)
{
    float d = max(dot(dx, dx), dot(dy, dy));
    return (d > 0.0f) ? (0.5f * log2(d)) : 0.0f;
}

// Screen-space cotangent frame (Schuler, "Followup: Normal Mapping Without
// Precomputed Tangents"): derives T/B from the position and UV derivatives.
// The parallax ray uses THIS frame rather than the stored tangents: the
// vertex buffer keeps only the tangent (the file's bit-packed bitangent -
// and with it the handedness bit - is dropped, see NifGeometry::tangents),
// and cross(n, t) guesses the wrong sign on mirrored UV islands. A mirrored
// axis is fatal for parallax specifically: the march runs AWAY from the
// viewer and relief reads as texture swimming instead of depth. This frame
// follows the actual per-pixel UV mapping, so the march direction is always
// right regardless of how the mesh's tangents were authored.
float3x3 CotangentFrame(float3 N, float3 p, float2 uv)
{
    float3 dp1 = ddx(p);
    float3 dp2 = ddy(p);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12f));
    return float3x3(T * invmax, B * invmax, N);
}

float SampleHeight(Texture2D heightTex, float2 uv, bool heightInAlpha, float mip)
{
    float4 s = heightTex.SampleLevel(gSampler, uv, mip);
    return heightInAlpha ? s.a : s.r;
}

// Projects a normalized tangent-space direction onto the tangent plane,
// with the projection LENGTH clamped instead of the usual raw xy/z divide:
// this viewer's free-orbiting camera routinely drives silhouette/grazing
// pixels through z ~= 0, where an unbounded divide yields Inf/NaN offsets
// that poison the quad's ddx/ddy-derived mip state and read back as a
// black frame (confirmed via GetWinEvent: no display driver reset logged
// while reproducing this - not a TDR).
float2 ClampedTangentPlaneProjection(float3 dirTS)
{
    return dirTS.xy * min(rcp(max(dirTS.z, 1e-3f)), 3.0f);
}

float2 ParallaxOcclusionUV(float2 uv, float3 viewTS, Texture2D heightTex, bool heightInAlpha, float scale, float2 dx, float2 dy)
{
    float3 v = normalize(viewTS);
    float2 projDir = ClampedTangentPlaneProjection(v);

    const float amplitude = kParallaxDepth * scale; // full relief span, height units
    const float mip = HeightMipFromGrad(dx, dy);
    const float2 uvSpan = projDir * amplitude;      // UV travel across that span

    // March front-to-back in relief DEPTH d (0 = relief top, 1 = bottom).
    // The eye ray enters at d=0 half a span toward the viewer (centred
    // relief) and sinks one layer per tap; diff = surfaceDepth - rayDepth
    // flips negative at the first layer where the ray has passed below the
    // height field. Fixed 24 taps, fully unrolled, first hit latched via
    // branchless selects - no divergent early-out (see the block comment
    // above kParallaxDepth).
    const int kLayers = 24;
    const float dStep = 1.0f / (float)kLayers;

    float dHit = 1.0f;
    float diffBefore = 0.0f;
    float diffAfter = -1.0f;
    bool found = false;

    float prevDiff = 1.0f - SampleHeight(heightTex, uv + 0.5f * uvSpan, heightInAlpha, mip);
    [unroll]
    for (int i = 1; i <= kLayers; ++i)
    {
        float d = (float)i * dStep;
        float surfDepth = 1.0f - SampleHeight(heightTex, uv + (0.5f - d) * uvSpan, heightInAlpha, mip);
        float diff = surfDepth - d;

        bool hit = (diff <= 0.0f) && !found;
        dHit = hit ? d : dHit;
        diffAfter = hit ? diff : diffAfter;
        diffBefore = hit ? prevDiff : diffBefore;
        found = found || hit;

        prevDiff = diff;
    }

    // Classic final refinement: diff crosses zero between the last two
    // layers; place the intersection at the linear crossing point.
    float denom = diffAfter - diffBefore;
    float w = (denom == 0.0f) ? 0.0f : saturate(diffAfter / denom); // 0 = hit layer, 1 = layer before
    float dStar = found ? (dHit - w * dStep) : 1.0f;

    return uv + (0.5f - dStar) * uvSpan;
}

// Height-field self-shadowing, the usual POM companion: march from the
// shaded texel toward where the light ray exits the relief (the ray climbs
// from the local height h0 up to the relief top) and measure how far the
// terrain pokes above it. Blockers near the texel are weighted harder than
// distant ones, which reads as a soft penumbra instead of a hard edge.
// lightTS: tangent-space vector pointing FROM the surface TOWARD the light
// (same convention ParallaxOcclusionUV's viewTS uses for the eye).
float ParallaxSoftShadow(float2 uv, float3 lightTS, Texture2D heightTex, bool heightInAlpha, float scale, float2 dx, float2 dy)
{
    const float mip = HeightMipFromGrad(dx, dy);
    const float amplitude = kParallaxDepth * scale;

    float3 l = normalize(lightTS);
    float2 projDir = ClampedTangentPlaneProjection(l);

    float h0 = SampleHeight(heightTex, uv, heightInAlpha, mip);

    const int kTaps = 8;
    const float tStep = 1.0f / (float)kTaps;
    const float kHardness = 6.0f;

    float occlusion = 0.0f;
    [unroll]
    for (int i = 1; i <= kTaps; ++i)
    {
        float t = (float)i * tStep;
        float rayH = lerp(h0, 1.0f, t);
        float2 sampleUv = uv + projDir * ((rayH - h0) * amplitude);
        float hs = SampleHeight(heightTex, sampleUv, heightInAlpha, mip);
        occlusion = max(occlusion, (hs - rayH) * (1.0f - t));
    }
    return 1.0f - saturate(occlusion * kHardness);
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

    // UV-derivative frame for the parallax/shadow rays (see CotangentFrame).
    // Computed here in uniform control flow: gFlags branches are per-draw
    // uniform, so quad derivative state stays coherent below.
    float3x3 tbnUV = CotangentFrame(nGeom, input.positionWS, input.uv);
    float3 viewTSuv = mul(tbnUV, viewDir);
    float3 lightTSuv = mul(tbnUV, lightDir);

    // --- Community Shaders "True PBR" (flag 8388608) ----------------------
    // PBRNifPatcher-marked meshes (SLSF2_Unused01): t0 albedo, t1 normal
    // (no spec mask in alpha), t3 optional displacement, t5 RMAOS
    // (R=roughness, G=metallic, B=AO, A=specular level), t7 optional
    // subsurface color map. The repurposed material fields arrive in the
    // same cbuffer spots the vanilla path uses: gSpecColor.rgb = subsurface
    // color, gSpecColor.a = specular level, gParams.x = roughness scale,
    // gParams.y = subsurface opacity, gParams.z = displacement scale.
    // GGX/Cook-Torrance under the viewer's directional+ambient lights - a
    // credible preview, not CS-exact (no IBL / light lists / wetness).
    if (gFlags & 8388608)
    {
        float2 uvP = input.uv;
        float2 pdx = ddx(input.uv);
        float2 pdy = ddy(input.uv);
        float parallaxShadowP = 1.0f;
        if (gFlags & 512)
        {
            uvP = ParallaxOcclusionUV(uvP, viewTSuv, gHeightTex, false, gParams.z, pdx, pdy); // scale = displacement_scale
            if (lightTSuv.z > 0.0f)
                parallaxShadowP = ParallaxSoftShadow(uvP, normalize(lightTSuv), gHeightTex, false, gParams.z, pdx, pdy);
        }

        float4 base = (gFlags & 1) ? gDiffuseTex.Sample(gSampler, uvP) : float4(1, 1, 1, 1);
        // Bake compensation: PBR albedos carry no baked lighting, so on the
        // same content they sit well below the vanilla diffuse the legacy
        // path shows next door. Scaled here (not in the ambient) so direct
        // light benefits too; tuned against the High Hrothgar set side by
        // side with its vanilla mesh.
        const float kPBRBakeCompensation = 1.7f;
        float3 albedoP = base.rgb * input.color.rgb * kPBRBakeCompensation;

        // RMAOS default when unbound: rough dielectric with full AO.
        float4 rmaos = (gFlags & 256) ? gEnvMaskTex.Sample(gSampler, uvP) : float4(1, 0, 1, 1);
        float roughness = clamp(rmaos.r * gParams.x, 0.045f, 1.0f);
        float metallic = saturate(rmaos.g);
        float ao = rmaos.b;
        float specLevel = rmaos.a * gSpecColor.a;

        float3 nP = nGeom;
        if (gFlags & 2)
        {
            float3 tn = normalize(gNormalTex.Sample(gSampler, uvP).rgb * 2.0f - 1.0f);
            nP = normalize(tn.x * t + tn.y * b + tn.z * nGeom);
        }

        float NdotL = saturate(dot(nP, lightDir));
        float NdotV = saturate(dot(nP, viewDir)) + 1e-5f;
        float3 h = normalize(lightDir + viewDir);
        float NdotH = saturate(dot(nP, h));
        float VdotH = saturate(dot(viewDir, h));

        float3 F0 = lerp(specLevel.xxx, albedoP, metallic);
        float alphaR = roughness * roughness;
        float a2 = alphaR * alphaR;
        float dn = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
        float Dggx = a2 / max(3.14159265f * dn * dn, 1e-6f);
        float k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
        float G = (NdotV / (NdotV * (1.0f - k) + k)) * (NdotL / (NdotL * (1.0f - k) + k));
        float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);
        float3 specBRDF = Dggx * G * F / max(4.0f * NdotV * NdotL, 1e-4f);
        if ((gFlags & 2048) == 0) // specular channel toggle
            specBRDF = float3(0, 0, 0);

        // The diffuse term drops the 1/PI (and the specular keeps a matching
        // PI) so overall brightness lines up with the legacy path's
        // albedo * (A + D*NdotL) scale under the same UI light settings.
        // parallaxShadowP attenuates the directional term only (CS-style).
        float3 diffuseColor = albedoP * (1.0f - metallic);
        float3 colorP = (diffuseColor + specBRDF * 3.14159265f) * (NdotL * gBrightness.xxx * parallaxShadowP);

        // Viewer IBL stand-in: in-game CS lights PBR meshes with image-based
        // ambient the flat gAmbient can't represent, and PBR albedos carry
        // no baked lighting, so the plain diffuse*A*ao term reads several
        // stops darker than the legacy path on the same content. Compensate
        // with (1) a fixed ambient boost, (2) a sky/ground hemisphere tint
        // (world +Y up after the axis correction), and (3) Karis' env-BRDF
        // approximation as ambient specular, fed by the procedural sky/
        // ground cube on t9: roughness walks its pre-flattened mip chain
        // like a prefiltered radiance map, and Fenv's metal F0 = albedo
        // tints the reflection the way conductors color theirs. The cube's
        // sphere mean luminance is 1.0 (see BuildIblCubemap), so the
        // dielectric brightness calibration below it is unchanged.
        const float kPBRAmbientBoost = 1.6f;
        float3 ambientP = gAmbient.xxx * kPBRAmbientBoost * ao;
        float hemi = lerp(0.75f, 1.25f, saturate(nP.y * 0.5f + 0.5f));
        float3 Fenv = F0 + (max((1.0f - roughness).xxx, F0) - F0) * pow(1.0f - NdotV, 5.0f);
        float3 iblSpec = gIblCube.SampleLevel(gSampler, reflect(-viewDir, nP), roughness * 6.0f).rgb;
        colorP += diffuseColor * ambientP * hemi + Fenv * iblSpec * ambientP;

        if (gFlags & 8)
        {
            float3 emissiveP = gGlowColor.rgb;
            if (gFlags & 4)
                emissiveP *= gGlowTex.Sample(gSampler, uvP).rgb;
            colorP += albedoP * emissiveP;
        }
        if (gFlags & 16777216)
        {
            // Subsurface: light-wrapped tint through the _s.dds color map.
            float4 subs = gBacklightTex.Sample(gSampler, uvP);
            float wrap = saturate((dot(nP, lightDir) + 0.6f) / 1.6f);
            colorP += subs.rgb * gSpecColor.rgb * gParams.y * wrap * gBrightness.xxx;
        }

        if (gFlags & 268435456)
        {
            // Lighting channel off: the raw textured surface, without the
            // viewer's bake compensation or tonemapping.
            colorP = base.rgb * input.color.rgb;
        }
        else
        {
            colorP = Tonemap(colorP) / Tonemap(float3(1, 1, 1));
        }
        float alphaP = input.color.a * base.a * gGlowColor.a;
        if (gFlags & 4194304)
            clip(alphaP - gAlphaTest);
        return float4(colorP, alphaP);
    }

    // --- ENB / Community Shaders "complex material" detection ------------
    // A complex-material _m.dds is distinguished from a vanilla env mask by
    // the ecosystem's detection convention: the COARSEST mip's alpha is
    // meaningfully below 1 (vanilla masks decode to an opaque alpha). Its
    // channels are then R = reflection amount (as vanilla), G = glossiness,
    // B = metalness, A = parallax height - a published texture-format
    // convention (ENB / Community Shaders), not code from either.
    const float kCMEps = 4.0f / 255.0f;
    bool complexMaterial = false;
    if ((gFlags & 256) != 0 && (gFlags & 67108864) == 0) // env mask bound, CM toggle not off
        complexMaterial = gEnvMaskTex.SampleLevel(gSampler, input.uv, 15).a < 1.0f - kCMEps;

    // Height/parallax UV displacement: parallax occlusion mapping over the
    // _p.dds height map (see ParallaxOcclusionUV) instead of sk_default's
    // single-tap offset - the "complex parallax" treatment ENB/Community
    // Shaders give the same data. A complex material without a _p.dds
    // carries its height field in the env mask's alpha instead; a per-pixel
    // alpha at the extremes means "no height authored" (CS's same guard).
    float2 uv = input.uv;
    float2 uvDx = ddx(input.uv);
    float2 uvDy = ddy(input.uv);
    // HeightScale for vanilla/_m parallax, driven by the UI's "Parallax
    // Height" slider (default 2.0: Skyrim's _p maps were authored for ENB
    // setups that run per-texture height scales well above CS's 1.0
    // default). True PBR keeps its authored displacement_scale untouched
    // (see the PBR branch above).
    const float kViewerHeightScale = gParallaxScale;
    bool heightFromP = false;
    bool heightFromCM = false;
    if (gFlags & 512)
    {
        uv = ParallaxOcclusionUV(uv, viewTSuv, gHeightTex, false, kViewerHeightScale, uvDx, uvDy);
        heightFromP = true;
    }
    else if (complexMaterial && (gFlags & 33554432)) // CM alpha is a real height field (CPU probe)
    {
        float cmHeightProbe = gEnvMaskTex.Sample(gSampler, uv).a;
        if (cmHeightProbe > kCMEps && cmHeightProbe < 1.0f - kCMEps)
        {
            uv = ParallaxOcclusionUV(uv, viewTSuv, gEnvMaskTex, true, kViewerHeightScale, uvDx, uvDy);
            heightFromCM = true;
        }
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

    // Height-field self-shadow (see ParallaxSoftShadow): darkens mortar /
    // crevices opposite the light so displaced stones read as relief.
    float parallaxShadow = 1.0f;
    if (heightFromP || heightFromCM)
    {
        float3 lightTS = normalize(lightTSuv);
        if (lightTS.z > 0.0f)
        {
            // Two explicit calls instead of a `cond ? gEnvMaskTex : gHeightTex`
            // resource-valued ternary at the call site: that pattern is what
            // TDR'd the GPU here (confirmed by bisection - disabling only this
            // call, with ParallaxOcclusionUV's own two DIRECT (non-ternary)
            // per-branch calls left untouched, made the hang disappear).
            // Texture2D is an opaque resource handle, not a plain value;
            // FXC/the NVIDIA driver evidently mis-codegens selecting between
            // two different SRVs this way instead of duplicating the call
            // per branch the way the compiler is supposed to.
            if (heightFromCM)
                parallaxShadow = ParallaxSoftShadow(uv, lightTS, gEnvMaskTex, true, kViewerHeightScale, uvDx, uvDy);
            else
                parallaxShadow = ParallaxSoftShadow(uv, lightTS, gHeightTex, false, kViewerHeightScale, uvDx, uvDy);
        }
    }

    // A/D: sk_* get these from the GL light; here they are the UI's
    // ambient/brightness settings as grayscale light colors.
    float3 A = gAmbient.xxx;
    float3 D = gBrightness.xxx;

    float3 diffuse = A + D * NdotL * parallaxShadow;

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

    // Complex material channels (see detection above). The env mask is
    // sampled once here for both the reflection and specular blocks.
    float4 envMaskSample = float4(1, 1, 1, 1);
    if (gFlags & 256)
        envMaskSample = gEnvMaskTex.Sample(gSampler, uv);
    float cmGloss = complexMaterial ? envMaskSample.g : 0.0f;
    float cmMetal = complexMaterial ? envMaskSample.b : 0.0f;
    float3 cmBaseColor = albedo;

    // Metals reflect via the cube map rather than scattering diffusely -
    // pull the diffuse term down so a full-metal CM surface doesn't read
    // as chalky (rough energy conservation, not ENB/CS-exact).
    if (complexMaterial)
        albedo *= 1.0f - 0.6f * cmMetal;

    // Environment cube map (sk_default lines 109-122 / multilayer 148-160):
    // multilayer scales by its own outer reflection strength.
    if (gFlags & 128)
    {
        float reflScale = (gFlags & 65536) ? gTintColor.w : gParams.w;
        float3 reflectedWS = reflect(-viewDir, n);
        float3 cube;
        if (complexMaterial)
        {
            // Glossiness picks the reflection sharpness through the cube's
            // own mip chain; metalness tints the reflection with the base
            // color the way conductors color their reflections.
            cube = gCubeTex.SampleLevel(gSampler, reflectedWS, (1.0f - cmGloss) * 5.0f).rgb * reflScale;
            cube *= envMaskSample.r;
            cube *= lerp(float3(1, 1, 1), cmBaseColor, cmMetal);
        }
        else
        {
            cube = gCubeTex.Sample(gSampler, reflectedWS).rgb * reflScale;
            if (gFlags & 256)
                cube *= envMaskSample.r;
            else
                cube *= normalMap.a;
        }
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
        float mask = specMask;
        if (complexMaterial)
        {
            // CM: G is an inverse-roughness in [0,1], mapped onto a Blinn-
            // Phong exponent range; R doubles as the specular mask.
            glossiness = exp2(1.0f + cmGloss * 9.0f);
            mask = envMaskSample.r;
        }
        spec = saturate(gSpecColor.rgb * gParams.x * mask * pow(NdotH, glossiness));
        spec *= D * parallaxShadow;
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

    float3 color;
    if (gFlags & 268435456)
    {
        // Lighting channel off: the raw textured surface (albedo already
        // carries the base map, vertex color, env/cube and tint terms).
        color = albedo;
    }
    else
    {
        color = albedo * (diffuse + emissive) + spec;
        color = Tonemap(color) / Tonemap(float3(1, 1, 1));
    }

    float alpha = input.color.a * baseMap.a * gGlowColor.a;
    if (gFlags & 4194304)                       // NiAlphaProperty alpha test (GL_ALPHA_TEST equivalent)
        clip(alpha - gAlphaTest);
    return float4(color, alpha);
}
