// NifDocument.cpp - see NifDocument.h for the scope note on why this is a
// curated direct binary parser instead of a generic nif.xml interpreter.
//
// Every field read below is annotated with the nif.xml source line(s) it was
// derived from (liteviewer/schema_reference/nif.xml, vendored from the
// niftools/nifxml project for reference only) so the layout can be re-checked
// against a newer schema if Bethesda ever revises these block types. Target
// scope: NIF Version 20.2.0.7 with BS Version 83 (Skyrim LE), 100 (Skyrim
// SE), or 130 (Fallout 4) - the three configurations the plan's Phase 6
// validation step exercises.
#include "NifDocument.h"
#include "NifStream.h"
#include "NifLog.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <fstream>

namespace nsk
{

namespace
{
    constexpr std::uint32_t kVersion2027 = 0x14020007; // "20.2.0.7", see nif.xml FileVersion docs (line ~310).
    constexpr std::uint32_t kBsVerSSE = 100;
    constexpr std::uint32_t kBsVerFO4 = 130;

    Triangle makeTri(std::uint16_t a, std::uint16_t b, std::uint16_t c)
    {
        return Triangle(a, b, c);
    }

    // IEEE-754 binary16 -> binary32 expansion (no hardware F16C dependency;
    // this path is only hit for the rare half-precision BSTriShape vertex
    // formats, not per-frame rendering).
    float halfToFloat(std::uint16_t h)
    {
        std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
        std::uint32_t exp = (h >> 10) & 0x1F;
        std::uint32_t mant = h & 0x3FFu;
        std::uint32_t bits;
        if (exp == 0)
        {
            if (mant == 0)
            {
                bits = sign;
            }
            else
            {
                std::uint32_t e = 127 - 15 + 1;
                while ((mant & 0x400u) == 0) { mant <<= 1; --e; }
                mant &= 0x3FFu;
                bits = sign | (e << 23) | (mant << 13);
            }
        }
        else if (exp == 31)
        {
            bits = sign | 0x7F800000u | (mant << 13);
        }
        else
        {
            bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
        }
        return std::bit_cast<float>(bits);
    }

    // Narrow (UTF-8) conversion for log messages only - NifDocument otherwise
    // keeps paths as std::wstring throughout (see NifDocument.h).
    std::string toNarrowUtf8(const std::wstring& w)
    {
        if (w.empty())
            return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
        return s;
    }

    // Heuristic used only for diagnostic logging (NIFLOG_* below): flags block
    // type names that plausibly carry renderable geometry so an unrecognised
    // one (e.g. a shape subclass this parser's dispatch table doesn't list
    // yet) shows up loudly in the log instead of silently contributing zero
    // triangles - this is exactly the "some NIFs render, some show nothing"
    // failure mode this logging was added to diagnose.
    bool looksGeometryRelated(const std::string& typeName)
    {
        static const char* kNeedles[] = { "Shape", "Mesh", "Tri", "Geom", "Particle", "Lines" };
        for (const char* needle : kNeedles)
        {
            if (typeName.find(needle) != std::string::npos)
                return true;
        }
        return false;
    }

    // Standard triangle-strip -> triangle-list fan-out (alternating winding
    // so every triangle in the strip keeps consistent front-face order).
    void stripToTriangles(const std::vector<std::uint16_t>& strip, std::vector<Triangle>& out)
    {
        if (strip.size() < 3)
            return;
        for (std::size_t i = 0; i + 2 < strip.size(); ++i)
        {
            std::uint16_t a = strip[i];
            std::uint16_t b = strip[i + 1];
            std::uint16_t c = strip[i + 2];
            if (a == b || b == c || a == c)
                continue; // degenerate stitch triangle, common at strip boundaries
            if (i % 2 == 0)
                out.push_back(makeTri(a, b, c));
            else
                out.push_back(makeTri(b, a, c));
        }
    }
}

std::string NifDocument::normalizeSlashes(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string NifDocument::resolveString(std::int32_t index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= m_strings.size())
        return {};
    return m_strings[static_cast<std::size_t>(index)];
}

const std::string& NifDocument::blockTypeName(int index) const
{
    static const std::string kEmpty;
    if (index < 0 || static_cast<std::size_t>(index) >= m_blocks.size())
        return kEmpty;
    return m_blocks[static_cast<std::size_t>(index)].typeName;
}

bool NifDocument::loadFromFile(const std::wstring& path, std::string* errorOut)
{
    NIFLOG_INFO("NifDocument::loadFromFile: '{}'", toNarrowUtf8(path));

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
    {
        m_lastError = "Could not open file";
        if (errorOut) *errorOut = m_lastError;
        NIFLOG_WARN("NifDocument::loadFromFile: FAILED - could not open file");
        return false;
    }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    m_fileBytes.resize(static_cast<std::size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(m_fileBytes.data()), sz))
    {
        m_lastError = "Read failure";
        if (errorOut) *errorOut = m_lastError;
        return false;
    }
    const bool ok = loadFromMemory(m_fileBytes, errorOut);
    if (ok)
        m_filePath = path;
    return ok;
}

