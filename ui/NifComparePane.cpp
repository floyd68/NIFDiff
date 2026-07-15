#include "NifComparePane.h"

#include "../core/StartupTrace.h"

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

    std::wstring FormatCount(std::size_t n)
    {
        std::wstring s = std::to_wstring(n);
        for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
            s.insert(static_cast<std::size_t>(i), L",");
        return s;
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

    // Bottom-right stats readout: total triangle count of the loaded scene,
    // plus the picked sub-mesh's count while one is selected. Full-width
    // bottom strip with trailing alignment = visually bottom-right.
    m_statsLabel = std::make_shared<FD2D::Text>(name + L"_Stats");
    m_statsLabel->SetFont(L"Segoe UI", 13.0f);
    m_statsLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));
    m_statsLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    // DockPanel's Fill dock stops any further docking, so the Top-docked
    // path strip and Bottom-docked stats strip must be added before the
    // Fill-docked viewport (see DockPanel::Arrange / NifViewport.h's
    // comment on the same pattern).
    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_statsLabel);
    SetChildDock(m_statsLabel, FD2D::Dock::Bottom);
    AddChild(m_viewport);
    SetChildDock(m_viewport, FD2D::Dock::Fill);

    // Click-to-select feedback: show the picked sub-mesh's name next to the
    // file path (NIFDiff has no block-tree sidebar for the selection to
    // land in, so the path strip doubles as the selection readout) and its
    // triangle count in the stats strip.
    m_viewport->SetOnSelectionChanged([this](const RenderMesh* sel)
    {
        m_selectedName = sel ? Utf8ToWide(sel->nodeName) : std::wstring();
        m_selectedKind = sel ? m_viewport->ShaderKindFor(*sel) : std::wstring();
        UpdatePathLabel();
        UpdateStatsLabel();
    });

    UpdatePathLabel();
    UpdateStatsLabel();
}

void NifComparePane::UpdatePathLabel()
{
    std::wstring text = m_doc ? m_doc->filePath() : L"(no file)";
    if (!m_selectedName.empty())
    {
        text += L"    ▸ " + m_selectedName;
        if (!m_selectedKind.empty())
            text += L"  [" + m_selectedKind + L"]";
    }
    m_pathLabel->SetText(text);
    m_pathLabel->Invalidate();
}

void NifComparePane::UpdateStatsLabel()
{
    std::wstring text;
    if (m_doc)
    {
        // File-wide shader kinds (see NifViewport::ShaderKindSummary), then
        // the selection/total triangle counts.
        const std::wstring shaders = m_viewport->ShaderKindSummary();
        if (!shaders.empty())
            text = L"Shaders: " + shaders + L"    |    ";
        if (const RenderMesh* sel = m_viewport->SelectedMesh(); sel && sel->geometry)
            text += L"Sel: " + FormatCount(sel->geometry->triangles.size()) + L"  |  ";
        // Trailing NBSPs keep the right-aligned text off the pane edge (Text
        // ignores Wnd margins, and plain trailing spaces collapse in DWrite's
        // trailing alignment).
        text += L"Total: " + FormatCount(m_viewport->TotalTriangleCount()) + L" tris\u00A0\u00A0";
    }
    m_statsLabel->SetText(text);
    m_statsLabel->Invalidate();
}

bool NifComparePane::Load(const std::wstring& path, std::string* error)
{
    StartupTrace::Phase total("Pane Load total");
    auto doc = std::make_unique<NifDocument>();
    {
        StartupTrace::Phase p("  NifDocument::loadFromFile");
        if (!doc->loadFromFile(path, error))
            return false;
    }
    m_doc = std::move(doc);
    {
        StartupTrace::Phase p("  Viewport SetDocument (scene build)");
        m_viewport->SetDocument(m_doc.get());
    }
    UpdatePathLabel();
    UpdateStatsLabel();
    if (m_onDocumentChanged)
        m_onDocumentChanged();
    if (m_onFileOpened)
        m_onFileOpened(path);
    return true;
}

void NifComparePane::Clear()
{
    m_doc.reset();
    m_viewport->SetDocument(nullptr);
    UpdatePathLabel();
    UpdateStatsLabel();
    if (m_onDocumentChanged)
        m_onDocumentChanged();
}

void NifComparePane::SetResourceResolver(ResourceResolver* resolver)
{
    m_viewport->SetResourceResolver(resolver);
}

void NifComparePane::SetTextureRepository(TextureRepository* repository)
{
    m_viewport->SetTextureRepository(repository);
}

void NifComparePane::SetRenderDevice(RenderDevice* device)
{
    m_viewport->SetRenderDevice(device);
}

void NifComparePane::InvalidateTextureCache()
{
    m_viewport->InvalidateTextureCache();
}

} // namespace nsk
