#include "NifComparePane.h"

#include <StackPanel.h>

namespace nsk
{

NifComparePane::NifComparePane(const std::wstring& name)
    : FD2D::DockPanel(name)
{
    m_viewport = std::make_shared<NifViewport>(name + L"_Viewport");

    m_openBtn = std::make_shared<FD2D::Button>(name + L"_Open");
    m_openBtn->SetLabel(L"Open...");
    m_closeBtn = std::make_shared<FD2D::Button>(name + L"_Close");
    m_closeBtn->SetLabel(L"✕ Close");

    auto buttonRow = std::make_shared<FD2D::StackPanel>(name + L"_Buttons", FD2D::Orientation::Horizontal);
    buttonRow->SetSpacing(4.0f);
    buttonRow->AddChild(m_openBtn);
    buttonRow->AddChild(m_closeBtn);

    m_pathLabel = std::make_shared<FD2D::Text>(name + L"_Path");
    m_pathLabel->SetFont(L"Segoe UI", 13.0f);
    m_pathLabel->SetEllipsisTrimmingEnabled(true); // long paths trim from the right within the pane width
    m_pathLabel->SetColor(D2D1::ColorF(0.75f, 0.75f, 0.78f));

    // DockPanel's Fill dock stops any further docking, so the Top-docked
    // path strip and Bottom-docked button row must be added before the
    // Fill-docked viewport (see DockPanel::Arrange / NifViewport.h's
    // comment on the same pattern).
    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(buttonRow);
    SetChildDock(buttonRow, FD2D::Dock::Bottom);
    AddChild(m_viewport);
    SetChildDock(m_viewport, FD2D::Dock::Fill);

    UpdatePathLabel();
}

void NifComparePane::UpdatePathLabel()
{
    m_pathLabel->SetText(m_doc ? m_doc->filePath() : L"(no file)");
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

void NifComparePane::SetOnOpen(std::function<void()> handler)
{
    m_openBtn->OnClick(std::move(handler));
}

void NifComparePane::SetOnClose(std::function<void()> handler)
{
    m_closeBtn->OnClick(std::move(handler));
}

} // namespace nsk
