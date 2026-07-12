// NifCompareSplitCoordinator.h - dynamic 2-4 pane layout helper for
// NifCompareView, ported/adapted from FICture2's own
// ImageBrowserSplitCoordinator (D:\Works\Ficture2\ImageBrowserSplitCoordinator.h,
// MIT licensed, same author) - see README.md's "Origins" section (FICture2
// subsection: "side-by-side compare composition pattern"). FICture2's
// version is already generic over FD2D::Wnd, so the algorithm carries over
// unchanged; only the naming is adapted to NIFDiff's pane vocabulary
// (viewer -> pane) and the max stays capped at 4.
#pragma once

#include <FD2D.h>

#include <memory>
#include <string>
#include <vector>

namespace nsk
{

class NifCompareSplitCoordinator
{
public:
    static bool CanAddPane(size_t paneCount, size_t maxPanes = 4);
    static std::wstring NextPaneName();

    // Builds a nested-SplitPanel host tree giving every pane equal width:
    // 1 -> passthrough, 2 -> 50/50, 3 -> 33/33/33, 4 -> 25/25/25/25.
    static std::shared_ptr<FD2D::Wnd> BuildEqualWidthHostTree(
        const std::vector<std::shared_ptr<FD2D::Wnd>>& panes);
};

} // namespace nsk
