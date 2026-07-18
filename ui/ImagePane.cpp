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
    m_image->SetOnLoadCompleted(
        [this](
            const std::wstring& path,
            HRESULT result)
        {
            OnLoadCompleted(path, result);
        });

    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_image);
    SetChildDock(m_image, FD2D::Dock::Fill);

    UpdatePathLabel();
}

ImagePane::~ImagePane() = default;

bool ImagePane::Load(
    const std::wstring& path,
    std::string* error)
{
    if (path.empty())
    {
        if (error)
        {
            *error = "ImagePane::Load: empty path";
        }
        return false;
    }

    m_path.clear();
    m_pendingPath = path;
    m_failedPath.clear();
    m_loadStatus = LoadStatus::Loading;
    UpdatePathLabel();

    if (!m_image->Load(path))
    {
        m_pendingPath.clear();
        m_failedPath = path;
        m_loadStatus = LoadStatus::Failed;
        UpdatePathLabel();
        if (error)
        {
            *error = "ImagePane::Load: decode request rejected";
        }
        Invalidate();
        return false;
    }

    Invalidate();
    return true;
}

std::wstring ImagePane::CurrentPath() const
{
    return m_loadStatus == LoadStatus::Loading
        ? m_pendingPath
        : m_path;
}

void ImagePane::Clear()
{
    m_path.clear();
    m_pendingPath.clear();
    m_failedPath.clear();
    m_loadStatus = LoadStatus::Empty;
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

void ImagePane::OnLoadCompleted(
    const std::wstring& path,
    HRESULT result)
{
    if (m_loadStatus != LoadStatus::Loading ||
        m_pendingPath != path)
    {
        return;
    }

    m_pendingPath.clear();
    if (SUCCEEDED(result))
    {
        m_path = path;
        m_failedPath.clear();
        m_loadStatus = LoadStatus::Ready;
        NotifyFileOpened(path);
    }
    else
    {
        m_path.clear();
        m_failedPath = path;
        m_loadStatus = LoadStatus::Failed;
    }

    UpdatePathLabel();
    Invalidate();
}

void ImagePane::UpdatePathLabel()
{
    std::wstring text;
    std::wstring copyText;
    switch (m_loadStatus)
    {
    case LoadStatus::Loading:
        text = L"Loading " + m_pendingPath;
        copyText = m_pendingPath;
        m_pathLabel->SetColor(
            D2D1::ColorF(0.75f, 0.75f, 0.78f));
        break;

    case LoadStatus::Ready:
        text = m_path;
        copyText = m_path;
        m_pathLabel->SetColor(
            D2D1::ColorF(0.75f, 0.75f, 0.78f));
        break;

    case LoadStatus::Failed:
        text = L"Failed to load " + m_failedPath;
        copyText = m_failedPath;
        m_pathLabel->SetColor(
            D2D1::ColorF(0.95f, 0.42f, 0.42f));
        break;

    case LoadStatus::Empty:
    default:
        text = L"(no image)";
        m_pathLabel->SetColor(
            D2D1::ColorF(0.75f, 0.75f, 0.78f));
        break;
    }

    m_pathLabel->SetText(text);
    m_pathLabel->SetCopyText(copyText);
    m_pathLabel->Invalidate();
}

} // namespace nsk