bool NifDocument::loadFromMemory(std::span<const std::uint8_t> data, std::string* errorOut)
{
    m_valid = false;
    m_nodes.clear();
    m_blocks.clear();
    m_strings.clear();
    m_geometries.clear();
    m_materials.clear();
    m_textureSets.clear();
    m_roots.clear();
    m_blockIndexToNodeIndex.clear();
    m_skinInstanceToPartitionRef.clear();
    m_skinPartitionGeometries.clear();

    if (data.data() != m_fileBytes.data())
    {
        m_fileBytes.assign(data.begin(), data.end());
        data = m_fileBytes;
    }

    NifIStream in(data);

    // Note: m_filePath is only set by loadFromFile() *after* this call
    // returns true, so it is not yet valid to log here.
    NIFLOG_INFO("NifDocument::load: {} bytes", data.size());

    if (!parseHeader(in))
    {
        m_lastError = "Not a recognised Skyrim/SSE/FO4 NIF (header parse failed)";
        if (errorOut) *errorOut = m_lastError;
        NIFLOG_WARN("NifDocument::load: FAILED - {}", m_lastError);
        return false;
    }
    NIFLOG_INFO("NifDocument::load: version='{}' bsVersion={} numBlocks={}",
        m_versionString, m_bsVersion, m_blocks.size());

    parseBlocks(in);
    buildHierarchyAndRoots();

    std::size_t shapeNodeCount = 0;
    for (const auto& n : m_nodes)
        if (n.isShape) ++shapeNodeCount;
    NIFLOG_INFO("NifDocument::load: done - {} scene node(s) ({} shape-typed), {} geometry data block(s), "
        "{} material(s), {} texture set(s), {} root(s)",
        m_nodes.size(), shapeNodeCount, m_geometries.size(), m_materials.size(), m_textureSets.size(), m_roots.size());

    m_valid = true;
    return true;
}

// --- Header ---------------------------------------------------------------
// nif.xml "Header" struct, lines ~1967-1987.
bool NifDocument::parseHeader(NifIStream& in)
{
    std::string headerLine = in.lineString();
    if (headerLine.find("Gamebryo File Format") == std::string::npos &&
        headerLine.find("NetImmerse File Format") == std::string::npos)
        return false;

    m_versionString = headerLine;
    m_version = in.u32();                                  // Version (FileVersion, line 1971)
    if (m_version != kVersion2027)
        return false; // out of scope - see class doc comment

    /*EndianType*/ in.u8();                                 // line 1972, since 20.0.0.3
    m_userVersion = in.u32();                                // User Version, line 1973
    std::uint32_t numBlocks = in.u32();                      // Num Blocks, line 1974

    // BS Header (BSStreamHeader, lines 1956-1964). #BSSTREAMHEADER# is true
    // for Version==20.2.0.7 unconditionally (line 40), which is all of our
    // supported versions.
    m_bsVersion = in.u32();                                  // BS Version (line 1958)
    in.shortString();                                        // Author (ExportString, line 1959)
    if (m_bsVersion > 130) in.u32();                          // Unknown Int, line 1960
    if (m_bsVersion < 131) in.shortString();                  // Process Script, line 1961
    in.shortString();                                         // Export Script, line 1962
    if (m_bsVersion >= 103) in.shortString();                 // Max Filepath, line 1963

    if (m_bsVersion != 83 && m_bsVersion != kBsVerSSE && m_bsVersion != kBsVerFO4)
        return false; // Skyrim LE / SE / FO4 only, per class doc comment.

    // Metadata (ByteArray, since=30.0.0.0) - not present for BSVER<=130.

    std::uint16_t numBlockTypes = in.u16();                   // line 1977
    std::vector<std::string> blockTypeNames(numBlockTypes);
    for (auto& s : blockTypeNames)
        s = in.sizedString();                                 // Block Types, line 1978 (4-byte length prefix)

    std::vector<std::uint16_t> blockTypeIndex(numBlocks);
    for (auto& idx : blockTypeIndex)
        idx = in.u16();                                        // Block Type Index, line 1980

    std::vector<std::uint32_t> blockSizes(numBlocks);
    for (auto& sz : blockSizes)
        sz = in.u32();                                         // Block Size, line 1981 (since 20.2.0.5)

    std::uint32_t numStrings = in.u32();                       // line 1982
    /*maxStringLength*/ in.u32();                              // line 1983
    m_strings.resize(numStrings);
    for (auto& s : m_strings)
        s = in.sizedString();                                  // Strings, line 1984

    std::uint32_t numGroups = in.u32();                        // line 1985
    for (std::uint32_t i = 0; i < numGroups; ++i)
        in.u32();                                              // Groups, line 1986

    if (in.eof())
        return false;

    std::size_t blockStart = in.pos();
    m_blocks.resize(numBlocks);
    std::size_t offset = blockStart;
    for (std::uint32_t i = 0; i < numBlocks; ++i)
    {
        std::uint16_t typeIdx = blockTypeIndex[i] & 0x7FFF; // top bit is a PhysX flag, line 306
        RawBlockInfo& b = m_blocks[i];
        b.typeName = (typeIdx < blockTypeNames.size()) ? blockTypeNames[typeIdx] : std::string();
        b.size = blockSizes[i];
        b.fileOffset = offset;
        offset += b.size;
    }

    m_blockTree = std::make_unique<NifItem>("NIF");
    return true;
}

