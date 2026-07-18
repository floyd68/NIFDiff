#include "ImagePane.h"

#include <d2d1.h>

#include <cmath>
#include <utility>

namespace nsk
{
namespace
{

const wchar_t* FormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return L"BGRA8 UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return L"RGBA8 UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return L"RGBA8 sRGB";
    case DXGI_FORMAT_BC1_UNORM:
        return L"BC1 UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return L"BC1 sRGB";
    case DXGI_FORMAT_BC2_UNORM:
        return L"BC2 UNORM";
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return L"BC2 sRGB";
    case DXGI_FORMAT_BC3_UNORM:
        return L"BC3 UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return L"BC3 sRGB";
    case DXGI_FORMAT_BC4_UNORM:
        return L"BC4 UNORM";
    case DXGI_FORMAT_BC4_SNORM:
        return L"BC4 SNORM";
    case DXGI_FORMAT_BC5_UNORM:
        return L"BC5 UNORM";
    case DXGI_FORMAT_BC5_SNORM:
        return L"BC5 SNORM";
    case DXGI_FORMAT_BC6H_UF16:
        return L"BC6H UF16";
    case DXGI_FORMAT_BC6H_SF16:
        return L"BC6H SF16";
    case DXGI_FORMAT_BC7_UNORM:
        return L"BC7 UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return L"BC7 sRGB";
    default:
        return L"Unknown";
    }
}

}

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
    m_image->SetOnMipSelectionCompleted(
        [this](uint32_t mipLevel, HRESULT result)
        {
            if (SUCCEEDED(result))
            {
                m_mipError.clear();
                UpdateInfoLabel();
                if (m_onViewChanged)
                {
                    m_onViewChanged(
                        m_image->GetState());
                }
            }
            else
            {
                m_mipError =
                    L"Failed to load mip " +
                    std::to_wstring(mipLevel);
            }
            SyncContentOverlay();
            Invalidate();
        });
    m_image->SetOnViewChanged(
        [this](
            const ImageViewState& state)
        {
            UpdateInfoLabel();
            if (m_onViewChanged)
            {
                m_onViewChanged(state);
            }
        });

    m_spinner =
        std::make_shared<FD2D::Spinner>(
            name + L"_Loading");
    FD2D::Spinner::Style spinnerStyle =
        m_spinner->GetStyle();
    spinnerStyle.dimBackground = true;
    spinnerStyle.dimAlpha = 0.32f;
    m_spinner->SetStyle(spinnerStyle);

    m_statusOverlay =
        std::make_shared<FD2D::Text>(
            name + L"_Status");
    m_statusOverlay->SetFont(L"Segoe UI", 18.0f);
    m_statusOverlay->SetTextAlignment(
        DWRITE_TEXT_ALIGNMENT_CENTER);
    m_statusOverlay->SetParagraphAlignment(
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_statusOverlay->SetColor(
        D2D1::ColorF(0.95f, 0.42f, 0.42f));

    m_infoLabel =
        std::make_shared<FD2D::Text>(
            name + L"_Info");
    m_infoLabel->SetFont(L"Segoe UI", 12.0f);
    m_infoLabel->SetEllipsisTrimmingEnabled(true);
    m_infoLabel->SetColor(
        D2D1::ColorF(0.58f, 0.63f, 0.70f));
    m_infoLabel->SetTooltipOnTruncation(true);

    m_contentOverlay =
        std::make_shared<FD2D::OverlayPanel>(
            name + L"_Content");
    m_contentOverlay->AddChild(m_image);
    m_contentOverlay->AddChild(m_spinner);
    m_contentOverlay->AddChild(m_statusOverlay);

    AddChild(m_pathLabel);
    SetChildDock(m_pathLabel, FD2D::Dock::Top);
    AddChild(m_infoLabel);
    SetChildDock(m_infoLabel, FD2D::Dock::Bottom);
    AddChild(m_contentOverlay);
    SetChildDock(m_contentOverlay, FD2D::Dock::Fill);

    UpdatePathLabel();
    UpdateInfoLabel();
    SyncContentOverlay();
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
    m_mipError.clear();
    m_loadStatus = LoadStatus::Loading;
    UpdatePathLabel();
    UpdateInfoLabel();
    SyncContentOverlay();

    if (!m_image->Load(path))
    {
        m_pendingPath.clear();
        m_failedPath = path;
        m_loadStatus = LoadStatus::Failed;
        UpdatePathLabel();
        SyncContentOverlay();
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
    m_mipError.clear();
    m_loadStatus = LoadStatus::Empty;
    if (m_image)
    {
        m_image->ClearImage();
    }
    UpdatePathLabel();
    UpdateInfoLabel();
    SyncContentOverlay();
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

void ImagePane::Rotate180()
{
    if (m_image)
    {
        m_image->Rotate180();
    }
}

void ImagePane::ResetRotation()
{
    if (m_image)
    {
        m_image->ResetRotation();
    }
}

void ImagePane::ToggleSampling()
{
    if (m_image)
    {
        m_image->ToggleSampling();
    }
}

bool ImagePane::HighQualitySampling() const
{
    return m_image &&
        m_image->HighQualitySampling();
}

void ImagePane::FitToScreen()
{
    if (m_image)
    {
        m_image->FitToScreen();
    }
}

bool ImagePane::SelectMip(uint32_t mipLevel)
{
    if (!m_image ||
        m_loadStatus != LoadStatus::Ready)
    {
        return false;
    }

    m_mipError.clear();
    SyncContentOverlay();
    return m_image->SelectMip(mipLevel);
}

uint32_t ImagePane::MipLevel() const
{
    return m_image ? m_image->MipLevel() : 0;
}

uint32_t ImagePane::MipLevels() const
{
    return m_image ? m_image->MipLevels() : 1;
}

ImagePresentation::ContentInfo
ImagePane::ContentInfo() const
{
    return m_image
        ? m_image->GetContentInfo()
        : ImagePresentation::ContentInfo {};
}

std::wstring ImagePane::InformationText() const
{
    if (!m_image ||
        m_loadStatus != LoadStatus::Ready)
    {
        return L"No image is loaded.";
    }

    const ImagePresentation::ContentInfo info =
        m_image->GetContentInfo();
    const ImageViewState state =
        m_image->GetState();
    const ImageCore::AlphaUsage alpha =
        m_image->EffectiveAlphaUsage();

    std::wstring text =
        L"Path: " +
        m_path +
        L"\n\nDimensions: " +
        std::to_wstring(info.width) +
        L" \u00d7 " +
        std::to_wstring(info.height) +
        L"\nFormat: " +
        FormatName(info.format) +
        L"\nMip level: " +
        std::to_wstring(info.sourceMipIndex) +
        L" of " +
        std::to_wstring(info.sourceMipLevels) +
        L"\nSource compression: " +
        (info.alpha.sourceWasBlockCompressed
            ? L"Block compressed"
            : L"Uncompressed") +
        L"\nAlpha usage: " +
        (alpha == ImageCore::AlphaUsage::Data
            ? L"Opaque data"
            : L"Transparency") +
        L"\n\nZoom: " +
        std::to_wstring(
            static_cast<int>(
                std::lround(state.zoom * 100.0f))) +
        L"%\nRotation: " +
        std::to_wstring(
            (state.rotation & 3) * 90) +
        L"\u00b0\nSampling: " +
        (state.highQualitySampling
            ? L"Smooth"
            : L"Nearest-neighbor");
    return text;
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
        UpdateInfoLabel();
    }
}

void ImagePane::SetOnViewChanged(
    std::function<void(const ImageViewState&)> handler)
{
    m_onViewChanged = std::move(handler);
}

void ImagePane::SetOnAnimationRequested(
    std::function<void()> handler)
{
    if (m_image)
    {
        m_image->SetOnAnimationRequested(
            std::move(handler));
    }
}

bool ImagePane::TickViewAnimation(
    unsigned long long nowMs)
{
    return m_image &&
        m_image->TickViewAnimation(nowMs);
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
    UpdateInfoLabel();
    SyncContentOverlay();
    Invalidate();
}

bool ImagePane::TryGetScreenshotClientRect(
    D2D1_RECT_F& rect) const
{
    if (m_loadStatus != LoadStatus::Ready ||
        !m_image)
    {
        return false;
    }

    rect = m_image->LayoutRect();
    return rect.right > rect.left &&
        rect.bottom > rect.top;
}

void ImagePane::SyncContentOverlay()
{
    if (!m_spinner || !m_statusOverlay)
    {
        return;
    }

    const bool loading =
        m_loadStatus == LoadStatus::Loading;
    m_spinner->SetActive(loading);
    m_statusOverlay->SetText(
        m_loadStatus == LoadStatus::Failed
            ? L"Failed to load image"
            : m_mipError);
    m_statusOverlay->Invalidate();
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

void ImagePane::UpdateInfoLabel()
{
    if (!m_infoLabel)
    {
        return;
    }

    std::wstring text;
    if (m_loadStatus == LoadStatus::Ready &&
        m_image)
    {
        const ImagePresentation::ContentInfo info =
            m_image->GetContentInfo();
        const ImageViewState state =
            m_image->GetState();
        text =
            std::to_wstring(info.width) +
            L" \u00d7 " +
            std::to_wstring(info.height) +
            L"  |  " +
            FormatName(info.format) +
            L"  |  " +
            (info.sourceMipLevels > 1
                ? L"Mip " +
                    std::to_wstring(
                        info.sourceMipIndex) +
                    L" of " +
                    std::to_wstring(
                        info.sourceMipLevels) +
                    L"  |  "
                : L"") +
            (state.highQualitySampling
                ? L"Smooth"
                : L"Nearest") +
            L"  |  " +
            std::to_wstring(
                static_cast<int>(
                    std::lround(
                        state.zoom * 100.0f))) +
            L"%";
        if ((state.rotation & 3) != 0)
        {
            text +=
                L"  " +
                std::to_wstring(
                    (state.rotation & 3) * 90) +
                L"\u00b0";
        }
    }

    m_infoLabel->SetText(text);
    m_infoLabel->SetTooltipText(text);
    m_infoLabel->Invalidate();
}

} // namespace nsk
