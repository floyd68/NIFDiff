// NifDocument.h - Qt-free replacement for src/model/basemodel.h + src/model/nifmodel.h
// and (in spirit) src/xml/nifxml.cpp.
//
// ============================================================================
// IMPORTANT SCOPE NOTE (read this before extending block support)
// ============================================================================
// The original NifModel/BaseModel pair is driven generically at runtime by
// nif.xml (parsed by xml/nifxml.cpp's QXmlDefaultHandler): every block/
// compound/field, every version condition ("vercond"), and every dynamic
// array-length expression is data, not code, which is what lets NifSkope
// read *any* block type in *any* NIF version without being told about it
// in advance.
//
// This lite viewer does not ship that generic engine. nif.xml is not part of
// this repository (it lives in the upstream niftools/nifxml project and is
// fetched by the docsys submodule's build tooling, not checked in here), and
// hand-writing a correct, general-purpose XML-driven binary reflection
// interpreter (compound inheritance, generic/templated compounds, the
// NifExpr condition language, per-version field variants, etc.) is a
// multi-week project by itself - out of scope for turning NifSkope into a
// lightweight viewer.
//
// Instead, NifDocument direct-parses a curated set of block types straight
// into plain C++ structs below. The exact field order/sizes for each parsed
// block were derived by cross-referencing the real, current niftools/nifxml
// nif.xml (vendored for reference only, not compiled, at
// liteviewer/schema_reference/nif.xml) for the Skyrim LE (BS Version 83),
// Skyrim SE (BS Version 100) and Fallout 4 (BS Version 130) configurations
// specifically - see the .cpp for field-by-field citations. Other games/NIF
// versions are not handled.
//
// Robustness net: since NIF version >= 20.2.0.5 (which covers every
// Bethesda game we target) the file header stores an explicit per-block
// byte size table ("Block Size" array). NifDocument uses that table to seek
// directly to each block's start offset rather than trusting its own parse
// to consume the exact number of bytes a block occupies. This means: (a)
// completely unrecognised block types are safely skipped whole, and (b) a
// parser for a *recognised* type only needs to correctly decode the fields
// it actually reads, in order, starting from the front of the block - it
// never needs to consume a block to its end. Both properties keep the
// curated-parser approach safe even though it is not a general-purpose
// reader.
// ============================================================================
#pragma once

#include "NifTypes.h"
#include "NifItem.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>

namespace nsk
{

constexpr std::int32_t kNoRef = -1;

// A textured/tinted material resolved from BSLightingShaderProperty +
// BSShaderTextureSet (Skyrim/SE/FO4) or NiMaterialProperty (legacy path).
//
// The has*/is* booleans below are DERIVED state, computed in
// buildHierarchyAndRoots() once the referenced BSShaderTextureSet has been
// resolved (several of them require knowing whether a texture slot is
// non-empty, and the set block may appear after the property block in file
// order) - mirroring NifSkope's BSLightingShaderProperty::updateParams()
// (src/gl/glproperty.cpp lines 1141-1256, the Skyrim/SE stream<130 branch).
struct NifMaterial
{
    // BSShaderTextureSet slots (backslashes normalized to '/'). Several
    // slots are dual-purpose per NifSkope: slot 2 is the glow map when
    // hasGlowMap, otherwise the rim/soft-light LightMask; slot 3 is the
    // parallax height map (shader type 3) or the face detail mask (type 4);
    // slot 6 is the face tint mask (type 4) or the multilayer inner map
    // (type 11); slot 7 is the back-lighting map or, for model-space-normal
    // materials without backlight, the external specular map. The renderer
    // binds each to the register matching the material's derived flags.
    std::string diffuseTexture;   // Textures[0]
    std::string normalTexture;    // Textures[1] (tangent- or model-space normal; alpha = specular mask)
    std::string glowTexture;      // Textures[2] (glow map OR rim/soft LightMask - see above)
    std::string heightTexture;    // Textures[3] (parallax height map OR face detail mask)
    std::string cubeTexture;      // Textures[4] (environment cube map)
    std::string envMaskTexture;   // Textures[5] (environment map mask)
    std::string innerTexture;     // Textures[6] (face tint mask OR multilayer inner map)
    std::string backlightTexture; // Textures[7] (back-lighting map OR MSN specular map)