// --- Block dispatch ---------------------------------------------------------
void NifDocument::parseBlocks(NifIStream& in)
{
    m_nodes.reserve(m_blocks.size());
    for (int i = 0; i < static_cast<int>(m_blocks.size()); ++i)
    {
        const RawBlockInfo& b = m_blocks[static_cast<std::size_t>(i)];

        NifItem* item = m_blockTree->addChild(std::to_string(i) + ": " + b.typeName);
        item->setValue(NifValue(static_cast<std::int64_t>(b.size)));

        if (!in.canRead(0) || b.fileOffset > in.size())
            continue;
        in.seek(b.fileOffset);

        const std::string& t = b.typeName;
        if (looksGeometryRelated(t))
            NIFLOG_TRACE("block {}: '{}' fileOffset={} size={}", i, t, b.fileOffset, b.size);
        const bool isNode = (t == "NiNode" || t == "BSFadeNode" || t == "BSLeafAnimNode" ||
                             t == "BSTreeNode" || t == "BSOrderedNode" || t == "BSMultiBoundNode" ||
                             t == "BSDebrisNode" || t == "BSDamageStage" || t == "BSBlastNode");
        const bool isOldShape = (t == "NiTriShape" || t == "NiTriStrips");
        const bool isBSShape = (t == "BSTriShape" || t == "BSSubIndexTriShape" || t == "BSMeshLODTriShape");

        if (isNode)
            parseNiNode(in, i, false);
        else if (isOldShape)
        {
            parseNiTriShapeOrStrips(in, i, t == "NiTriStrips");
            NIFLOG_TRACE("block {}: shape '{}' (geometry in separate *Data block)", i, t);
        }
        else if (t == "NiTriShapeData")
            parseNiTriShapeData(in, i);
        else if (t == "NiTriStripsData")
            parseNiTriStripsData(in, i);
        else if (isBSShape)
        {
            parseBSTriShape(in, i);
            NIFLOG_TRACE("block {}: shape '{}' (inline vertex buffer)", i, t);
        }
        else if (t == "BSLightingShaderProperty")
            parseBSLightingShaderProperty(in, i);
        else if (t == "BSShaderTextureSet")
            parseBSShaderTextureSet(in, i);
        else if (t == "NiMaterialProperty")
            parseNiMaterialProperty(in, i);
        else if (t == "NiAlphaProperty")
            parseNiAlphaProperty(in, i);
        else if (t == "NiSkinInstance" || t == "BSDismemberSkinInstance")
            parseNiSkinInstance(in, i);
        else if (t == "NiSkinPartition")
            parseNiSkinPartition(in, i);
        else
        {
            // Everything else: intentionally left unparsed. Next block's
            // offset comes from the header's Block Size table, not from
            // stream position, so unrecognised types are safely skipped
            // whole. Flag ones that look geometry-related loudly - a shape
            // subclass missing from the isOldShape/isBSShape checks above
            // (e.g. BSDynamicTriShape, BSSegmentedTriShape) silently
            // contributes zero triangles, which is exactly the "some NIFs
            // render, some show nothing" symptom this logging exists for.
            if (looksGeometryRelated(t))
            {
                NIFLOG_WARN("block {}: unrecognised geometry-like type '{}' (size={}) - "
                    "skipped, will NOT render", i, t, b.size);
            }
        }
    }
}

// --- Shared NiObjectNET / NiAVObject header reader --------------------------
std::string NifDocument::readObjectNetName(NifIStream& in, bool isBSLightingShaderProperty)
{
    // nif.xml lines 3363-3372.
    if (isBSLightingShaderProperty)
        in.u32(); // "Shader Type" (BSLightingShaderType), onlyT=BSLightingShaderProperty, line 3365

    std::int32_t nameIdx = in.i32();               // Name (string -> StringRef), line 3366
    std::uint32_t numExtra = in.u32();              // Num Extra Data List, line 3369
    for (std::uint32_t i = 0; i < numExtra; ++i)
        in.i32();                                   // Extra Data List (Ref[]), line 3370
    in.i32();                                        // Controller (Ref), line 3371

    return resolveString(nameIdx);
}

NifDocument::AvObjectHeader NifDocument::readAvObjectHeader(NifIStream& in, bool isBSLightingShaderProperty)
{
    AvObjectHeader hdr;
    hdr.name = readObjectNetName(in, isBSLightingShaderProperty);

    // nif.xml lines 3444-3499. BSVER (83/100/130) is always > 26, so the
    // 32-bit Flags variant (line 3446) applies, never the legacy ushort one.
    in.u32();                                        // Flags, line 3446
    Vector3 t = in.vector3();                        // Translation, line 3488
    Matrix rot = in.matrix33();                      // Rotation, line 3489
    float scale = in.f32();                           // Scale, line 3490
    // Velocity (until 4.2.2.0) and Num Properties/Properties (NI_BS_LTE_FO3)
    // do not exist for our versions - nothing to skip here.
    hdr.collisionObjectRef = in.i32();                 // Collision Object (Ref), line 3498, since 10.0.1.0

    hdr.transform.translation = t;
    hdr.transform.rotation = rot;
    hdr.transform.scale = scale;
    return hdr;
}

std::vector<std::int32_t> NifDocument::readRefArray(NifIStream& in, std::uint32_t count)
{
    std::vector<std::int32_t> refs(count);
    for (auto& r : refs)
        r = in.i32();
    return refs;
}

// --- NiNode ------------------------------------------------------------------
void NifDocument::parseNiNode(NifIStream& in, int blockIndex, bool /*isFadeNodeLike*/)
{
    AvObjectHeader hdr = readAvObjectHeader(in);

    NifSceneNode node;
    node.name = hdr.name;
    node.blockIndex = blockIndex;
    node.localTransform = hdr.transform;
    node.isShape = false;

    std::uint32_t numChildren = in.u32();             // Num Children, nif.xml line 4390
    std::vector<std::int32_t> children = readRefArray(in, numChildren); // Children, line 4391
    // Num Effects/Effects (NI_BS_LT_FO4 only) intentionally not read - not
    // needed to build the render scene graph, and safely skipped via the
    // block-size table regardless of BS version.

    m_blockIndexToNodeIndex[blockIndex] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(node);

    // Record children as a temporary field on the node via parentIndex fixups
    // done in buildHierarchyAndRoots(); stash the raw ref list keyed by block
    // index so that pass can resolve parent/child links after every block has
    // been parsed (guarantees forward-referenced children still resolve).
    m_pendingChildren[blockIndex] = std::move(children);
}

