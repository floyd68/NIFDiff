#include "NodeTransformDiff.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <unordered_map>

namespace nsk
{
namespace
{

std::string LowerAscii(std::string value)
{
    for (char& c : value)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(
                c - 'A' + 'a');
        }
    }
    return value;
}

std::string SiblingBucket(
    int parentIndex,
    const std::string& normalizedName)
{
    return std::to_string(parentIndex) +
        ":" +
        std::to_string(normalizedName.size()) +
        ":" +
        normalizedName;
}

std::string AppendKeySegment(
    std::string key,
    const std::string& normalizedName,
    int siblingIndex)
{
    key += std::to_string(normalizedName.size());
    key += ":";
    key += normalizedName;
    key += "#";
    key += std::to_string(siblingIndex);
    key += "/";
    return key;
}

std::string MatchSignature(
    const NodeTransformSnapshot& node)
{
    return std::to_string(
               node.normalizedName.size()) +
        ":" +
        node.normalizedName +
        ":" +
        node.blockType;
}

float QuaternionDot(
    const Quat& lhs,
    const Quat& rhs)
{
    return lhs[0] * rhs[0] +
        lhs[1] * rhs[1] +
        lhs[2] * rhs[2] +
        lhs[3] * rhs[3];
}

using IndexMap =
    std::unordered_map<std::string, std::size_t>;
using CandidateMap =
    std::unordered_map<
        std::string,
        std::vector<std::size_t>>;

IndexMap BuildPathIndex(
    const NodeTransformSnapshotSet& pane)
{
    IndexMap result;
    for (std::size_t i = 0;
         i < pane.nodes.size();
         ++i)
    {
        result.emplace(
            pane.nodes[i].hierarchyKey,
            i);
    }
    return result;
}

CandidateMap BuildCandidateIndex(
    const NodeTransformSnapshotSet& pane)
{
    CandidateMap result;
    for (std::size_t i = 0;
         i < pane.nodes.size();
         ++i)
    {
        result[MatchSignature(
            pane.nodes[i])]
            .push_back(i);
    }
    return result;
}

} // namespace

bool NodeTransformDiffRow::differs() const
{
    for (const NodeTransformCell& cell : cells)
    {
        if (cell.status != NodeMatchStatus::Equal ||
            (cell.delta.has_value() &&
             cell.delta->changed()))
        {
            return true;
        }
    }
    return false;
}

NodeTransformSnapshotSet BuildNodeTransformSnapshotSet(
    const NifDocument& document,
    const std::wstring& label)
{
    std::vector<std::string> blockTypes;
    blockTypes.reserve(
        document.nodes().size());
    for (const NifSceneNode& node :
         document.nodes())
    {
        blockTypes.push_back(
            document.blockTypeName(
                node.blockIndex));
    }
    return BuildNodeTransformSnapshotSetFromNodes(
        document.nodes(),
        blockTypes,
        label,
        document.filePath());
}

