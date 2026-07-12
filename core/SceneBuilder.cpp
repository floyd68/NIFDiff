#include "SceneBuilder.h"
#include "NifLog.h"
#include <vector>

namespace nsk
{

namespace
{
    // Resolves node[i]'s world-space Transform by composing up the
    // parentIndex chain, memoizing results and guarding against malformed
    // cyclic parent links (should never happen in a real NIF, but a
    // corrupt/hand-edited file should not be allowed to hang the viewer).
    Transform computeWorldTransform(
        const std::vector<NifSceneNode>& nodes,
        int index,
        std::vector<Transform>& worldCache,
        std::vector<std::int8_t>& state) // 0 = unvisited, 1 = in-progress, 2 = done
    {
        if (state[static_cast<std::size_t>(index)] == 2)
            return worldCache[static_cast<std::size_t>(index)];

        const NifSceneNode& node = nodes[static_cast<std::size_t>(index)];
        Transform world = node.localTransform;

        if (node.parentIndex != kNoRef && state[static_cast<std::size_t>(node.parentIndex)] != 1)
        {
            state[static_cast<std::size_t>(index)] = 1;
            Transform parentWorld = computeWorldTransform(nodes, node.parentIndex, worldCache, state);
            world = parentWorld * node.localTransform;
        }

        worldCache[static_cast<std::size_t>(index)] = world;
        state[static_cast<std::size_t>(index)] = 2;
        return world;
    }

    // Bethesda/Gamebryo NIF files are authored Z-up. NifSkope's own default
    // ("Up Axis" = Z, glview.cpp's cfg.upAxis == ZAxis) leaves the scene data
    // untouched and instead points its camera presets at the Z-up model
    // directly (e.g. "Front" = setRotation(-90, 0, 180)) - i.e. NifSkope's
    // *camera* is what's really doing the axis compensation, not a scene
    // transform. This renderer's Camera (Camera.h) is a plain Y-up orbit
    // camera with no such per-preset compensation, so the equivalent fix is
    // applied once here instead, baked into every mesh's world transform: a
    // fixed -90 degree rotation about X maps NIF (X,Y,Z) = (right, forward,
    // up) onto this renderer's (X,Y,Z) = (right, up, forward) convention -
    // i.e. (x,y,z) -> (x,z,-y), a proper rotation (det=+1), not a mirror, so
    // it does not affect winding/backface culling. With this alone the
    // default camera faces the model's front, matching NifSkope's default
    // view of the same file (verified side-by-side against a live NifSkope
    // render). NifViewport::RebuildScene frames the camera from these same
    // corrected worldTransforms, so Camera itself needs no changes.
    Matrix4 axisCorrectionZupToYup()
    {
        Matrix4 m; // identity
        m(0, 0) = 1.0f; m(0, 1) = 0.0f; m(0, 2) = 0.0f;
        m(1, 0) = 0.0f; m(1, 1) = 0.0f; m(1, 2) = 1.0f;
        m(2, 0) = 0.0f; m(2, 1) = -1.0f; m(2, 2) = 0.0f;
        return m;
    }

    // Composes node[index]'s local transforms up the parent chain, EXCLUDING
    // the top-level root node's own transform - the exact semantic of
    // NifSkope's Node::localTrans(root) with root = the scene root, which is
    // what its BSShape skinning path uses (see applySkinning below). For the
    // common case (bone is a direct child of the scene root) this is just
    // the bone's own local transform.
    Transform localTransExcludingRoot(const std::vector<NifSceneNode>& nodes, int index)
    {
        Transform t;
        int guard = 0;
        while (index != kNoRef && nodes[static_cast<std::size_t>(index)].parentIndex != kNoRef && guard++ < 256)
        {
            t = nodes[static_cast<std::size_t>(index)].localTransform * t;
            index = nodes[static_cast<std::size_t>(index)].parentIndex;
        }
        return t;
    }