// --- NiTriShape / NiTriStrips (legacy geometry + external *Data block) -----
void NifDocument::parseNiTriShapeOrStrips(NifIStream& in, int blockIndex, bool /*isStrips*/)
{
    AvObjectHeader hdr = readAvObjectHeader(in);

    NifSceneNode node;
    node.name = hdr.name;
    node.blockIndex = blockIndex;
    node.localTransform = hdr.transform;
    node.isShape = true;

    // NiGeometry fields (nif.xml lines 3866-3873): Data / Skin Instance /
    // Material Data / Shader Property / Alpha Property. Byte layout is
    // identical whether the file uses the NI_BS_LT_SSE row or the
    // BS_GTE_SSE row (both apply to Version==20.2.0.7, just gated by BSVER).
    node.geometryBlockIndex = in.i32();                // Data (Ref), line 3866/3867
    node.skinInstanceRef = in.i32();                    // Skin Instance (Ref), line 3868/3869 - see
                                                          // buildHierarchyAndRoots()'s skin-partition fallback

    // MaterialData (struct, lines 3844-3855), since=20.2.0.5 -> Num
    // Materials onward is present for our version.
    std::uint32_t numMaterials = in.u32();              // Num Materials
    for (std::uint32_t i = 0; i < numMaterials; ++i)
    {
        in.i32(); // Material Name (NiFixedString)
        in.i32(); // Material Extra Data (int)
    }
    in.i32();                                            // Active Material
    in.u8();                                              // Material Needs Update (bool), since 20.2.0.7

    node.shaderPropertyIndex = in.i32();                 // Shader Property (Ref), line 3872 (BS_GT_FO3, always true here)
    node.alphaPropertyIndex = in.i32();                  // Alpha Property (Ref), line 3873

    m_blockIndexToNodeIndex[blockIndex] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(node);
}

// --- NiGeometryData leading fields shared by NiTriShapeData/NiTriStripsData
namespace
{
    struct GeomDataHeader
    {
        std::uint16_t numVertices = 0;
        bool hasTangents = false;
    };
}

static GeomDataHeader readGeomDataHeaderAndVerts(NifIStream& in, NifGeometry& geo)
{
    GeomDataHeader hdr;

    in.i32();                                            // Group ID, line 3886, since 10.1.0.114
    hdr.numVertices = in.u16();                           // Num Vertices, line 3887
    in.u8();                                              // Keep Flags, line 3890
    in.u8();                                              // Compress Flags, line 3891
    bool hasVertices = in.boolean();                      // Has Vertices, line 3892
    if (hasVertices)
    {
        geo.positions.resize(hdr.numVertices);
        for (auto& v : geo.positions)
            v = in.vector3();                              // Vertices, line 3893
    }

    // #BS202# (Version==20.2.0.7 && BSVER>0) is true for all supported
    // files, so "BS Data Flags" (ushort bitfield) applies, never the legacy
    // "Data Flags" enum (line 3894 vs 3895).
    std::uint16_t bsDataFlags = in.u16();                 // BS Data Flags, line 3895
    bool hasUv = (bsDataFlags & 0x0001) != 0;              // bit 0, nif.xml line 1640
    hdr.hasTangents = (bsDataFlags & 0x1000) != 0;         // bit 12, nif.xml line 1642

    in.u32();                                              // Material CRC, line 3896 (BS_GT_FO3, always true here)

    bool hasNormals = in.boolean();                       // Has Normals, line 3897
    if (hasNormals)
    {
        geo.normals.resize(hdr.numVertices);
        for (auto& n : geo.normals)
            n = in.vector3();                              // Normals, line 3898
    }
    if (hasNormals && hdr.hasTangents)
    {
        for (std::uint16_t i = 0; i < hdr.numVertices; ++i)
            in.vector3();                                   // Tangents, line 3899 (not needed for basic shading)
        for (std::uint16_t i = 0; i < hdr.numVertices; ++i)
            in.vector3();                                   // Bitangents, line 3900
    }

    in.f32(); in.f32(); in.f32(); in.f32();                // Bounding Sphere (NiBound = Vector3+float), line 3903

    bool hasVertexColors = in.boolean();                   // Has Vertex Colors, line 3904
    if (hasVertexColors)
    {
        geo.colors.resize(hdr.numVertices);
        for (auto& c : geo.colors)
            c = in.color4();                                // Vertex Colors, line 3911
    }

    // UV Sets (line 3918): length = (BSDataFlags & 1) since legacy "Data
    // Flags" doesn't exist post-4.2.2.0. 0 or 1 UV set for every Bethesda
    // mesh we care about.
    if (hasUv)
    {
        geo.uvs.resize(hdr.numVertices);
        for (auto& uv : geo.uvs)
            uv = in.vector2();                              // TexCoord, one set only
    }

    in.u16();                                              // Consistency Flags, line 3919 (ConsistencyType, ushort)
    in.i32();                                              // Additional Data (Ref), line 3920, since 20.0.0.4

    return hdr;
}

void NifDocument::parseNiTriShapeData(NifIStream& in, int blockIndex)
{
    NifGeometry geo;
    readGeomDataHeaderAndVerts(in, geo);

    std::uint16_t numTriangles = in.u16();                 // Num Triangles (NiTriBasedGeomData), line 3928
    std::uint32_t numTrianglePoints = in.u32();             // Num Triangle Points, line 5292 (unused, kept for parity)
    (void)numTrianglePoints;
    bool hasTriangles = in.boolean();                      // Has Triangles, line 5293, since 10.1.0.0
    if (hasTriangles)
    {
        geo.triangles.resize(numTriangles);
        for (auto& tri : geo.triangles)
            tri = in.triangle();                            // Triangles, line 5295
    }

    m_geometries[blockIndex] = std::move(geo);
}

void NifDocument::parseNiTriStripsData(NifIStream& in, int blockIndex)
{
    NifGeometry geo;
    readGeomDataHeaderAndVerts(in, geo);

    in.u16();                                               // Num Triangles (NiTriBasedGeomData), line 3928 - unused for strips

    std::uint16_t numStrips = in.u16();                     // Num Strips, line 5306
    std::vector<std::uint16_t> stripLengths(numStrips);
    for (auto& len : stripLengths)
        len = in.u16();                                     // Strip Lengths, line 5307

    bool hasPoints = in.boolean();                          // Has Points, line 5308, since 10.0.1.3
    if (hasPoints)
    {
        for (std::uint16_t s = 0; s < numStrips; ++s)
        {
            std::vector<std::uint16_t> strip(stripLengths[s]);
            for (auto& p : strip)
                p = in.u16();                                // Points, line 5310
            stripToTriangles(strip, geo.triangles);
        }
    }

    m_geometries[blockIndex] = std::move(geo);
}