NodeTransformSnapshotSet BuildNodeTransformSnapshotSetFromNodes(
    const std::vector<NifSceneNode>& nodes,
    const std::vector<std::string>& blockTypes,
    const std::wstring& label,
    const std::wstring& sourcePath)
{
    NodeTransformSnapshotSet result;
    result.label = label;
    result.sourcePath = sourcePath;
    std::vector<std::string> normalizedNames;
    std::vector<int> siblingIndices(
        nodes.size(),
        0);
    std::unordered_map<std::string, int> siblingCounts;
    normalizedNames.reserve(nodes.size());

    for (std::size_t i = 0;
         i < nodes.size();
         ++i)
    {
        normalizedNames.push_back(
            LowerAscii(nodes[i].name));
        const std::string bucket =
            SiblingBucket(
                nodes[i].parentIndex,
                normalizedNames.back());
        siblingIndices[i] =
            siblingCounts[bucket]++;
    }

    result.nodes.reserve(nodes.size());
    for (std::size_t i = 0;
         i < nodes.size();
         ++i)
    {
        std::vector<int> lineage;
        std::vector<bool> visited(
            nodes.size(),
            false);
        int current = static_cast<int>(i);
        while (current >= 0 &&
               static_cast<std::size_t>(current) <
                   nodes.size() &&
               !visited[static_cast<std::size_t>(
                   current)])
        {
            visited[static_cast<std::size_t>(
                current)] = true;
            lineage.push_back(current);
            current =
                nodes[static_cast<std::size_t>(
                    current)]
                    .parentIndex;
        }
        std::reverse(
            lineage.begin(),
            lineage.end());

        NodeTransformSnapshot snapshot;
        snapshot.nodeIndex =
            static_cast<int>(i);
        snapshot.blockIndex =
            nodes[i].blockIndex;
        snapshot.nodeName =
            nodes[i].name;
        snapshot.normalizedName =
            normalizedNames[i];
        snapshot.blockType =
            i < blockTypes.size()
                ? blockTypes[i]
                : std::string();
        snapshot.depth =
            lineage.empty()
                ? 0
                : static_cast<int>(
                      lineage.size() - 1);
        snapshot.isShape =
            nodes[i].isShape;
        snapshot.localTransform =
            nodes[i].localTransform;
        snapshot.transformValid =
            IsFiniteTransform(
                snapshot.localTransform);

        for (std::size_t segment = 0;
             segment < lineage.size();
             ++segment)
        {
            const int lineageIndex =
                lineage[segment];
            const std::size_t nodeOffset =
                static_cast<std::size_t>(
                    lineageIndex);
            const std::string& normalized =
                normalizedNames[nodeOffset];
            if (segment + 1 ==
                lineage.size())
            {
                snapshot.parentKey =
                    snapshot.hierarchyKey;
                snapshot.parentDisplayPath =
                    snapshot.displayPath;
            }
            snapshot.hierarchyKey =
                AppendKeySegment(
                    std::move(
                        snapshot.hierarchyKey),
                    normalized,
                    siblingIndices[nodeOffset]);

            if (!snapshot.displayPath.empty())
            {
                snapshot.displayPath += "/";
            }
            snapshot.displayPath +=
                nodes[nodeOffset].name.empty()
                    ? "(unnamed)"
                    : nodes[nodeOffset].name;
            const std::string bucket =
                SiblingBucket(
                    nodes[nodeOffset].parentIndex,
                    normalized);
            if (siblingCounts[bucket] > 1)
            {
                snapshot.displayPath +=
                    std::format(
                        "[{}]",
                        siblingIndices[nodeOffset]);
            }
        }
        result.nodes.push_back(
            std::move(snapshot));
    }
    return result;
}

bool IsFiniteTransform(
    const Transform& transform)
{
    for (unsigned i = 0; i < 3; ++i)
    {
        if (!std::isfinite(
                transform.translation[i]))
        {
            return false;
        }
        for (unsigned j = 0; j < 3; ++j)
        {
            if (!std::isfinite(
                    transform.rotation(i, j)))
            {
                return false;
            }
        }
    }
    if (!std::isfinite(
            transform.scale))
    {
        return false;
    }
    const Quat rotation =
        transform.rotation.toQuat();
    for (unsigned i = 0; i < 4; ++i)
    {
        if (!std::isfinite(rotation[i]))
        {
            return false;
        }
    }
    return true;
}

