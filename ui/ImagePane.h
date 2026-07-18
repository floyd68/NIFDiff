// ImagePane.h - pane metadata and controls around ImagePresentation.
#pragma once

#include "ImagePresentation.h"
#include "PaneContent.h"

#include <Text.h>

#include <functional>
#include <memory>
#include <string>

namespace nsk
{

class ImagePane : public PaneContent
{
public:
    explicit ImagePane(const std::wstring& name);
    ~ImagePane() override;

    // PaneContent: this is the 2D image content.
    Kind PaneKind() const override { return Kind::Image; }
    std::wstring CurrentPath() const override;
    bool Load(const std::wstring& path, std::string* error = nullptr) override;
    void Clear() override;

    // Texture-view controls (forwarded to the image display), driven by the
    // view's keyboard shortcuts while an image pane is active.
    void SetChannelMode(int mode);      // 0=RGBA,1=R,2=G,3=B,4=A; re-select toggles off
    void ToggleAlphaCheckerboard();
    void RotateCW();
    void RotateCCW();
    void ResetView();

    // Per-pane alpha-usage override for the ambiguous cases the Auto policy can't
    // decide (a compressed straight alpha that IS real transparency, or a loose
    // alpha that is data). Driven by the pane's context menu; reset to Auto on
    // each new image. EffectiveAlphaUsage is what's actually in effect (for the
    // menu's checked state).
    void SetAlphaUsageOverride(ImageCore::AlphaUsage usage);
    ImageCore::AlphaUsage AlphaUsageOverride() const;
    ImageCore::AlphaUsage EffectiveAlphaUsage() const;

    using ImageViewState = ImagePresentation::ViewState;
    ImageViewState ViewState() const;
    void SetViewState(const ImageViewState& state);          // applies without re-notifying
    void SetOnViewChanged(std::function<void(const ImageViewState&)> handler);

private:
    enum class LoadStatus
    {
        Empty,
        Loading,
        Ready,
        Failed
    };

    void OnLoadCompleted(
        const std::wstring& path,
        HRESULT result);
    void UpdatePathLabel();

    std::shared_ptr<ImagePresentation> m_image;
    std::shared_ptr<FD2D::Text> m_pathLabel;
    std::wstring m_path;
    std::wstring m_pendingPath;
    std::wstring m_failedPath;
    LoadStatus m_loadStatus { LoadStatus::Empty };
};

} // namespace nsk
