#include "../core/NodeTransformDiff.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>

namespace
{

nsk::NodeTransformSnapshot MakeNode(
    std::string key,
    std::string parent,
    std::string name,
    std::string path,
    float tx = 0.0f)
{
    nsk::NodeTransformSnapshot node;
    node.hierarchyKey = std::move(key);
    node.parentKey = std::move(parent);
    node.nodeName = name;
    node.normalizedName = std::move(name);
    node.displayPath = std::move(path);
    node.blockType = "NiNode";
    node.localTransform.translation =
        nsk::Vector3(tx, 0.0f, 0.0f);
    return node;
}

const nsk::NodeTransformDiffRow* FindRow(
    const nsk::NodeTransformDiffReport& report,
    const std::string& displayPath)
{
    for (const nsk::NodeTransformDiffRow& row :
         report.rows)
    {
        if (row.displayPath == displayPath)
        {
            return &row;
        }
    }
    return nullptr;
}

bool Expect(
    bool condition,
    const char* message)
{
    if (!condition)
    {
        std::cout
            << "FAIL: "
            << message
            << '\n';
    }
    return condition;
}

} // namespace

int main()
{
    nsk::NodeTransformSnapshotSet baseline;
    baseline.label = L"Baseline";
    baseline.nodes.push_back(
        MakeNode(
            "root",
            "",
            "root",
            "Root"));
    baseline.nodes.push_back(
        MakeNode(
            "root/spine",
            "root",
            "spine",
            "Root/Spine"));
    baseline.nodes.push_back(
        MakeNode(
            "root/dup0",
            "root",
            "bone",
            "Root/Bone[0]"));
    baseline.nodes.push_back(
        MakeNode(
            "root/dup1",
            "root",
            "bone",
            "Root/Bone[1]"));
    baseline.nodes.push_back(
        MakeNode(
            "root/missing",
            "root",
            "missing",
            "Root/Missing"));

    nsk::NodeTransformSnapshotSet comparison;
    comparison.label = L"Comparison";
    comparison.nodes.push_back(
        MakeNode(
            "root",
            "",
            "root",
            "Root",
            1.0f));
    comparison.nodes.push_back(
        MakeNode(
            "other/spine",
            "other",
            "spine",
            "Other/Spine"));
    comparison.nodes.push_back(
        MakeNode(
            "other/bone",
            "other",
            "bone",
            "Other/Bone"));
    comparison.nodes.push_back(
        MakeNode(
            "root/added",
            "root",
            "added",
            "Root/Added"));

    nsk::NodeTransformDiffReport report =
        nsk::BuildNodeTransformDiffReport(
            {
                baseline,
                comparison,
                nsk::NodeTransformSnapshotSet {
                    L"Empty",
                    L"",
                    {},
                },
            },
            0);

    const nsk::NodeTransformDiffRow* root =
        FindRow(report, "Root");
    const nsk::NodeTransformDiffRow* spine =
        FindRow(report, "Root/Spine");
    const nsk::NodeTransformDiffRow* duplicate =
        FindRow(report, "Root/Bone[0]");
    const nsk::NodeTransformDiffRow* missing =
        FindRow(report, "Root/Missing");
    const nsk::NodeTransformDiffRow* added =
        FindRow(report, "Root/Added");

    bool ok = true;
    ok &= Expect(
        root != nullptr &&
            root->cells[1].status ==
                nsk::NodeMatchStatus::Changed &&
            root->cells[1].delta.has_value() &&
            std::abs(
                root->cells[1]
                    .delta->translationLength -
                1.0f) <
                1.0e-6f,
        "translation delta");
    ok &= Expect(
        spine != nullptr &&
            spine->cells[1].status ==
                nsk::NodeMatchStatus::Reparented,
        "unique reparent match");
    ok &= Expect(
        duplicate != nullptr &&
            duplicate->cells[1].status ==
                nsk::NodeMatchStatus::Ambiguous,
        "duplicate name ambiguity");
    ok &= Expect(
        missing != nullptr &&
            missing->cells[1].status ==
                nsk::NodeMatchStatus::Removed,
        "removed node");
    ok &= Expect(
        added != nullptr &&
            added->cells[1].status ==
                nsk::NodeMatchStatus::Added &&
            added->cells[2].status ==
                nsk::NodeMatchStatus::NotPresent &&
            !added->cells[2].node.has_value(),
        "three-pane added/not-present status");

    nsk::Transform rotated;
    rotated.rotation =
        nsk::Matrix::euler(
            0.0f,
            0.0f,
            std::numbers::pi_v<float> /
                2.0f);
    const nsk::NodeTransformDelta rotationDelta =
        nsk::CompareNodeTransforms(
            nsk::Transform(),
            rotated);
    ok &= Expect(
        std::abs(
            rotationDelta.rotationDegrees -
            90.0f) <
            1.0e-3f,
        "quaternion angle delta");

    nsk::Transform invalid;
    invalid.translation[0] =
        (std::numeric_limits<float>::quiet_NaN)();
    const nsk::NodeTransformDelta invalidDelta =
        nsk::CompareNodeTransforms(
            nsk::Transform(),
            invalid);
    ok &= Expect(
        !invalidDelta.valid &&
            invalidDelta.changed(),
        "invalid transform classification");

    std::vector<nsk::NifSceneNode> parsedNodes(5);
    parsedNodes[0].name = "Root";
    parsedNodes[0].parentIndex = nsk::kNoRef;
    parsedNodes[1].name = "Bone";
    parsedNodes[1].parentIndex = 0;
    parsedNodes[2].name = "Bone";
    parsedNodes[2].parentIndex = 0;
    const std::string utf8Bone(
        reinterpret_cast<const char*>(
            u8"뼈"));
    parsedNodes[3].name = utf8Bone;
    parsedNodes[3].parentIndex = 4;
    parsedNodes[4].name = "Cycle";
    parsedNodes[4].parentIndex = 3;
    const nsk::NodeTransformSnapshotSet snapshots =
        nsk::BuildNodeTransformSnapshotSetFromNodes(
            parsedNodes,
            std::vector<std::string>(
                parsedNodes.size(),
                "NiNode"),
            L"Snapshot Test");
    ok &= Expect(
        snapshots.nodes.size() == 5 &&
            snapshots.nodes[1].displayPath ==
                "Root/Bone[0]" &&
            snapshots.nodes[2].displayPath ==
                "Root/Bone[1]" &&
            snapshots.nodes[1].hierarchyKey !=
                snapshots.nodes[2].hierarchyKey,
        "snapshot sibling key generation");
    ok &= Expect(
        snapshots.nodes[3].displayPath.find(
            utf8Bone) !=
                std::string::npos &&
            !snapshots.nodes[3]
                 .hierarchyKey.empty() &&
            !snapshots.nodes[4]
                 .hierarchyKey.empty(),
        "UTF-8 snapshot and hierarchy cycle guard");

    if (!ok)
    {
        return 1;
    }
    std::cout
        << "All node transform diff tests passed.\n";
    return 0;
}