NodeTransformDelta CompareNodeTransforms(
    const Transform& baseline,
    const Transform& comparison,
    const NodeTransformTolerance& tolerance)
{
    NodeTransformDelta delta;
    if (!IsFiniteTransform(baseline) ||
        !IsFiniteTransform(comparison))
    {
        delta.valid = false;
        return delta;
    }
    delta.translation =
        comparison.translation -
        baseline.translation;
    delta.translationLength =
        std::sqrt(
            delta.translation[0] *
                delta.translation[0] +
            delta.translation[1] *
                delta.translation[1] +
            delta.translation[2] *
                delta.translation[2]);
    delta.scale =
        comparison.scale -
        baseline.scale;

    const Quat baselineRotation =
        baseline.rotation.toQuat();
    const Quat comparisonRotation =
        comparison.rotation.toQuat();
    for (unsigned i = 0; i < 4; ++i)
    {
        if (!std::isfinite(
                baselineRotation[i]) ||
            !std::isfinite(
                comparisonRotation[i]))
        {
            delta.valid = false;
            return delta;
        }
    }
    const float dot =
        (std::clamp)(
            std::abs(
                QuaternionDot(
                    baselineRotation,
                    comparisonRotation)),
            0.0f,
            1.0f);
    delta.rotationDegrees =
        2.0f *
        std::acos(dot) *
        180.0f /
        std::numbers::pi_v<float>;
    if (!std::isfinite(
            delta.translationLength) ||
        !std::isfinite(
            delta.rotationDegrees) ||
        !std::isfinite(delta.scale))
    {
        delta.valid = false;
        return delta;
    }

    delta.translationChanged =
        delta.translationLength >
        tolerance.translation;
    delta.rotationChanged =
        delta.rotationDegrees >
        tolerance.rotationDegrees;
    delta.scaleChanged =
        std::abs(delta.scale) >
        tolerance.scale;
    return delta;
}