    Color3 emissiveColor { 0.0f, 0.0f, 0.0f };
    float emissiveMultiple = 1.0f;
    Color3 specularColor { 1.0f, 1.0f, 1.0f };
    float specularStrength = 1.0f;
    float glossiness = 80.0f;     // BSLightingShaderProperty's default (nif.xml line 6610)
    float alpha = 1.0f;

    Vector2 uvOffset { 0.0f, 0.0f };
    Vector2 uvScale { 1.0f, 1.0f };
    float lightingEffect1 = 0.3f; // soft-light wrap strength (nif.xml line 6614)
    float lightingEffect2 = 2.0f; // rim-light power (nif.xml line 6615)
    float environmentReflection = 1.0f; // Environment Map Scale / Eye Cubemap Scale
    Color3 tintColor { 1.0f, 1.0f, 1.0f }; // Skin/Hair Tint Color (shader type 5/6)

    // Multilayer parallax (shader type 11 / SLSF2_Multi_Layer_Parallax) -
    // nif.xml lines 6641-6644.
    Vector2 innerTextureScale { 1.0f, 1.0f };
    float innerThickness = 0.0f;
    float outerRefractionStrength = 0.0f;
    float outerReflectionStrength = 1.0f;

    // BSEffectShaderProperty (isEffectShader true) - nif.xml lines 6651-6691.
    // diffuseTexture holds Source Texture and emissiveColor/emissiveMultiple
    // hold Base Color rgb / Base Color Scale; the fields below are the rest.
    std::string greyscaleTexture;      // Greyscale Texture (palette for the greyscale* lookups)
    float effectEmissiveAlpha = 1.0f;  // Base Color alpha
    float falloffStartAngle = 1.0f;    // Falloff Start/Stop Angle+Opacity, used when useFalloff
    float falloffStopAngle = 1.0f;
    float falloffStartOpacity = 0.0f;
    float falloffStopOpacity = 0.0f;

    // Raw BSLightingShaderProperty identity, kept for derivation/debugging.
    std::uint32_t shaderType = 0;   // SkyrimShaderType (0=Default, 1=EnvMap, 2=Glow, 3=Parallax, 5=SkinTint, 6=HairTint, 16=EyeEnvmap, ...)
    std::uint32_t shaderFlags1 = 0; // SkyrimShaderPropertyFlags1
    std::uint32_t shaderFlags2 = 0; // SkyrimShaderPropertyFlags2