// --- BSTriShape family (Skyrim SE / Fallout 4 inline vertex buffer) --------
void NifDocument::parseBSTriShape(NifIStream& in, int blockIndex)
{
    AvObjectHeader hdr = readAvObjectHeader(in);

    NifSceneNode node;
    node.name = hdr.name;
    node.blockIndex = blockIndex;
    node.localTransform = hdr.transform;
    node.isShape = true;
    node.geometryBlockIndex = kNoRef; // inline geometry, not a separate *Data block

    // nif.xml lines 8242-8258.
    in.f32(); in.f32(); in.f32(); in.f32();                 // Bounding Sphere (NiBound), line 8242
    if (m_bsVersion >= 155) { in.f32(); in.f32(); in.f32(); in.f32(); in.f32(); in.f32(); } // Bound Min Max, F76 only (out of scope, defensive)

    node.skinInstanceRef = in.i32();                        // Skin (Ref), line 8244 - see the fallback resolution
                                                              // in buildHierarchyAndRoots() for skinned shapes whose
                                                              // own Data Size is 0 (see NifSceneNode::skinInstanceRef).
    node.shaderPropertyIndex = in.i32();                    // Shader Property (Ref), line 8245
    node.alphaPropertyIndex = in.i32();                     // Alpha Property (Ref), line 8246

    std::uint64_t vertexDesc = in.read<std::uint64_t>();    // Vertex Desc (BSVertexDesc), line 8247
    std::uint16_t attribBits = static_cast<std::uint16_t>((vertexDesc >> 44) & 0xFFFFull); // "Vertex Desc >> 44", line 8252

    std::uint32_t numTriangles = (m_bsVersion >= kBsVerFO4) ? in.u32() : static_cast<std::uint32_t>(in.u16()); // line 8248/8249
    std::uint16_t numVertices = in.u16();                    // Num Vertices, line 8250
    std::uint32_t dataSize = in.u32();                       // Data Size, line 8251 (still physically stored)

    NIFLOG_TRACE("parseBSTriShape: block {} '{}' vertexDesc=0x{:016x} attribBits=0x{:04x} "
        "numTriangles={} numVertices={} dataSize={} skinRef={}",
        blockIndex, hdr.name, vertexDesc, attribBits, numTriangles, numVertices, dataSize, node.skinInstanceRef);

    NifGeometry geo;
    if (dataSize > 0)
    {
        readVertexDataBuffer(in, geo, numVertices, attribBits, /*allowFo4HalfPrecision=*/true);

        geo.triangles.resize(numTriangles);
        for (auto& tri : geo.triangles)
            tri = in.triangle();                              // Triangles, line 8254
    }

    node.inlineGeometry = std::move(geo);

    m_blockIndexToNodeIndex[blockIndex] = static_cast<int>(m_nodes.size());
    m_nodes.push_back(node);
}

// --- Shared BSVertexDataSSE/BSVertexData per-vertex decode ------------------
// Factored out of parseBSTriShape (still its only caller for the FO4 half-
// precision branch) so parseNiSkinPartition's structurally-identical global
// "Vertex Data" field (nif.xml line 5103, BS_SSE only - i.e.
// allowFo4HalfPrecision is always false for that caller) can reuse it
// instead of duplicating ~80 lines of bit-for-bit vertex decode logic.
void NifDocument::readVertexDataBuffer(NifIStream& in, NifGeometry& geo, std::uint16_t numVertices,
    std::uint16_t attribBits, bool allowFo4HalfPrecision)
{
    // Attribute masks per NifTypes.h's VertexFlags (nif.xml BSVertexDesc bits 44+).
    bool fullPrecision = (attribBits & VF_FULLPREC) != 0; // only meaningful for the FO4 BSVertexData branch
    bool hasPos = (attribBits & VF_VERTEX) != 0;
    bool hasUv = (attribBits & VF_UV) != 0;
    bool hasNormal = (attribBits & VF_NORMAL) != 0;
    bool hasTangent = (attribBits & VF_TANGENT) != 0;
    bool hasColor = (attribBits & VF_COLORS) != 0;
    bool hasSkin = (attribBits & VF_SKINNED) != 0;

    geo.positions.resize(numVertices);
    if (hasUv) geo.uvs.resize(numVertices);
    if (hasNormal) geo.normals.resize(numVertices);
    if (hasColor) geo.colors.resize(numVertices);

    for (std::uint16_t v = 0; v < numVertices; ++v)
    {
        // BSVertexDataSSE (SSE, nif.xml lines 2132-2138) always stores a
        // full-precision Vector3 position. The older BSVertexData struct
        // (used for BSVER>=130/FO4 BSTriShape only - NiSkinPartition's
        // global buffer is BS_SSE-only, so allowFo4HalfPrecision is always
        // false there) instead branches on the FULLPREC bit between
        // HalfVector3 and Vector3 - both are handled here since BSTriShape
        // is used by both SSE and FO4. FO4/F76's BSVertexData (nif.xml
        // lines 2111-2130) branches the Vertex+trailing-W fields together on
        // VF_FULLPREC (0x400): full precision uses Vector3 + float/uint (4
        // bytes each, like SSE's BSVertexDataSSE below); half precision uses
        // HalfVector3 (3 x hfloat) + hfloat/ushort (2 bytes each) - NOT a
        // 4th position half. Reading a fixed-size trailing field regardless
        // of precision (as an earlier version of this code did) double-
        // read/mis-sized this field for half-precision FO4 vertices and
        // desynced every field after it for the rest of the vertex.
        bool fo4HalfPrecision = allowFo4HalfPrecision && (m_bsVersion >= kBsVerFO4 && !fullPrecision);
        if (hasPos)
        {
            if (fo4HalfPrecision)
            {
                float x = halfToFloat(in.read<std::uint16_t>());
                float y = halfToFloat(in.read<std::uint16_t>());
                float z = halfToFloat(in.read<std::uint16_t>());
                geo.positions[v] = Vector3(x, y, z);
            }
            else
            {
                geo.positions[v] = in.vector3();
            }
        }

        if (fo4HalfPrecision)
        {
            if (hasTangent && hasPos)
                in.read<std::uint16_t>();                  // Bitangent X (hfloat), cond (arg&0x411)==0x11
            else if (hasPos)
                in.read<std::uint16_t>();                  // Unused W (ushort), cond (arg&0x411)==0x1
        }
        else
        {
            if (hasTangent && hasPos)
                in.f32();                                  // Bitangent X (float), cond (arg&0x11)==0x11 / (arg&0x411)==0x411
            else if (hasPos)
                in.u32();                                   // Unused W (uint), cond (arg&0x11)==0x1 / (arg&0x411)==0x401
        }

        if (hasUv)
        {
            // HalfTexCoord = 2 x hfloat.
            float uu = halfToFloat(in.read<std::uint16_t>());
            float vv = halfToFloat(in.read<std::uint16_t>());
            geo.uvs[v] = Vector2(uu, vv);
        }

        if (hasNormal)
        {
            float nx = (static_cast<int>(in.u8()) - 127) / 127.0f;
            float ny = (static_cast<int>(in.u8()) - 127) / 127.0f;
            float nz = (static_cast<int>(in.u8()) - 127) / 127.0f;
            geo.normals[v] = Vector3(nx, ny, nz);
            in.u8();                                      // Bitangent Y (normbyte), cond arg&0x8
        }
        if (hasNormal && hasTangent)
        {
            in.u8(); in.u8(); in.u8();                    // Tangent (ByteVector3), cond (arg&0x18)==0x18
            in.u8();                                       // Bitangent Z (normbyte)
        }
        if (hasColor)
        {
            float r = in.u8() / 255.0f;
            float g = in.u8() / 255.0f;
            float b = in.u8() / 255.0f;
            float a = in.u8() / 255.0f;
            geo.colors[v] = Color4(r, g, b, a);
        }
        if (hasSkin)
        {
            in.read<std::uint16_t>(); in.read<std::uint16_t>(); in.read<std::uint16_t>(); in.read<std::uint16_t>(); // Bone Weights (hfloat[4])
            in.u8(); in.u8(); in.u8(); in.u8();           // Bone Indices (byte[4])
        }
    }
}