NodeTransformDiffReport BuildNodeTransformDiffReport(
    std::vector<NodeTransformSnapshotSet> panes,
    std::size_t baselinePane,
    const NodeTransformTolerance& tolerance)
{
    NodeTransformDiffReport report;
    report.tolerance = tolerance;
    report.panes = std::move(panes);
    if (report.panes.empty())
    {
        return report;
    }
    report.baselinePane =
        (std::min)(
            baselinePane,
            report.panes.size() - 1);

    const NodeTransformSnapshotSet& baseline =
        report.panes[report.baselinePane];
    std::vector<IndexMap> pathIndices;
    std::vector<CandidateMap> candidateIndices;
    std::vector<std::vector<bool>> used;
    pathIndices.reserve(report.panes.size());
    candidateIndices.reserve(report.panes.size());
    used.reserve(report.panes.size());
    for (const NodeTransformSnapshotSet& pane :
         report.panes)
    {
        pathIndices.push_back(
            BuildPathIndex(pane));
        candidateIndices.push_back(
            BuildCandidateIndex(pane));
        used.emplace_back(
            pane.nodes.size(),
            false);
    }

    const CandidateMap& baselineCandidates =
        candidateIndices[report.baselinePane];
    for (std::size_t baselineIndex = 0;
         baselineIndex < baseline.nodes.size();
         ++baselineIndex)
    {
        const NodeTransformSnapshot& baselineNode =
            baseline.nodes[baselineIndex];
        NodeTransformDiffRow row;
        row.displayPath =
            baselineNode.displayPath;
        row.depth =
            baselineNode.depth;
        row.cells.resize(
            report.panes.size());
        row.cells[report.baselinePane].node =
            baselineNode;
        row.cells[report.baselinePane].status =
            baselineNode.transformValid
                ? NodeMatchStatus::Equal
                : NodeMatchStatus::Invalid;
        used[report.baselinePane][baselineIndex] =
            true;

        const std::string signature =
            MatchSignature(baselineNode);
        const auto baselineSignatureIt =
            baselineCandidates.find(signature);
        const bool baselineSignatureUnique =
            baselineSignatureIt !=
                baselineCandidates.end() &&
            baselineSignatureIt->second.size() == 1;

        for (std::size_t paneIndex = 0;
             paneIndex < report.panes.size();
             ++paneIndex)
        {
            if (paneIndex ==
                report.baselinePane)
            {
                continue;
            }

            const NodeTransformSnapshot* matched =
                nullptr;
            std::size_t matchedIndex = 0;
            bool reparented = false;
            const auto exactIt =
                pathIndices[paneIndex].find(
                    baselineNode.hierarchyKey);
            if (exactIt !=
                pathIndices[paneIndex].end())
            {
                matchedIndex =
                    exactIt->second;
                matched =
                    &report.panes[paneIndex]
                         .nodes[matchedIndex];
            }
            else
            {
                const auto candidateIt =
                    candidateIndices[paneIndex]
                        .find(signature);
                if (baselineSignatureUnique &&
                    candidateIt !=
                        candidateIndices[paneIndex]
                            .end() &&
                    candidateIt->second.size() == 1)
                {
                    matchedIndex =
                        candidateIt->second.front();
                    matched =
                        &report.panes[paneIndex]
                             .nodes[matchedIndex];
                    reparented =
                        matched->parentKey !=
                        baselineNode.parentKey;
                }
                else if (
                    candidateIt !=
                        candidateIndices[paneIndex]
                            .end() &&
                    !candidateIt->second.empty())
                {
                    row.cells[paneIndex].status =
                        NodeMatchStatus::Ambiguous;
                }
            }

            if (matched == nullptr)
            {
                if (row.cells[paneIndex].status !=
                    NodeMatchStatus::Ambiguous)
                {
                    row.cells[paneIndex].status =
                        NodeMatchStatus::Removed;
                }
                continue;
            }

            used[paneIndex][matchedIndex] =
                true;
            NodeTransformCell& cell =
                row.cells[paneIndex];
            cell.node = *matched;
            cell.delta =
                CompareNodeTransforms(
                    baselineNode.localTransform,
                    matched->localTransform,
                    tolerance);
            cell.status =
                !cell.delta->valid
                    ? NodeMatchStatus::Invalid
                : reparented
                    ? NodeMatchStatus::Reparented
                    : (cell.delta->changed()
                           ? NodeMatchStatus::Changed
                           : NodeMatchStatus::Equal);
        }
        report.rows.push_back(
            std::move(row));
    }

    std::unordered_map<std::string, std::size_t>
        addedRows;
    for (std::size_t paneIndex = 0;
         paneIndex < report.panes.size();
         ++paneIndex)
    {
        if (paneIndex ==
            report.baselinePane)
        {
            continue;
        }
        const NodeTransformSnapshotSet& pane =
            report.panes[paneIndex];
        for (std::size_t nodeIndex = 0;
             nodeIndex < pane.nodes.size();
             ++nodeIndex)
        {
            if (used[paneIndex][nodeIndex])
            {
                continue;
            }
            const NodeTransformSnapshot& node =
                pane.nodes[nodeIndex];
            auto [it, inserted] =
                addedRows.emplace(
                    node.hierarchyKey,
                    report.rows.size());
            if (inserted)
            {
                NodeTransformDiffRow row;
                row.displayPath =
                    node.displayPath;
                row.depth =
                    node.depth;
                row.cells.resize(
                    report.panes.size());
                for (NodeTransformCell& cell :
                     row.cells)
                {
                    cell.status =
                        NodeMatchStatus::NotPresent;
                }
                report.rows.push_back(
                    std::move(row));
            }
            NodeTransformCell& cell =
                report.rows[it->second]
                    .cells[paneIndex];
            cell.node = node;
            cell.status =
                node.transformValid
                    ? NodeMatchStatus::Added
                    : NodeMatchStatus::Invalid;
        }
    }
    return report;
}

const wchar_t* NodeMatchStatusName(
    NodeMatchStatus status)
{
    switch (status)
    {
    case NodeMatchStatus::Equal:
        return L"Equal";
    case NodeMatchStatus::Changed:
        return L"Changed";
    case NodeMatchStatus::Added:
        return L"Added";
    case NodeMatchStatus::Removed:
        return L"Removed";
    case NodeMatchStatus::Reparented:
        return L"Reparented";
    case NodeMatchStatus::Ambiguous:
        return L"Ambiguous";
    case NodeMatchStatus::NotPresent:
        return L"Not Present";
    case NodeMatchStatus::Invalid:
        return L"Invalid";
    }
    return L"Unknown";
}

} // namespace nsk
