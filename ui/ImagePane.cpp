#include "ImagePane.h"

#include <d2d1.h>

#include <utility>

namespace nsk
{

ImagePane::ImagePane(const std::wstring& name)
    : PaneContent(name)
{
    m_pathLabel =
        std::make_shared<FD2D::Text>(name + L"_Path");
    m_pathLabel->SetFont(L"Segoe UI", 13.0f);
    m_pathLabel->SetEllipsisTrimmingEnabled(true);
    m_pathLabel->SetColor(
        D2D1::ColorF(0.75f, 0.75f, 0.78f));
    m_pathLabel->SetTooltipOnTruncation(true);
    m_pathLabel->SetCopyTextOnRightClick(true);

    m_image =
        std::make_shared<ImagePresentation>(
            name + L"_Image");

    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_image);
    SetChildDock(m_image, FD2D::Dock::Fill);

    UpdatePathLabel();
}

ImagePane::~ImagePane() = default;

bool ImagePane::Load(
    const std::wstring& path,
    std::string* /*error*/)
{
    if (path.empty() || !m_image->Load(path))
    {
        return false;
    }

    m_path = path;
    UpdatePathLabel();
    NotifyFileOpened(path);
    Invalidate();
    return true;
}

void ImagePane::Clear()
{
    m_path.clear();
    if (m_image)
    {
        m_image->ClearImage();
    }
    UpdatePathLabel();
    Invalidate();
}

void ImagePane::SetChannelMode(int mode)
{
    if (m_image)
    {
        m_image->SetChannelMode(mode);
    }
}

void ImagePane::ToggleAlphaCheckerboard()
{
    if (m_image)
    {
        m_image->ToggleCheckerboard();
    }
}

void ImagePane::SetAlphaUsageOverride(
    ImageCore::AlphaUsage usage)
{
    if (m_image)
    {
        m_image->SetAlphaUsageOverride(usage);
    }
}

ImageCore::AlphaUsage
ImagePane::AlphaUsageOverride() const
{
    return m_image
        ? m_image->AlphaUsageOverride()
        : ImageCore::AlphaUsage::Auto;
}

ImageCore::AlphaUsage
ImagePane::EffectiveAlphaUsage() const
{
    return m_image
        ? m_image->EffectiveAlphaUsage()
        : ImageCore::AlphaUsage::Auto;
}

void ImagePane::RotateCW()
{
    if (m_image)
    {
        m_image->RotateCW();
    }
}

void ImagePane::RotateCCW()
{
    if (m_image)
    {
        m_image->RotateCCW();
    }
}

void ImagePane::ResetView()
{
    if (m_image)
    {
        m_image->ResetView();
    }
}

ImagePane::ImageViewState ImagePane::ViewState() const
{
    return m_image
        ? m_image->GetState()
        : ImageViewState {};
}

void ImagePane::SetViewState(
    const ImageViewState& state)
{
    if (m_image)
    {
        m_image->SetState(state);
    }
}

void ImagePane::SetOnViewChanged(
    std::function<void(const ImageViewState&)> handler)
{
    if (m_image)
    {
        m_image->SetOnViewChanged(
            std::move(handler));
    }
}

void ImagePane::UpdatePathLabel()
{
    m_pathLabel->SetText(
        m_path.empty()
            ? L"(no image)"
            : m_path);
    m_pathLabel->SetCopyText(m_path);
    m_pathLabel->Invalidate();
}

} // namespace nsk