    // Derived per updateParams() - see the struct comment.
    bool hasSpecular = true;        // SLSF1_Specular (bit 0); off -> specular contribution is zero
    bool hasEmittance = false;      // SLSF1_Own_Emit (bit 22); off -> no emissive tint at all
    bool hasGlowMap = false;        // glow shader type + SLSF2_Glow_Map (bit 6) + slot 2 non-empty
    bool hasHeightMap = false;      // parallax shader type + SLSF1_Parallax (bit 11) + slot 3 non-empty
    bool hasBacklight = false;      // SLSF2_Back_Lighting (bit 27, Skyrim/SE only)
    bool hasRimlight = false;       // SLSF2_Rim_Lighting (bit 26, Skyrim/SE only)
    bool hasSoftlight = false;      // SLSF2_Soft_Lighting (bit 25, Skyrim/SE only)
    bool hasEnvironmentMap = false; // env/eye shader type + matching SLSF1 mapping flag
    bool hasCubeMap = false;        // hasEnvironmentMap + slot 4 non-empty
    bool useEnvironmentMask = false;// hasEnvironmentMap + slot 5 non-empty (else normal map alpha masks)
    bool hasTintColor = false;      // shader type 5 (skin) or 6 (hair)
    bool isDoubleSided = false;     // SLSF2_Double_Sided (bit 4)
    bool hasRefraction = false;     // SLSF1_Refraction (bit 15): a backbuffer-distortion effect
                                    // (heat haze etc.) with no direct color contribution - the
                                    // renderer skips these instead of drawing their textures flat
    bool isDecal = false;           // SLSF1_Decal (bit 26) or SLSF1_Dynamic_Decal (bit 27); same bit
                                    // positions in the Skyrim and FO4 SF1 words. The renderer draws
                                    // these with a depth bias (NifSkope: glPolygonOffset(-1,-1)) so
                                    // coplanar overlay meshes don't z-fight their base surface.
    bool hasAlphaBlend = false;     // from the shape's NiAlphaProperty (not this block)
    bool hasAlphaTest = false;      // NiAlphaProperty alpha-test enable (AlphaFlags bit 9)
    float alphaTestThreshold = 0.0f; // NiAlphaProperty Threshold / 255, for the shader's clip()
    // NiAlphaProperty blend functions (nif.xml AlphaFunction enum: 0=ONE,
    // 1=ZERO, 2=SRC_COLOR, ..., 6=SRC_ALPHA, 7=INV_SRC_ALPHA, ...). Fire
    // glows etc. use SRC_ALPHA/ONE (additive) - rendering those with the
    // standard alpha equation turns a light contribution into an occluder.
    std::uint8_t alphaSrcBlend = 6; // default SRC_ALPHA
    std::uint8_t alphaDstBlend = 7; // default INV_SRC_ALPHA
    bool depthWrite = true;         // SLSF2_ZBuffer_Write (bit 0); glow planes clear it

    // Community Shaders "True PBR" (PBRNifPatcher marking, Skyrim/SE only):
    // SLSF2_Unused01 flags the material as PBR; the fields/slots then mean
    // Glossiness = specular level, Specular Strength = roughness scale,
    // Lighting Effect 1 = subsurface opacity, Lighting Effect 2 =
    // displacement scale, slot 3 = displacement, slot 5 = RMAOS
    // (roughness/metal/AO/spec), slot 7 = subsurface color map.
    bool isPBR = false;                 // SLSF2_Unused01 (bit 23)
    bool pbrSubsurface = false;         // SLSF2_Rim_Lighting repurposed as the subsurface switch

    bool hasModelSpaceNormals = false;  // SLSF1_Model_Space_Normals (bit 12) - sk_msn.frag path
    bool hasSpecularMap = false;        // SLSF1_Specular + slot 7 non-empty (MSN external spec mask)
    bool hasTintMask = false;           // shader type 4 (FaceTint) - slot 6 overlay
    bool hasDetailMask = false;         // shader type 4 (FaceTint) - slot 3 overlay
    bool hasMultiLayerParallax = false; // SLSF2_Multi_Layer_Parallax (bit 24) - sk_multilayer.frag path

