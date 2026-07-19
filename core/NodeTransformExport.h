#pragma once

#include "NodeTransformDiff.h"

#include <string>

namespace nsk
{

bool ExportNodeTransformCsv(
    const NodeTransformDiffReport& report,
    const std::wstring& path,
    std::string* errorOut = nullptr);

bool ExportNodeTransformJson(
    const NodeTransformDiffReport& report,
    const std::wstring& path,
    std::string* errorOut = nullptr);

} // namespace nsk
