#include "../core/NodeTransformExport.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace
{

std::string ReadAll(
    const std::filesystem::path& path)
{
    std::ifstream input(
        path,
        std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool Contains(
    const std::string& value,
    const std::string& expected,
    const char* message)
{
    if (value.find(expected) !=
        std::string::npos)
    {
        return true;
    }
    std::cout
        << "FAIL: "
        << message
        << '\n';
    return false;
}

} // namespace

int main()
{
    nsk::NodeTransformSnapshot baselineNode;
    baselineNode.displayPath =
        "Root/Bone,\"A\"";
    baselineNode.nodeName =
        std::string(
            reinterpret_cast<const char*>(
                u8"뼈")) +
        ",\"A\"";
    baselineNode.blockType =
        "NiNode";
    baselineNode.localTransform.translation =
        nsk::Vector3(
            1.0f,
            2.0f,
            3.0f);

    nsk::NodeTransformSnapshot comparisonNode =
        baselineNode;
    comparisonNode.localTransform.translation =
        nsk::Vector3(
            2.0f,
            2.0f,
            3.0f);

    nsk::NodeTransformDiffReport report;
    report.baselinePane = 0;
    report.panes.resize(3);
    report.panes[0].label = L"기준";
    report.panes[0].sourcePath =
        L"C:\\Meshes\\base,one.nif";
    report.panes[1].label = L"Comparison";
    report.panes[1].sourcePath =
        L"C:\\Meshes\\other.nif";
    report.panes[2].label = L"Empty";
    report.panes[2].sourcePath =
        L"C:\\Meshes\\empty.nif";

    nsk::NodeTransformDiffRow row;
    row.displayPath =
        baselineNode.displayPath;
    row.cells.resize(3);
    row.cells[0].node =
        baselineNode;
    row.cells[0].status =
        nsk::NodeMatchStatus::Equal;
    row.cells[1].node =
        comparisonNode;
    row.cells[1].delta =
        nsk::CompareNodeTransforms(
            baselineNode.localTransform,
            comparisonNode.localTransform);
    row.cells[1].status =
        nsk::NodeMatchStatus::Changed;
    row.cells[2].status =
        nsk::NodeMatchStatus::NotPresent;
    report.rows.push_back(
        std::move(row));

    nsk::NodeTransformSnapshot invalidNode =
        baselineNode;
    invalidNode.displayPath =
        "Root/Invalid";
    invalidNode.localTransform.scale =
        (std::numeric_limits<float>::infinity)();
    invalidNode.transformValid = false;
    nsk::NodeTransformDiffRow invalidRow;
    invalidRow.displayPath =
        invalidNode.displayPath;
    invalidRow.cells.resize(3);
    invalidRow.cells[0].node =
        invalidNode;
    invalidRow.cells[0].status =
        nsk::NodeMatchStatus::Invalid;
    invalidRow.cells[1].node =
        invalidNode;
    invalidRow.cells[1].status =
        nsk::NodeMatchStatus::Invalid;
    invalidRow.cells[1].delta =
        nsk::CompareNodeTransforms(
            invalidNode.localTransform,
            invalidNode.localTransform);
    invalidRow.cells[2].status =
        nsk::NodeMatchStatus::NotPresent;
    report.rows.push_back(
        std::move(invalidRow));

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path();
    const std::filesystem::path csvPath =
        directory /
        "nifdiff-node-transform-test.csv";
    const std::filesystem::path jsonPath =
        directory /
        "nifdiff-node-transform-test.json";

    std::string error;
    bool ok =
        nsk::ExportNodeTransformCsv(
            report,
            csvPath.wstring(),
            &error) &&
        nsk::ExportNodeTransformJson(
            report,
            jsonPath.wstring(),
            &error);
    if (!ok)
    {
        std::cout
            << "FAIL: "
            << error
            << '\n';
        return 1;
    }

    const std::string csv =
        ReadAll(csvPath);
    const std::string json =
        ReadAll(jsonPath);
    ok &=
        csv.size() >= 3 &&
        static_cast<unsigned char>(csv[0]) ==
            0xEF &&
        static_cast<unsigned char>(csv[1]) ==
            0xBB &&
        static_cast<unsigned char>(csv[2]) ==
            0xBF;
    ok &= Contains(
        csv,
        "\"C:\\Meshes\\base,one.nif\"",
        "CSV comma quoting");
    ok &= Contains(
        csv,
        "\"Root/Bone,\"\"A\"\"\"",
        "CSV quote escaping");
    ok &= Contains(
        json,
        "\"formatVersion\":1",
        "JSON version");
    ok &= Contains(
        json,
        "\\\"A\\\"",
        "JSON quote escaping");
    ok &= Contains(
        json,
        reinterpret_cast<const char*>(
            u8"뼈"),
        "JSON UTF-8 node name");
    ok &= Contains(
        csv,
        "Not Present",
        "three-pane not-present status");
    ok &= Contains(
        json,
        "\"valid\":false",
        "invalid transform marker");
    ok &=
        json.find("nan") ==
            std::string::npos &&
        json.find("inf") ==
            std::string::npos;
    ok &= Contains(
        json,
        "\"rotationDegrees\"",
        "JSON transform delta");

    std::error_code ignored;
    std::filesystem::remove(
        csvPath,
        ignored);
    std::filesystem::remove(
        jsonPath,
        ignored);

    if (!ok)
    {
        return 1;
    }
    std::cout
        << "All node transform export tests passed.\n";
    return 0;
}