    // Static matrix-palette skinning for BSTriShape-with-NiSkinPartition
    // shapes, exactly per NifSkope's BSShape::transformShapes()
    // (src/gl/bsshape.cpp lines 291-327, the code path NifSkope actually
    // uses for these shapes - NOT glmesh.cpp, which handles only the legacy
    // NiTriShape family):
    //   t = scene->view * bone->localTrans(0) * bw.trans;   // view dropped here (world space wanted)
    //   transVerts[v]    += t * verts[v] * w.weight;
    //   transNorms[v]    += t.rotation * norms[v] * w.weight;    // then normalized
    //   transTangents[v] += t.rotation * tangents[v] * w.weight; // then normalized
    // where bw.trans is the bone's NiSkinData BoneData transform (index-
    // aligned with NiSkinInstance's Bones[]), the per-vertex weights/bone
    // indices come embedded in the NiSkinPartition global vertex buffer
    // (indices referencing Bones[] directly - bsshape.cpp lines 249-263),
    // and neither NiSkinData's overall Skin Transform nor the shape's own
    // scene-graph transform participates. The skinned result is world-space
    // (NifSkope draws it under scene->view alone), so build() applies only
    // the axis correction on top, not worldCache[i].
    //
    // Historical note: earlier attempts at this exact formula appeared to
    // "explode" the mesh (~2.5x inflated, torn at the arms), which led to a
    // detour of treating the raw buffer as pre-posed geometry with an ad-hoc
    // axis permutation. The real culprit was never this formula: NifIStream's
    // multi-component readers evaluated their reads as unordered function
    // arguments (MSVC: right-to-left), silently swapping X<->Z in every
    // Vector3/Triangle while matrix reads (explicit loop) stayed correct -
    // see NifStream.h's comment. With that fixed, this formula reproduces
    // NifSkope's output, verified against a live NifSkope 2.0 Dev 7 render
    // of the same file with Do Skinning toggled both ways.
    //
    // Returns nullptr if the skin data this needs isn't fully resolvable, so
    // the caller falls back to the ordinary rigid path.
    std::shared_ptr<NifGeometry> applySkinning(const NifDocument& doc, const NifSceneNode& node, const NifGeometry& geo)
    {
        const auto& weightsMap = doc.skinPartitionWeights();
        auto weightsIt = weightsMap.find(node.skinPartitionRef);
        if (weightsIt == weightsMap.end() || weightsIt->second.size() != geo.positions.size())
            return nullptr;

        auto bonesIt = doc.skinInstanceBones().find(node.skinInstanceRef);
        auto dataRefIt = doc.skinInstanceToDataRef().find(node.skinInstanceRef);
        if (bonesIt == doc.skinInstanceBones().end() || dataRefIt == doc.skinInstanceToDataRef().end())
            return nullptr;
        auto skinDataIt = doc.skinData().find(dataRefIt->second);
        if (skinDataIt == doc.skinData().end())
            return nullptr;
        const NifSkinData& skinData = skinDataIt->second;
        const std::vector<std::int32_t>& boneBlocks = bonesIt->second;
        if (skinData.boneOffsets.size() != boneBlocks.size())
            return nullptr; // malformed/truncated file - bail rather than index out of range

        std::vector<Transform> perBoneTransform(boneBlocks.size());
        for (std::size_t b = 0; b < boneBlocks.size(); ++b)
        {
            int boneNodeIdx = doc.nodeIndexForBlock(boneBlocks[b]);
            Transform boneTrans; // identity if unresolved - matches NifSkope skipping unfound bones' contributions in effect
            if (boneNodeIdx != kNoRef)
                boneTrans = localTransExcludingRoot(doc.nodes(), boneNodeIdx);
            perBoneTransform[b] = boneTrans * skinData.boneOffsets[b];
        }

        auto skinned = std::make_shared<NifGeometry>(geo); // copies uvs/colors/triangles as-is; positions/normals/tangents overwritten below
        const std::vector<NifVertexSkinWeights>& weights = weightsIt->second;
        const bool hasNormals = !geo.normals.empty();
        const bool hasTangents = !geo.tangents.empty();
        for (std::size_t v = 0; v < geo.positions.size(); ++v)
        {
            const NifVertexSkinWeights& vw = weights[v];
            Vector3 posAccum(0.0f, 0.0f, 0.0f);
            Vector3 normAccum(0.0f, 0.0f, 0.0f);
            Vector3 tanAccum(0.0f, 0.0f, 0.0f);
            float totalWeight = 0.0f;
            for (int slot = 0; slot < 4; ++slot)
            {
                float w = vw.weight[slot];
                if (w <= 0.0f)
                    continue;
                std::uint16_t bi = vw.boneIndex[slot];
                if (bi >= perBoneTransform.size())
                    continue;
                const Transform& t = perBoneTransform[bi];
                posAccum += t * geo.positions[v] * w;
                if (hasNormals)
                    normAccum += (t.rotation * geo.normals[v]) * w;
                if (hasTangents)
                    tanAccum += (t.rotation * geo.tangents[v]) * w;
                totalWeight += w;
            }
            // totalWeight should always be ~1 for a well-formed export
            // (verified against real content); a genuinely unweighted stray
            // vertex has no bone to place it via, so it's left at its raw
            // stored position/normal rather than guessing.
            if (totalWeight > 0.0f)
            {
                skinned->positions[v] = posAccum;
                if (hasNormals)  { normAccum.normalize(); skinned->normals[v] = normAccum; }
                if (hasTangents) { tanAccum.normalize();  skinned->tangents[v] = tanAccum; }
            }
        }
        return skinned;
    }
}

std::vector<RenderMesh> SceneBuilder::build(const NifDocument& doc)
{
    std::vector<RenderMesh> out;
    const std::vector<NifSceneNode>& nodes = doc.nodes();
    if (nodes.empty())
        return out;

    std::vector<Transform> worldCache(nodes.size());
    std::vector<std::int8_t> state(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        computeWorldTransform(nodes, static_cast<int>(i), worldCache, state);

    const auto& geometries = doc.geometries();
    const auto& materials = doc.materials();
    const Matrix4 axisCorrection = axisCorrectionZupToYup();

    std::size_t shapeNodeCount = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const NifSceneNode& node = nodes[i];
        if (!node.isShape)
            continue;
        ++shapeNodeCount;

        const NifGeometry* geo = nullptr;
        if (node.geometryBlockIndex != kNoRef)
        {
            auto it = geometries.find(node.geometryBlockIndex);
            if (it != geometries.end())
                geo = &it->second;
        }
        else
        {
            geo = &node.inlineGeometry; // BSTriShape family: geometry lives on the node itself
        }

        if (!geo || geo->positions.empty() || geo->triangles.empty())
        {
            // Nothing to draw (e.g. a collision-only shape, or a shape whose
            // *Data block referenced by geometryBlockIndex was never parsed -
            // see NifDocument::parseBlocks's "unrecognised geometry-like
            // type" warning for that case).
            NIFLOG_WARN("SceneBuilder: shape '{}' (block {}) skipped - {}", node.name, node.blockIndex,
                !geo ? "no geometry data resolved (geometryBlockIndex not found)"
                     : (geo->positions.empty() ? "empty positions" : "empty triangles"));
            continue;
        }

        RenderMesh mesh;
        mesh.nodeName = node.name;
        mesh.geometry = geo;

        if (node.hasSkinWeights)
            mesh.ownedGeometry = applySkinning(doc, node, *geo);

        if (mesh.ownedGeometry)
        {
            // applySkinning() already produced world-space (document-native,
            // pre axis-correction) positions - see its comment - so only the
            // axis correction is left to apply here, not worldCache[i] (a
            // skinned shape's own scene-graph transform does not apply to
            // its NiSkinPartition-sourced geometry).
            mesh.geometry = mesh.ownedGeometry.get();
            mesh.worldTransform = axisCorrection;
        }
        else
        {
            mesh.worldTransform = axisCorrection * worldCache[i].toMatrix4();
        }

        auto matIt = materials.find(node.shaderPropertyIndex);
        if (matIt == materials.end())
            matIt = materials.find(node.materialPropertyIndex);
        mesh.material = (matIt != materials.end()) ? matIt->second : defaultMaterial();

        NIFLOG_TRACE("SceneBuilder: shape '{}' (block {}) -> {} vert(s), {} tri(s)",
            node.name, node.blockIndex, geo->positions.size(), geo->triangles.size());

        out.push_back(std::move(mesh));
    }

    NIFLOG_INFO("SceneBuilder::build: {} shape node(s) considered, {} render mesh(es) produced",
        shapeNodeCount, out.size());

    return out;
}

} // namespace nsk