// --- NiSkinInstance / BSDismemberSkinInstance -------------------------------
void NifDocument::parseNiSkinInstance(NifIStream& in, int blockIndex)
{
    // nif.xml lines 5080-5087. NiSkinInstance inherits NiObject directly (no
    // name/extra-data/controller header, unlike NiObjectNET-derived types).
    // BSDismemberSkinInstance's own trailing body-part fields (Num
    // Partitions/Partitions) are intentionally not read - safely skipped via
    // the block-size table, same as everywhere else.
    /*dataRef*/ in.i32();                     // Data (Ref -> NiSkinData), line 5082
    std::int32_t skinPartitionRef = in.i32(); // Skin Partition (Ref -> NiSkinPartition), line 5083

    m_skinInstanceToPartitionRef[blockIndex] = skinPartitionRef;
}

// --- NiSkinPartition (BS_SSE) ------------------------------------------------
// Holds the actual rest-pose vertex/triangle data for a skinned Skyrim SE
// shape whose owning BSTriShape has Data Size == 0 (see parseBSTriShape /
// NifSceneNode::skinInstanceRef) - Bethesda's SSE export strips the inline
// buffer on a skinned BSTriShape and stores one shared vertex buffer plus
// per-bone-group "partitions" here instead, to avoid duplicating vertex data
// per partition. Reconstructing a flat NifGeometry only needs: (a) the
// shared global vertex buffer (nif.xml line 5103, same BSVertexDataSSE wire
// format as BSTriShape's own inline buffer - see readVertexDataBuffer), and
// (b) each partition's triangles, whose LOCAL vertex indices are remapped to
// GLOBAL indices into that shared buffer via the partition's own Vertex Map.
// No bone weights/skeleton needed for a static bind-pose render - see this
// class's scope note on skinning/animation being out of scope, which this
// deliberately does NOT reopen: this only reads already-baked rest-pose
// positions, not runtime bone deformation.
void NifDocument::parseNiSkinPartition(NifIStream& in, int blockIndex)
{
    std::uint32_t numPartitions = in.u32();               // Num Partitions, line 5099
    std::uint32_t dataSize = in.u32();                     // Data Size, line 5100 (BS_SSE, physically stored)
    std::uint32_t vertexSize = in.u32();                    // Vertex Size, line 5101 (BS_SSE, physically stored)
    std::uint64_t vertexDesc = in.read<std::uint64_t>();    // Vertex Desc, line 5102
    std::uint16_t attribBits = static_cast<std::uint16_t>((vertexDesc >> 44) & 0xFFFFull);

    NIFLOG_TRACE("parseNiSkinPartition: block {} numPartitions={} dataSize={} vertexSize={} attribBits=0x{:04x}",
        blockIndex, numPartitions, dataSize, vertexSize, attribBits);

    NifGeometry geo; // shared global vertex buffer; triangles accumulated below across all partitions
    std::uint32_t numVerticesGlobal = (vertexSize > 0) ? (dataSize / vertexSize) : 0;
    if (dataSize > 0 && numVerticesGlobal > 0 && numVerticesGlobal <= 0xFFFFu)
    {
        readVertexDataBuffer(in, geo, static_cast<std::uint16_t>(numVerticesGlobal), attribBits,
            /*allowFo4HalfPrecision=*/false); // line 5103's Vertex Data field is BS_SSE-only
    }

    for (std::uint32_t p = 0; p < numPartitions; ++p)
    {
        // SkinPartition struct, nif.xml lines 2147-2173.
        std::uint16_t pNumVertices = in.u16();               // Num Vertices
        std::uint16_t pNumTriangles = in.u16();               // Num Triangles
        std::uint16_t pNumBones = in.u16();                    // Num Bones
        std::uint16_t pNumStrips = in.u16();                    // Num Strips
        std::uint16_t pNumWeightsPerVertex = in.u16();           // Num Weights Per Vertex
        for (std::uint16_t b = 0; b < pNumBones; ++b)
            in.u16();                                             // Bones[]

        bool hasVertexMap = in.boolean();                        // Has Vertex Map, since 10.1.0.0 (always true for our version range)
        std::vector<std::uint16_t> vertexMap;
        if (hasVertexMap)
        {
            vertexMap.resize(pNumVertices);
            for (auto& vm : vertexMap)
                vm = in.u16();                                    // Vertex Map
        }

        bool hasVertexWeights = in.boolean();                    // Has Vertex Weights
        if (hasVertexWeights)
        {
            // Vertex weights only matter for runtime bone deformation, not a
            // static bind-pose render (see this function's scope note).
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pNumVertices) * pNumWeightsPerVertex; ++i)
                in.f32();
        }

        std::vector<std::uint16_t> stripLengths(pNumStrips);
        for (auto& len : stripLengths)
            len = in.u16();                                       // Strip Lengths

        bool hasFaces = in.boolean();                             // Has Faces, since 10.1.0.0

        std::vector<Triangle> localTriangles;
        if (hasFaces)
        {
            if (pNumStrips != 0)
            {
                for (std::uint16_t s = 0; s < pNumStrips; ++s)
                {
                    std::vector<std::uint16_t> strip(stripLengths[s]);
                    for (auto& pt : strip)
                        pt = in.u16();                             // Strips
                    stripToTriangles(strip, localTriangles);
                }
            }
            else
            {
                localTriangles.resize(pNumTriangles);
                for (auto& tri : localTriangles)
                    tri = in.triangle();                           // Triangles
            }
        }

        bool hasBoneIndices = in.boolean();                       // Has Bone Indices
        if (hasBoneIndices)
        {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pNumVertices) * pNumWeightsPerVertex; ++i)
                in.u8();                                            // Bone Indices
        }

        // BS_SSE-only trailing fields (nif.xml lines 2169-2172): LOD Level,
        // Global VB, a per-partition Vertex Desc (redundant with the block-
        // level one above) and a Triangles Copy - none needed here, and
        // safely skipped via the block-size table like everywhere else.

        // Remap this partition's LOCAL triangle indices (0..pNumVertices-1)
        // to GLOBAL indices into the shared vertex buffer read above, via
        // this partition's own Vertex Map (falls back to identity if the
        // map was absent, which should not happen for our version range).
        for (const Triangle& lt : localTriangles)
        {
            auto mapIdx = [&vertexMap](std::uint16_t localIdx) -> std::uint16_t
            {
                return (localIdx < vertexMap.size()) ? vertexMap[localIdx] : localIdx;
            };
            geo.triangles.emplace_back(mapIdx(lt.v1()), mapIdx(lt.v2()), mapIdx(lt.v3()));
        }
    }

    NIFLOG_TRACE("parseNiSkinPartition: block {} -> {} global vert(s), {} tri(s) across {} partition(s)",
        blockIndex, geo.positions.size(), geo.triangles.size(), numPartitions);

    m_skinPartitionGeometries[blockIndex] = std::move(geo);
}