    // BSEffectShaderProperty-derived (sk_effectshader.frag path).
    bool isEffectShader = false;
    bool useFalloff = false;        // SLSF1_Use_Falloff (bit 6)
    bool greyscaleColor = false;    // SLSF1_Greyscale_To_PaletteColor (bit 4)
    bool greyscaleAlpha = false;    // SLSF1_Greyscale_To_PaletteAlpha (bit 5)
    bool hasWeaponBlood = false;    // SLSF2_Weapon_Blood (bit 17, Skyrim/SE only)
};

// Raw (un-transformed) triangle mesh geometry as read from NiTriShapeData /
// NiTriStripsData / BSTriShape's inline vertex buffer.
struct NifGeometry
{
    std::vector<Vector3> positions;
    std::vector<Vector3> normals;   // may be empty
    std::vector<Vector2> uvs;       // may be empty
    std::vector<Color4> colors;     // may be empty
    // Tangent only (no stored bitangent): the renderer reconstructs the
    // bitangent as cross(normal, tangent) in the pixel shader instead of
    // decoding the file's separately bit-packed Bitangent X/Y/Z fields (see
    // NifDocument.cpp's readVertexDataBuffer) - a standard simplification
    // that can get handedness wrong on meshes with mirrored UV islands, but
    // is correct for the common case and avoids a second packed-byte decode
    // path. May be empty if the source vertex format had no tangent data.
    std::vector<Vector3> tangents;
    std::vector<Triangle> triangles;
};

// Up to 4 (bone, weight) influences for one vertex of a NiSkinPartition-
// reconstructed mesh. boneIndex indexes into the owning NiSkinInstance's
// bones() array (itself a list of NiNode block indices - see
// NifDocument::skinInstanceBones()), NOT directly into m_nodes/m_blocks.
struct NifVertexSkinWeights
{
    std::array<std::uint16_t, 4> boneIndex { 0, 0, 0, 0 };
    std::array<float, 4> weight { 0.0f, 0.0f, 0.0f, 0.0f };
};

// NiSkinData: the bind-pose offsets needed to place a skinned mesh's stored
// (reference-pose) vertex positions correctly in the scene, per NifSkope's
// own bsshape.cpp BSShape::transformShapes() formula (see SceneBuilder.cpp's
// applySkinning - that's the code path NifSkope actually uses for the
// BSTriShape family, unlike glmesh.cpp which handles only legacy shapes):
//   finalWorldPos = sum_i weight_i *
//       (boneLocalTrans_i * boneOffsets[i]) * localVertexPos
// boneOffsets[i] is BoneData[i]'s "Skin Transform" field, index-aligned with
// the owning NiSkinInstance's Bones[]. skinTransform is NiSkinData's own
// top-level, identically-named "Skin Transform" field - parsed and kept for
// completeness, but NOT part of the BSShape formula (NifSkope reads it into
// skeletonTrans and its BSShape path never uses it).
struct NifSkinData
{
    Transform skinTransform;
    std::vector<Transform> boneOffsets; // parallel to the owning NiSkinInstance's bones()
};

// One renderable node in the scene graph: either a plain NiNode (no geometry,
// just a transform + children) or a shape (NiTriShape/NiTriStrips/BSTriShape
// family) that additionally owns geometry + a material.
struct NifSceneNode
{
    std::string name;
    std::int32_t blockIndex = kNoRef;
    std::int32_t parentIndex = kNoRef;
    Transform localTransform;

    bool isShape = false;
    // NiAVObject Flags bit 0 ("Hidden"/App_Culled): the engine never draws
    // these - furniture animation-marker rigs, editor markers, bounds
    // placeholders. Inherited down the subtree at scene-build time (a hidden
    // NiNode hides everything under it), matching NifSkope's Node::isHidden.
    bool isHidden = false;
    std::int32_t geometryBlockIndex = kNoRef;  // resolved NifGeometry owner, or kNoRef if inline (BSTriShape)
    NifGeometry inlineGeometry;                // populated directly for BSTriShape/BSSubIndexTriShape
    std::int32_t shaderPropertyIndex = kNoRef;
    std::int32_t alphaPropertyIndex = kNoRef;
    std::int32_t materialPropertyIndex = kNoRef; // legacy NiMaterialProperty, rarely used post-Skyrim

