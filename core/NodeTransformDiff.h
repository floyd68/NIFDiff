#pragma once

#include "NifDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace nsk
{

struct NodeTransformTolerance
{
    float translation = 1.0e-4f;
    float rotationDegrees = 0.01f;
    float scale = 1.0e-5f;
};

struct NodeTransformSnapshot
{
    int nodeIndex = kNoRef;
    std::int32_t blockIndex = kNoRef;
    std::string hierarchyKey;
    std::string parentKey;
    std::string displayPath;
    std::string parentDisplayPath;
    std::string nodeName;
    std::string normalizedName;
    std::string blockType;
    int depth = 0;
    bool isShape = false;
    bool transformValid = true;
    Transform localTransform;
};

struct NodeTransformSnapshotSet
{
    std::wstring label;
    std::wstring sourcePath;
    std::vector<NodeTransformSnapshot> nodes;
};

enum class NodeMatchStatus
{
    Equal,
    Changed,
    Added,
    Removed,
    Reparented,
    Ambiguous,
    NotPresent,
    Invalid
};

struct NodeTransformDelta
{
    Vector3 translation;
    float translationLength = 0.0f;
    float rotationDegrees = 0.0f;
    float scale = 0.0f;
    bool translationChanged = false;
    bool rotationChanged = false;
    bool scaleChanged = false;
    bool valid = true;

    bool changed() const
    {
        return !valid ||
            translationChanged ||
            rotationChanged ||
            scaleChanged;
    }
};

struct NodeTransformCell
{
    std::optional<NodeTransformSnapshot> node;
    std::optional<NodeTransformDelta> delta;
    NodeMatchStatus status = NodeMatchStatus::NotPresent;
};

struct NodeTransformDiffRow
{
    std::string displayPath;
    int depth = 0;
    std::vector<NodeTransformCell> cells;

    bool differs() const;
};

struct NodeTransformDiffReport
{
    std::size_t baselinePane = 0;
    NodeTransformTolerance tolerance;
    std::vector<NodeTransformSnapshotSet> panes;
    std::vector<NodeTransformDiffRow> rows;
};

NodeTransformSnapshotSet BuildNodeTransformSnapshotSet(
    const NifDocument& document,
    const std::wstring& label);

NodeTransformSnapshotSet BuildNodeTransformSnapshotSetFromNodes(
    const std::vector<NifSceneNode>& nodes,
    const std::vector<std::string>& blockTypes,
    const std::wstring& label,
    const std::wstring& sourcePath = {});

bool IsFiniteTransform(
    const Transform& transform);

NodeTransformDelta CompareNodeTransforms(
    const Transform& baseline,
    const Transform& comparison,
    const NodeTransformTolerance& tolerance = {});

NodeTransformDiffReport BuildNodeTransformDiffReport(
    std::vector<NodeTransformSnapshotSet> panes,
    std::size_t baselinePane,
    const NodeTransformTolerance& tolerance = {});

const wchar_t* NodeMatchStatusName(
    NodeMatchStatus status);

} // namespace nsk