// --- BSLightingShaderProperty -----------------------------------------------
void NifDocument::parseBSLightingShaderProperty(NifIStream& in, int blockIndex)
{
    readObjectNetName(in, /*isBSLightingShaderProperty=*/true);
    // BSShaderProperty contributes 0 bytes for BSVER>34 (nif.xml line 6230,
    // gated by NI_BS_LTE_FO3), so BSLightingShaderProperty's own fields
    // start immediately after the NiObjectNET header above.

    in.u32();                                                 // Shader Flags 1 (SK or FO4 variant), lines 6591/6593 - both 4 bytes
    in.u32();                                                  // Shader Flags 2, lines 6592/6594 - both 4 bytes
    // BSVER>=155 (Fallout 76) Shader Type/SF1/SF2 arrays: out of scope, not present for BSVER<=130.

    in.vector2();                                              // UV Offset (TexCoord), line 6600
    in.vector2();                                              // UV Scale (TexCoord), line 6601

    NifMaterial mat;
    std::int32_t textureSetRef = in.i32();                     // Texture Set (Ref), line 6602
    mat.emissiveColor = in.color3();                           // Emissive Color, line 6603
    mat.emissiveMultiple = in.f32();                            // Emissive Multiple, line 6604
    // Everything after this point (Root Material, Alpha, Glossiness,
    // Specular, per-Shader-Type tail...) is intentionally not read - see
    // NifDocument.h scope note; the next block is located via the header's
    // Block Size table, not via stream position.

    // BSShaderTextureSet may appear later in block order than this
    // property, so texture path resolution is deferred to
    // buildHierarchyAndRoots() (see m_pendingMaterialTextureSetRef).
    m_pendingMaterialTextureSetRef[blockIndex] = textureSetRef;
    m_materials[blockIndex] = mat;
}

// --- BSShaderTextureSet ------------------------------------------------------
void NifDocument::parseBSShaderTextureSet(NifIStream& in, int blockIndex)
{
    // nif.xml lines 6315-6318. Inherits NiObject directly (no name/extra
    // data/controller header).
    std::uint32_t numTextures = in.u32();                      // Num Textures
    std::vector<std::string> textures(numTextures);
    for (auto& t : textures)
        t = normalizeSlashes(in.sizedString());                // Textures[N] (SizedString)
    m_textureSets[blockIndex] = std::move(textures);
}