    // Skin instance (NiSkinInstance/BSDismemberSkinInstance) ref, if any. For
    // a skinned Skyrim SE shape the owning BSTriShape's own vertex buffer is
    // intentionally empty (Data Size == 0) - the actual rest-pose vertex/
    // triangle data instead lives in this skin instance's referenced
    // NiSkinPartition block. buildHierarchyAndRoots() resolves that chain
    // and fills inlineGeometry from it as a fallback when the shape's own
    // geometry came back empty - see NifDocument.cpp's parseNiSkinPartition.
    //
    // That fallback geometry alone is NOT correctly positioned/oriented,
    // though: a skinned shape's stored vertex positions are in an arbitrary
    // reference-pose space that only becomes correct once transformed by
    // per-vertex bone weights (real matrix-palette skinning), not simply by
    // composing this node's own transform up the parent chain like a rigid
    // shape - see SceneBuilder.cpp's applySkinning for the actual math.
    // hasSkinWeights is set once buildHierarchyAndRoots() has confirmed
    // NiSkinPartition-derived per-vertex weights are available (i.e.
    // whether SceneBuilder should run that path for this node at all).
    std::int32_t skinInstanceRef = kNoRef;
    bool hasSkinWeights = false;
    // NiSkinPartition block index the fallback geometry (and hasSkinWeights,
    // when true) came from - set alongside them in buildHierarchyAndRoots().
    // SceneBuilder looks up skinPartitionWeights()[skinPartitionRef] with
    // this, since NifVertexSkinWeights is keyed by partition block index,
    // not skinInstanceRef.
    std::int32_t skinPartitionRef = kNoRef;
};

// Top-level parsed document: header info + curated block records. See the
// scope note above - blocks with a type this parser does not recognise are
// retained only as a name/size record (m_blocks) for introspection/
// debugging; they contribute nothing to the render scene.
class NifDocument
{
public:
    [[nodiscard]] bool loadFromFile(const std::wstring& path, std::string* errorOut = nullptr);
    // data may point anywhere (e.g. a BSA extraction buffer); the bytes are
    // copied into this document, so the span only needs to live for the call.
    [[nodiscard]] bool loadFromMemory(std::span<const std::uint8_t> data, std::string* errorOut = nullptr);

    // Set only by loadFromFile (empty for loadFromMemory); used by the app
    // shell to persist "last opened" paths across sessions (phase4_shell).
    const std::wstring& filePath() const { return m_filePath; }

    bool isValid() const { return m_valid; }
    const std::string& versionString() const { return m_versionString; }
    std::uint32_t version() const { return m_version; }
    std::uint32_t bsVersion() const { return m_bsVersion; }

    int blockCount() const { return static_cast<int>(m_blocks.size()); }
    const std::string& blockTypeName(int index) const;

    const std::vector<NifSceneNode>& nodes() const { return m_nodes; }
    const std::unordered_map<std::int32_t, NifGeometry>& geometries() const { return m_geometries; }
    const std::unordered_map<std::int32_t, NifMaterial>& materials() const { return m_materials; }
    const std::unordered_map<std::int32_t, std::vector<std::string>>& textureSets() const { return m_textureSets; }

    // Skinning data, used by SceneBuilder::applySkinning() - see
    // NifSceneNode::hasSkinWeights / NifSkinData's comment for the math.
    // Block index -> block index into m_nodes' owner map (skinInstanceRef
    // -> NiNode block index list, parallel to that NiSkinInstance's Bones[]).
    const std::unordered_map<std::int32_t, std::vector<std::int32_t>>& skinInstanceBones() const { return m_skinInstanceBones; }
    const std::unordered_map<std::int32_t, std::int32_t>& skinInstanceToDataRef() const { return m_skinInstanceToDataRef; }
    const std::unordered_map<std::int32_t, NifSkinData>& skinData() const { return m_skinData; }
    const std::unordered_map<std::int32_t, std::vector<NifVertexSkinWeights>>& skinPartitionWeights() const { return m_skinPartitionWeights; }
    // Resolves a raw block index (e.g. from skinInstanceBones()) to an index
    // into nodes(), or kNoRef if that block wasn't parsed into a scene node.
    int nodeIndexForBlock(std::int32_t blockIndex) const;

    // Root node indices into nodes() (top-level NIF objects not referenced as
    // anyone else's child).
    const std::vector<int>& roots() const { return m_roots; }

    // Optional generic block/field browser tree (see NifItem.h), built for
    // every parsed block regardless of whether NifDocument understood its
    // contents, using only (block index, type name, byte size) - handy for a
    // lightweight "Block List" debug panel without a full reflection engine.
    const NifItem* blockTree() const { return m_blockTree.get(); }

