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
        mesh.worldTransform = worldCache[i].toMatrix4();
        mesh.geometry = geo;

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