// --- NiMaterialProperty (legacy, rare post-Skyrim) --------------------------
void NifDocument::parseNiMaterialProperty(NifIStream& in, int blockIndex)
{
    readObjectNetName(in, false);

    // nif.xml lines 4367-4377. BSVER (83/100/130) all >= 26, so Ambient/
    // Diffuse Color are skipped.
    NifMaterial mat;
    mat.specularColor = in.color3();                           // Specular Color, line 4372
    mat.emissiveColor = in.color3();                            // Emissive Color, line 4373
    in.f32();                                                    // Glossiness, line 4374
    mat.alpha = in.f32();                                        // Alpha, line 4375
    if (m_bsVersion > 21)
        mat.emissiveMultiple = in.f32();                         // Emissive Mult, line 4376

    m_materials[blockIndex] = mat;
}

// --- NiAlphaProperty ---------------------------------------------------------
void NifDocument::parseNiAlphaProperty(NifIStream& in, int blockIndex)
{
    readObjectNetName(in, false);

    std::uint16_t flags = in.u16();                             // Flags (AlphaFlags), nif.xml line 3978
    in.u8();                                                     // Threshold, line 3979

    // Recorded onto whichever material already references this alpha
    // property block, if any, once buildHierarchyAndRoots() links shapes to
    // their shader/alpha property indices; for now stash the flag directly
    // keyed by this block's index for that lookup.
    m_pendingAlphaFlags[blockIndex] = flags;
}

// --- Hierarchy + material/alpha resolution ----------------------------------
void NifDocument::buildHierarchyAndRoots()
{
    for (auto& [parentBlockIdx, children] : m_pendingChildren)
    {
        auto parentIt = m_blockIndexToNodeIndex.find(parentBlockIdx);
        if (parentIt == m_blockIndexToNodeIndex.end())
            continue;
        int parentNodeIdx = parentIt->second;
        for (std::int32_t childBlockIdx : children)
        {
            auto childIt = m_blockIndexToNodeIndex.find(childBlockIdx);
            if (childIt == m_blockIndexToNodeIndex.end())
                continue;
            m_nodes[static_cast<std::size_t>(childIt->second)].parentIndex = parentNodeIdx;
        }
    }

    // A node is a root if nothing claimed it as a child above.
    std::vector<bool> hasParent(m_nodes.size(), false);
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].parentIndex != kNoRef)
            hasParent[i] = true;
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
        if (!hasParent[i])
            m_roots.push_back(static_cast<int>(i));

    // Resolve BSLightingShaderProperty -> BSShaderTextureSet texture paths
    // now that every block (regardless of file order) has been parsed.
    for (auto& [propBlockIdx, texSetRef] : m_pendingMaterialTextureSetRef)
    {
        if (texSetRef < 0)
            continue;
        auto matIt = m_materials.find(propBlockIdx);
        auto texIt = m_textureSets.find(texSetRef);
        if (matIt == m_materials.end() || texIt == m_textureSets.end())
            continue;
        if (!texIt->second.empty())
            matIt->second.diffuseTexture = texIt->second[0];
        if (texIt->second.size() > 1)
            matIt->second.normalTexture = texIt->second[1];
    }

    // Apply alpha blending flag (bit 0 of AlphaFlags means "alpha blending
    // enabled", nif.xml AlphaFlags bitfield) onto materials referenced by a
    // shape whose Alpha Property points at a parsed NiAlphaProperty block.
    for (const auto& node : m_nodes)
    {
        if (!node.isShape || node.alphaPropertyIndex < 0)
            continue;
        auto flagIt = m_pendingAlphaFlags.find(node.alphaPropertyIndex);
        if (flagIt == m_pendingAlphaFlags.end())
            continue;
        auto matIt = m_materials.find(node.shaderPropertyIndex);
        if (matIt != m_materials.end())
            matIt->second.hasAlphaBlend = (flagIt->second & 0x1) != 0;
    }

    // Fall back to a skin partition's reconstructed geometry for skinned
    // shapes whose own vertex buffer/geometry data block came back empty
    // (Data Size == 0 - see parseBSTriShape/NifSceneNode::skinInstanceRef
    // and parseNiSkinPartition's scope note). Deferred to here, after every
    // block has been parsed, for the same file-order-independence reason as
    // the texture/alpha resolution above.
    for (auto& node : m_nodes)
    {
        if (!node.isShape || node.skinInstanceRef < 0)
            continue;

        bool ownGeometryEmpty;
        if (node.geometryBlockIndex != kNoRef)
        {
            auto geoIt = m_geometries.find(node.geometryBlockIndex);
            ownGeometryEmpty = (geoIt == m_geometries.end() ||
                geoIt->second.positions.empty() || geoIt->second.triangles.empty());
        }
        else
        {
            ownGeometryEmpty = node.inlineGeometry.positions.empty() || node.inlineGeometry.triangles.empty();
        }
        if (!ownGeometryEmpty)
            continue;

        auto partRefIt = m_skinInstanceToPartitionRef.find(node.skinInstanceRef);
        if (partRefIt == m_skinInstanceToPartitionRef.end() || partRefIt->second < 0)
            continue;
        auto partGeoIt = m_skinPartitionGeometries.find(partRefIt->second);
        if (partGeoIt == m_skinPartitionGeometries.end() ||
            partGeoIt->second.positions.empty() || partGeoIt->second.triangles.empty())
            continue;

        NIFLOG_INFO("NifDocument: shape '{}' (block {}) had no own geometry - using NiSkinPartition "
            "block {} instead ({} vert(s), {} tri(s))",
            node.name, node.blockIndex, partRefIt->second,
            partGeoIt->second.positions.size(), partGeoIt->second.triangles.size());

        node.inlineGeometry = partGeoIt->second; // copy: multiple shapes could in principle share a skin instance
        node.geometryBlockIndex = kNoRef;        // SceneBuilder reads inlineGeometry uniformly once this is kNoRef
    }
}

} // namespace nsk