    const std::string& lastError() const { return m_lastError; }

private:
    struct RawBlockInfo
    {
        std::string typeName;
        std::uint32_t size = 0;
        std::size_t fileOffset = 0;
    };

    bool parseHeader(class NifIStream& in);
    void parseBlocks(class NifIStream& in);
    void buildHierarchyAndRoots();

    // Individual block-type parsers. Each receives a stream pre-seeked to the
    // block's start offset (right after the type-name/size accounting done
    // by the header) and may stop reading early - see scope note above.
    void parseNiNode(class NifIStream& in, int blockIndex, bool isFadeNodeLike);
    void parseNiTriShapeOrStrips(class NifIStream& in, int blockIndex, bool isStrips);
    void parseNiTriShapeData(class NifIStream& in, int blockIndex);
    void parseNiTriStripsData(class NifIStream& in, int blockIndex);
    void parseBSTriShape(class NifIStream& in, int blockIndex);
    void parseBSLightingShaderProperty(class NifIStream& in, int blockIndex);
    void parseBSEffectShaderProperty(class NifIStream& in, int blockIndex);
    void parseBSShaderTextureSet(class NifIStream& in, int blockIndex);
    void parseNiMaterialProperty(class NifIStream& in, int blockIndex);
    void parseNiAlphaProperty(class NifIStream& in, int blockIndex);
    // NiSkinInstance/BSDismemberSkinInstance: reads the shared NiSkinInstance
    // prefix (Data ref, Skin Partition ref, Skeleton Root ptr, Bones[] ptr
    // array) both share - BSDismemberSkinInstance's own trailing body-part
    // fields are safely skipped via the block-size table, same as everywhere
    // else. Skeleton Root is read but not stored: SceneBuilder's skinning
    // assumes it coincides with the overall scene root (see its comment) -
    // true for a standalone character/armor skeleton, which covers this
    // parser's scope.
    void parseNiSkinInstance(class NifIStream& in, int blockIndex);
    // NiSkinData: per-bone bind-pose offsets needed to correctly place a
    // skinned shape's vertices - see NifSkinData's own comment.
    void parseNiSkinData(class NifIStream& in, int blockIndex);
    // NiSkinPartition (BS_SSE only - see NifDocument.cpp's scope note on
    // this parser): holds the actual rest-pose vertex/triangle/bone-weight
    // data for a skinned Skyrim SE shape whose own BSTriShape vertex buffer
    // is empty.
    void parseNiSkinPartition(class NifIStream& in, int blockIndex);
    // Shared BSVertexDataSSE/BSVertexData per-vertex decode, used by both
    // parseBSTriShape's inline buffer and parseNiSkinPartition's global
    // vertex buffer (identical wire format - nif.xml's BSVertexDesc-driven
    // arg on both "Vertex Data" fields is the same expression).
    // outSkinWeights, if non-null and VF_SKINNED is set in attribBits, is
    // resized to numVertices and filled with each vertex's own embedded
    // bone weights/indices (global-buffer-index-aligned - no partition-local
    // remap needed, unlike NiSkinPartition's separate per-partition Bone
    // Indices/Vertex Weights fields - see parseNiSkinPartition's comment on
    // why this is preferred over reconstructing from those instead).
    void readVertexDataBuffer(class NifIStream& in, NifGeometry& geo, std::uint16_t numVertices,
        std::uint16_t attribBits, bool allowFo4HalfPrecision,
        std::vector<NifVertexSkinWeights>* outSkinWeights = nullptr);

