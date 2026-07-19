#include "NodeTransformExport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace nsk
{
namespace
{

std::string WideToUtf8(
    const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int count =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(
        static_cast<std::size_t>(count),
        '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count,
        nullptr,
        nullptr);
    return result;
}

std::string CsvField(
    const std::string& value)
{
    if (value.find_first_of(",\"\r\n") ==
        std::string::npos)
    {
        return value;
    }
    std::string result = "\"";
    for (char c : value)
    {
        if (c == '"')
        {
            result += "\"\"";
        }
        else
        {
            result += c;
        }
    }
    result += '"';
    return result;
}

std::string JsonString(
    const std::string& value)
{
    std::string result = "\"";
    for (const unsigned char c : value)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                result += std::format(
                    "\\u{:04x}",
                    static_cast<unsigned>(c));
            }
            else
            {
                result +=
                    static_cast<char>(c);
            }
            break;
        }
    }
    result += '"';
    return result;
}

std::string JsonString(
    const std::wstring& value)
{
    return JsonString(
        WideToUtf8(value));
}

std::string StatusUtf8(
    NodeMatchStatus status)
{
    return WideToUtf8(
        NodeMatchStatusName(status));
}

std::string CsvNumber(float value)
{
    return std::isfinite(value)
        ? std::format(
              "{:.9g}",
              value)
        : std::string();
}

std::string JsonNumber(float value)
{
    return std::isfinite(value)
        ? std::format(
              "{:.9g}",
              value)
        : std::string("null");
}

void AppendTransformCsv(
    std::vector<std::string>& fields,
    const std::optional<NodeTransformSnapshot>& node)
{
    if (!node ||
        !node->transformValid ||
        !IsFiniteTransform(
            node->localTransform))
    {
        for (int i = 0; i < 8; ++i)
        {
            fields.emplace_back();
        }
        return;
    }
    const Transform& transform =
        node->localTransform;
    const Quat rotation =
        transform.rotation.toQuat();
    fields.push_back(
        CsvNumber(transform.translation[0]));
    fields.push_back(
        CsvNumber(transform.translation[1]));
    fields.push_back(
        CsvNumber(transform.translation[2]));
    fields.push_back(
        CsvNumber(rotation[0]));
    fields.push_back(
        CsvNumber(rotation[1]));
    fields.push_back(
        CsvNumber(rotation[2]));
    fields.push_back(
        CsvNumber(rotation[3]));
    fields.push_back(
        CsvNumber(transform.scale));
}

void WriteTransformJson(
    std::ostream& out,
    const std::optional<NodeTransformSnapshot>& node)
{
    if (!node)
    {
        out << "null";
        return;
    }
    const Transform& transform =
        node->localTransform;
    const bool valid =
        node->transformValid &&
        IsFiniteTransform(transform);
    if (!valid)
    {
        out
            << "{\"path\":"
            << JsonString(node->displayPath)
            << ",\"parent\":"
            << JsonString(node->parentDisplayPath)
            << ",\"name\":"
            << JsonString(node->nodeName)
            << ",\"blockType\":"
            << JsonString(node->blockType)
            << ",\"valid\":false,\"translation\":null,"
               "\"rotationQuaternion\":null,\"scale\":null}";
        return;
    }
    const Quat rotation =
        transform.rotation.toQuat();
    out
        << "{\"path\":"
        << JsonString(node->displayPath)
        << ",\"parent\":"
        << JsonString(node->parentDisplayPath)
        << ",\"name\":"
        << JsonString(node->nodeName)
        << ",\"blockType\":"
        << JsonString(node->blockType)
        << ",\"valid\":true,\"translation\":["
        << JsonNumber(transform.translation[0])
        << ","
        << JsonNumber(transform.translation[1])
        << ","
        << JsonNumber(transform.translation[2])
        << "],\"rotationQuaternion\":["
        << JsonNumber(rotation[0])
        << ","
        << JsonNumber(rotation[1])
        << ","
        << JsonNumber(rotation[2])
        << ","
        << JsonNumber(rotation[3])
        << "],\"scale\":"
        << JsonNumber(transform.scale)
        << "}";
}

