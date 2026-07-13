#include "NifCompareSplitCoordinator.h"

#include <SplitPanel.h>

#include <functional>

namespace nsk
{

bool NifCompareSplitCoordinator::CanAddPane(size_t paneCount, size_t maxPanes)
{
    return paneCount < maxPanes;
}

std::wstring NifCompareSplitCoordinator::NextPaneName()
{
    static int s_paneId = 1;
    return L"pane_" + std::to_wstring(s_paneId++);
}

std::shared_ptr<FD2D::Wnd> NifCompareSplitCoordinator::BuildEqualWidthHostTree(
    const std::vector<std::shared_ptr<FD2D::Wnd>>& panes)
{
    const size_t n = panes.size();
    if (n == 0)
    {
        return nullptr;
    }

    static int s_hostId = 1;

    auto makeSplit = [](
        const std::wstring& name,
        FD2D::SplitterOrientation orientation,
        float ratio,
        const std::shared_ptr<FD2D::Wnd>& a,
        const std::shared_ptr<FD2D::Wnd>& b) -> std::shared_ptr<FD2D::SplitPanel>
    {
        auto sp = std::make_shared<FD2D::SplitPanel>(name, orientation);
        sp->SetSplitRatio(ratio);
        sp->SetConstraintPropagation(FD2D::ConstraintPropagation::None);
        sp->SetFirstChild(a);
        sp->SetSecondChild(b);
        return sp;
    };

    // Equal-width row over panes[first..first+count): a balanced split tree
    // (2 -> 1+1, 3 -> 1+2, 4 -> 2+2) whose top-level ratio leftCount/count
    // makes every leaf come out at exactly 1/count of the row.
    std::function<std::shared_ptr<FD2D::Wnd>(size_t, size_t)> buildRow =
        [&](size_t first, size_t count) -> std::shared_ptr<FD2D::Wnd>
    {
        if (count == 1)
        {
            return panes[first];
        }
        const size_t leftCount = count / 2;
        auto left = buildRow(first, leftCount);
        auto right = buildRow(first + leftCount, count - leftCount);
        return makeSplit(L"hSplit_" + std::to_wstring(s_hostId++),
            FD2D::SplitterOrientation::Horizontal,
            static_cast<float>(leftCount) / static_cast<float>(count),
            left, right);
    };

    if (n <= 4)
    {
        return buildRow(0, n);
    }

    // Two-row grid: 5-6 panes -> 3 columns, 7-8 -> 4 columns. The top row
    // fills first in pane order; an odd pane count just leaves the bottom
    // row with fewer (wider) panes rather than an empty placeholder cell.
    const size_t topCount = (n <= 6) ? 3 : 4;
    auto top = buildRow(0, topCount);
    auto bottom = buildRow(topCount, n - topCount);
    return makeSplit(L"vSplit_" + std::to_wstring(s_hostId++),
        FD2D::SplitterOrientation::Vertical, 0.5f, top, bottom);
}

} // namespace nsk