    // Shared NiObjectNET + NiAVObject leading-field reader (name/extra data
    // list/controller, then flags/translation/rotation/scale/collision).
    // Returns the object's display name (resolved through the string table).
    struct AvObjectHeader
    {
        std::string name;
        Transform transform;
        std::uint32_t flags = 0; // NiAVObject Flags (bit 0 = Hidden)
        std::int32_t collisionObjectRef = kNoRef;
    };
    // outShaderType, when non-null and isBSLightingShaderProperty is set,
    // receives the leading "Shader Type" u32 (SkyrimShaderType) that
    // BSLightingShaderProperty stores BEFORE the NiObjectNET name.
    std::string readObjectNetName(class NifIStream& in, bool isBSLightingShaderProperty,
        std::uint32_t* outShaderType = nullptr);
    AvObjectHeader readAvObjectHeader(class NifIStream& in, bool isBSLightingShaderProperty = false);
    std::vector<std::int32_t> readRefArray(class NifIStream& in, std::uint32_t count);
    std::string resolveString(std::int32_t index) const;
    static std::string normalizeSlashes(std::string s);

    bool m_valid = false;
    std::string m_versionString;
    std::uint32_t m_version = 0;
    std::uint32_t m_userVersion = 0;
    std::uint32_t m_bsVersion = 0;

    std::vector<RawBlockInfo> m_blocks;
    std::vector<std::string> m_strings;

    std::vector<NifSceneNode> m_nodes;                              // indexed the same as m_blocks for shape/node blocks
    std::unordered_map<std::int32_t, int> m_blockIndexToNodeIndex;   // block index -> index into m_nodes
    std::unordered_map<std::int32_t, NifGeometry> m_geometries;      // block index (NiTriShapeData/NiTriStripsData) -> geometry
    std::unordered_map<std::int32_t, NifMaterial> m_materials;       // block index (BSLightingShaderProperty/NiMaterialProperty) -> material
    std::unordered_map<std::int32_t, std::vector<std::string>> m_textureSets; // block index (BSShaderTextureSet) -> texture paths
    std::vector<int> m_roots;

    // Parse-time scratch state, resolved once (in buildHierarchyAndRoots())
    // after every block has been visited, since referenced blocks
    // (children/texture sets/alpha properties) are not guaranteed to
    // precede the block that references them in file order.
    std::unordered_map<std::int32_t, std::vector<std::int32_t>> m_pendingChildren;        // NiNode block index -> child block indices
    std::unordered_map<std::int32_t, std::int32_t> m_pendingMaterialTextureSetRef;         // shader property block index -> texture set block index
    std::unordered_map<std::int32_t, std::pair<std::uint16_t, std::uint8_t>> m_pendingAlphaFlags; // NiAlphaProperty block index -> (AlphaFlags, Threshold)
    std::unordered_map<std::int32_t, std::int32_t> m_skinInstanceToPartitionRef;           // NiSkinInstance/BSDismemberSkinInstance block index -> NiSkinPartition block index
    std::unordered_map<std::int32_t, NifGeometry> m_skinPartitionGeometries;               // NiSkinPartition block index -> reconstructed geometry (global vertex buffer + all partitions' triangles, remapped)
    std::unordered_map<std::int32_t, std::vector<NifVertexSkinWeights>> m_skinPartitionWeights; // NiSkinPartition block index -> per-global-vertex bone weights, parallel to m_skinPartitionGeometries' positions
    std::unordered_map<std::int32_t, std::int32_t> m_skinInstanceToDataRef;                // NiSkinInstance block index -> NiSkinData block index
    std::unordered_map<std::int32_t, std::vector<std::int32_t>> m_skinInstanceBones;       // NiSkinInstance block index -> Bones[] (NiNode block indices, in NifVertexSkinWeights::boneIndex order)
    std::unordered_map<std::int32_t, NifSkinData> m_skinData;                              // NiSkinData block index -> bind-pose offsets

    std::unique_ptr<NifItem> m_blockTree;
    std::string m_lastError;
    std::wstring m_filePath;

    // Owns the file bytes for the lifetime of the document (block records
    // above only store offsets/sizes; string data is copied out eagerly).
    std::vector<std::uint8_t> m_fileBytes;
};

} // namespace nsk