void SetError(
    std::string* errorOut,
    const std::string& message)
{
    if (errorOut != nullptr)
    {
        *errorOut = message;
    }
}

} // namespace

bool ExportNodeTransformCsv(
    const NodeTransformDiffReport& report,
    const std::wstring& path,
    std::string* errorOut)
{
    try
    {
        std::ofstream out(
            std::filesystem::path(path),
            std::ios::binary |
                std::ios::trunc);
        if (!out)
        {
            SetError(
                errorOut,
                "Could not open the CSV output file.");
            return false;
        }
        out.write("\xEF\xBB\xBF", 3);
        out
            << "baseline_file,comparison_file,status,hierarchy,"
               "node_name,block_type,baseline_parent,comparison_parent,"
               "baseline_valid,comparison_valid,"
               "baseline_tx,baseline_ty,baseline_tz,"
               "baseline_qw,baseline_qx,baseline_qy,baseline_qz,"
               "baseline_scale,"
               "comparison_tx,comparison_ty,comparison_tz,"
               "comparison_qw,comparison_qx,comparison_qy,comparison_qz,"
               "comparison_scale,"
               "delta_tx,delta_ty,delta_tz,delta_translation_length,"
               "delta_rotation_degrees,delta_scale\n";

        if (report.panes.empty() ||
            report.baselinePane >=
                report.panes.size())
        {
            return true;
        }
        const NodeTransformSnapshotSet& baselinePane =
            report.panes[report.baselinePane];
        for (const NodeTransformDiffRow& row :
             report.rows)
        {
            const NodeTransformCell& baselineCell =
                row.cells[report.baselinePane];
            for (std::size_t paneIndex = 0;
                 paneIndex < report.panes.size();
                 ++paneIndex)
            {
                if (paneIndex ==
                    report.baselinePane)
                {
                    continue;
                }
                const NodeTransformCell& cell =
                    row.cells[paneIndex];
                const NodeTransformSnapshot* representative =
                    baselineCell.node
                        ? &*baselineCell.node
                        : (cell.node
                               ? &*cell.node
                               : nullptr);
                std::vector<std::string> fields
                {
                    WideToUtf8(
                        baselinePane.sourcePath),
                    WideToUtf8(
                        report.panes[paneIndex]
                            .sourcePath),
                    StatusUtf8(cell.status),
                    row.displayPath,
                    representative
                        ? representative->nodeName
                        : std::string(),
                    representative
                        ? representative->blockType
                        : std::string(),
                    baselineCell.node
                        ? baselineCell.node
                              ->parentDisplayPath
                        : std::string(),
                    cell.node
                        ? cell.node
                              ->parentDisplayPath
                        : std::string(),
                    baselineCell.node
                        ? (baselineCell.node
                                   ->transformValid &&
                               IsFiniteTransform(
                                   baselineCell.node
                                       ->localTransform)
                               ? "true"
                               : "false")
                        : std::string(),
                    cell.node
                        ? (cell.node->transformValid &&
                               IsFiniteTransform(
                                   cell.node
                                       ->localTransform)
                               ? "true"
                               : "false")
                        : std::string(),
                };
                AppendTransformCsv(
                    fields,
                    baselineCell.node);
                AppendTransformCsv(
                    fields,
                    cell.node);
                if (cell.delta &&
                    cell.delta->valid)
                {
                    fields.push_back(
                        CsvNumber(
                            cell.delta
                                ->translation[0]));
                    fields.push_back(
                        CsvNumber(
                            cell.delta
                                ->translation[1]));
                    fields.push_back(
                        CsvNumber(
                            cell.delta
                                ->translation[2]));
                    fields.push_back(
                        CsvNumber(
                            cell.delta
                                ->translationLength));
                    fields.push_back(
                        CsvNumber(
                            cell.delta
                                ->rotationDegrees));
                    fields.push_back(
                        CsvNumber(
                            cell.delta->scale));
                }
                else
                {
                    for (int i = 0; i < 6; ++i)
                    {
                        fields.emplace_back();
                    }
                }
                for (std::size_t i = 0;
                     i < fields.size();
                     ++i)
                {
                    if (i != 0)
                    {
                        out << ',';
                    }
                    out << CsvField(fields[i]);
                }
                out << '\n';
            }
        }
        if (!out)
        {
            SetError(
                errorOut,
                "Writing the CSV output failed.");
            return false;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        SetError(
            errorOut,
            error.what());
        return false;
    }
}

bool ExportNodeTransformJson(
    const NodeTransformDiffReport& report,
    const std::wstring& path,
    std::string* errorOut)
{
    try
    {
        std::ofstream out(
            std::filesystem::path(path),
            std::ios::binary |
                std::ios::trunc);
        if (!out)
        {
            SetError(
                errorOut,
                "Could not open the JSON output file.");
            return false;
        }

        out
            << "{\"formatVersion\":1,\"baselinePane\":"
            << report.baselinePane
            << ",\"tolerance\":{\"translation\":"
            << JsonNumber(
                   report.tolerance.translation)
            << ",\"rotationDegrees\":"
            << JsonNumber(
                   report.tolerance
                       .rotationDegrees)
            << ",\"scale\":"
            << JsonNumber(
                   report.tolerance.scale)
            << "},\"panes\":[";
        for (std::size_t paneIndex = 0;
             paneIndex < report.panes.size();
             ++paneIndex)
        {
            if (paneIndex != 0)
            {
                out << ',';
            }
            out
                << "{\"label\":"
                << JsonString(
                       report.panes[paneIndex]
                           .label)
                << ",\"sourcePath\":"
                << JsonString(
                       report.panes[paneIndex]
                           .sourcePath)
                << "}";
        }
        out << "],\"rows\":[";
        for (std::size_t rowIndex = 0;
             rowIndex < report.rows.size();
             ++rowIndex)
        {
            if (rowIndex != 0)
            {
                out << ',';
            }
            const NodeTransformDiffRow& row =
                report.rows[rowIndex];
            out
                << "{\"hierarchy\":"
                << JsonString(row.displayPath)
                << ",\"depth\":"
                << row.depth
                << ",\"cells\":[";
            for (std::size_t paneIndex = 0;
                 paneIndex < row.cells.size();
                 ++paneIndex)
            {
                if (paneIndex != 0)
                {
                    out << ',';
                }
                const NodeTransformCell& cell =
                    row.cells[paneIndex];
                out
                    << "{\"status\":"
                    << JsonString(
                           StatusUtf8(
                               cell.status))
                    << ",\"transform\":";
                WriteTransformJson(
                    out,
                    cell.node);
                out << ",\"delta\":";
                if (!cell.delta)
                {
                    out << "null";
                }
                else if (!cell.delta->valid)
                {
                    out
                        << "{\"valid\":false,"
                           "\"translation\":null,"
                           "\"translationLength\":null,"
                           "\"rotationDegrees\":null,"
                           "\"scale\":null}";
                }
                else
                {
                    out
                        << "{\"valid\":true,\"translation\":["
                        << JsonNumber(
                               cell.delta
                                   ->translation[0])
                        << ","
                        << JsonNumber(
                               cell.delta
                                   ->translation[1])
                        << ","
                        << JsonNumber(
                               cell.delta
                                   ->translation[2])
                        << "],\"translationLength\":"
                        << JsonNumber(
                               cell.delta
                                   ->translationLength)
                        << ",\"rotationDegrees\":"
                        << JsonNumber(
                               cell.delta
                                   ->rotationDegrees)
                        << ",\"scale\":"
                        << JsonNumber(
                               cell.delta->scale)
                        << "}";
                }
                out << "}";
            }
            out << "]}";
        }
        out << "]}";
        if (!out)
        {
            SetError(
                errorOut,
                "Writing the JSON output failed.");
            return false;
        }
        return true;
    }
    catch (const std::exception& error)
    {
        SetError(
            errorOut,
            error.what());
        return false;
    }
}

} // namespace nsk
