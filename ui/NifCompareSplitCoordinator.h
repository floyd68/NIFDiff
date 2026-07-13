// NifCompareSplitCoordinator.h - dynamic 1-8 pane layout helper for
// NifCompareView, ported/adapted from FICture2's own
// ImageBrowserSplitCoordinator (D:\Works\Ficture2\ImageBrowserSplitCoordinator.h,
// MIT licensed, same author) - see README.md's "Origins" section (FICture2
// subsection: "side-by-side compare composition pattern"). FICture2's
// version is already generic over FD2D::Wnd, so the algorithm carries over
// unchanged; the naming is adapted to NIFDiff's pane vocabulary
// (viewer -> pane) and the single row is extended to a two-row grid past 4
// panes (see BuildEqualWidthHostTree).
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
    static bool CanAddPane(size_t paneCount, size_t maxPanes = 8);
    static std::wstring NextPaneName();

    // Builds a nested-SplitPanel host tree laying the panes out as a grid:
    //   1-4 panes -> one row of equal widths (n x 1)
    //   5-6 panes -> two rows, 3 columns (3 x 2; a 5th pane leaves the
    //                bottom row as two half-width panes)
    //   7-8 panes -> two rows, 4 columns (4 x 2)
    // Rows fill top-first in pane order; both rows share the height 50/50.
    static std::shared_ptr<FD2D::Wnd> BuildEqualWidthHostTree(
        const std::vector<std::shared_ptr<FD2D::Wnd>>& panes);
};

} // namespace nsk
