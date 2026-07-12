#include "NifCompareSplitCoordinator.h"

#include <SplitPanel.h>

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
    if (n == 1)
    {
        return panes[0];
    }

    auto makeSplit = [](
        const std::wstring& name,
        float ratio,
        const std::shared_ptr<FD2D::Wnd>& a,
        const std::shared_ptr<FD2D::Wnd>& b) -> std::shared_ptr<FD2D::SplitPanel>
    {
        auto sp = std::make_shared<FD2D::SplitPanel>(name, FD2D::SplitterOrientation::Horizontal);
        sp->SetSplitRatio(ratio);
        sp->SetConstraintPropagation(FD2D::ConstraintPropagation::None);
        sp->SetFirstChild(a);
        sp->SetSecondChild(b);
        return sp;
    };

    static int s_hostId = 1;

    if (n == 2)
    {
        return makeSplit(L"hSplit2_" + std::to_wstring(s_hostId++), 0.5f, panes[0], panes[1]);
    }
    if (n == 3)
    {
        // 1/3 | (2/3 split into 1/2 + 1/2) => 1/3, 1/3, 1/3
        auto right = makeSplit(L"hSplit3_r_" + std::to_wstring(s_hostId++), 0.5f, panes[1], panes[2]);
        return makeSplit(L"hSplit3_" + std::to_wstring(s_hostId++), 1.0f / 3.0f, panes[0], right);
    }

    // n == 4
    auto left = makeSplit(L"hSplit4_l_" + std::to_wstring(s_hostId++), 0.5f, panes[0], panes[1]);
    auto right = makeSplit(L"hSplit4_r_" + std::to_wstring(s_hostId++), 0.5f, panes[2], panes[3]);
    return makeSplit(L"hSplit4_" + std::to_wstring(s_hostId++), 0.5f, left, right);
}

} // namespace nsk
