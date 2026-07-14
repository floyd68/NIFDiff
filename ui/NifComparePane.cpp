#include "NifComparePane.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace nsk
{

namespace
{
    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty())
            return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
        return w;
    }
}

NifComparePane::NifComparePane(const std::wstring& name)
    : FD2D::DockPanel(name)
{
    m_viewport = std::make_shared<NifViewport>(name + L"_Viewport");

    m_pathLabel = std::make_shared<FD2D::Text>(name + L"_Path");
    m_pathLabel->SetFont(L"Segoe UI", 13.0f);
    m_pathLabel->SetEllipsisTrimmingEnabled(true); // long paths trim from the right within the pane width
    m_pathLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));

    // DockPanel's Fill dock stops any further docking, so the Top-docked
    // path strip must be added before the Fill-docked viewport (see
    // DockPanel::Arrange / NifViewport.h's comment on the same pattern).
    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_viewport);
    SetChildDock(m_viewport, FD2D::Dock::Fill);

    // Click-to-select feedback: show the picked sub-mesh's name next to the
    // file path (NIFDiff has no block-tree sidebar for the selection to
    // land in, so the path strip doubles as the selection readout).
    m_viewport->SetOnSelectionChanged([this](const RenderMesh* sel)
    {
        m_selectedName = sel ? Utf8ToWide(sel->nodeName) : std::wstring();
        UpdatePathLabel();
    });

    UpdatePathLabel();
}

void NifComparePane::UpdatePathLabel()
{
    std::wstring text = m_doc ? m_doc->filePath() : L"(no file)";
    if (!m_selectedName.empty())
        text += L"    ▸ " + m_selectedName;
    m_pathLabel->SetText(text);
    m_pathLabel->Invalidate();
}

bool NifComparePane::Load(const std::wstring& path, std::string* error)
{
    auto doc = std::make_unique<NifDocument>();
    if (!doc->loadFromFile(path, error))
        return false;
    m_doc = std::move(doc);
    m_viewport->SetDocument(m_doc.get());
    UpdatePathLabel();
    return true;
}

void NifComparePane::Clear()
{
    m_doc.reset();
    m_viewport->SetDocument(nullptr);
    UpdatePathLabel();
}

void NifComparePane::SetResourceResolver(ResourceResolver* resolver)
{
    m_viewport->SetResourceResolver(resolver);
}

void NifComparePane::InvalidateTextureCache()
{
    m_viewport->InvalidateTextureCache();
}

} // namespace nsk
