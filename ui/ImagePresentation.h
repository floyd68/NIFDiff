// ImagePresentation.h - reusable asynchronous image presentation widget.
#pragma once

#include "ImageAlphaPresentation.h"
#include "ImageCore/DecodedImage.h"

#include <Image.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace FD2D
{
class AsyncRedrawToken;
}

namespace nsk
{

class ImagePresentation : public FD2D::Image
{
public:
    struct ViewState
    {
        float zoom { 1.0f };
        float panX { 0.0f };
        float panY { 0.0f };
        int rotation { 0 };
        int channelMode { 0 };
        bool checkerboard { false };
        bool highQualitySampling { true };
        uint32_t mipLevel { 0 };
    };

    struct ContentInfo
    {
        UINT width { 0 };
        UINT height { 0 };
        DXGI_FORMAT format { DXGI_FORMAT_UNKNOWN };
        ImageAlphaInfo alpha {};
        uint32_t sourceMipLevels { 1 };
        uint32_t sourceMipIndex { 0 };
        UINT sourceWidth { 0 };
        UINT sourceHeight { 0 };
        bool gpuPresentation { false };
    };

    explicit ImagePresentation(const std::wstring& name);
    ~ImagePresentation() override;

    bool Load(const std::wstring& path);
    void ClearImage();
    std::wstring CurrentPath() const;

    void SetChannelMode(int mode);
    void ToggleCheckerboard();
    void RotateCW();
    void RotateCCW();
    void Rotate180();
    void ResetRotation();
    void ToggleSampling();
    bool HighQualitySampling() const;
    void FitToScreen();
    bool SelectMip(uint32_t mipLevel);
    uint32_t MipLevel() const;
    uint32_t MipLevels() const;

    void SetAlphaUsageOverride(ImageCore::AlphaUsage usage);
    ImageCore::AlphaUsage AlphaUsageOverride() const;
    ImageCore::AlphaUsage EffectiveAlphaUsage() const;

    ViewState GetState() const;
    ContentInfo GetContentInfo() const;
    void SetState(const ViewState& state);
    void SetOnViewChanged(std::function<void(const ViewState&)> handler);
    void SetOnAnimationRequested(std::function<void()> handler);
    bool TickViewAnimation(unsigned long long nowMs);
    void SetOnLoadCompleted(
        std::function<void(const std::wstring&, HRESULT)> handler);
    void SetOnMipSelectionCompleted(
        std::function<void(uint32_t, HRESULT)> handler);

    void OnAttached(FD2D::Backplate& backplate) override;
    void OnGraphicsInvalidated(
        FD2D::GraphicsInvalidationReason reason,
        const FD2D::GraphicsGeneration& generation) override;
    void OnRenderD3D(ID3D11DeviceContext* context) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;
    void Arrange(FD2D::Rect finalRect) override;

private:
    struct LoadState;

    enum class TakeMode
    {
        AnyForGpu,
        UncompressedForD2D
    };

    bool TryApplyCachedSrv(
        const std::wstring& path,
        uint32_t mipLevel,
        std::uint64_t generation);
    void StartDecode(
        const std::wstring& path,
        uint32_t mipLevel,
        std::uint64_t generation,
        bool allowGpuCompressedDDS);
    void ReloadCurrent(bool allowGpuCompressedDDS);
    void RequestCpuRetry(
        const std::wstring& path,
        std::uint64_t generation);
    void StagePayload(
        ImageCore::DecodedImage image,
        const std::wstring& path,
        std::uint64_t generation);
    void StageLoadFailure(
        const std::wstring& path,
        std::uint64_t generation,
        HRESULT result);
    void PublishLoadResult(
        const std::wstring& path,
        std::uint64_t generation,
        HRESULT result);
    void PublishPendingFailure();
    bool IsCurrent(
        const std::wstring& path,
        std::uint64_t generation) const;

    void ApplyDrawState();
    void PushDrawState();
    void ResetViewState();
    void ClampPan();
    void CancelZoomAnimation();
    bool TakePending(
        ImageCore::DecodedImage& out,
        std::wstring& outPath,
        std::uint64_t& outGeneration,
        TakeMode mode);
    void RestagePayload(
        const std::wstring& path,
        std::uint64_t generation);

    static bool IsBlockCompressed(DXGI_FORMAT format);
    static DXGI_FORMAT TypedSrvFormat(DXGI_FORMAT format);

    std::shared_ptr<LoadState> m_loadState;

    mutable std::mutex m_payloadMutex;
    ImageCore::DecodedImage m_payload;
    std::wstring m_payloadPath;
    std::uint64_t m_payloadGeneration { 0 };
    bool m_needUpload { false };
    std::wstring m_failurePath;
    std::uint64_t m_failureGeneration { 0 };
    HRESULT m_failureResult { S_OK };
    bool m_hasPendingFailure { false };

    std::wstring m_srvPath;
    bool m_usingD2DBitmap { false };
    std::shared_ptr<FD2D::AsyncRedrawToken> m_redraw;

    static constexpr float kMinZoom = 0.02f;
    static constexpr float kMaxZoom = 64.0f;
    float m_zoom { 1.0f };
    float m_panX { 0.0f };
    float m_panY { 0.0f };
    int m_rotation { 0 };
    int m_channelMode { 0 };
    ImageAlphaInfo m_srvAlpha {};
    DXGI_FORMAT m_sourceFormat { DXGI_FORMAT_UNKNOWN };
    uint32_t m_sourceMipLevels { 1 };
    uint32_t m_sourceMipIndex { 0 };
    UINT m_sourceWidth { 0 };
    UINT m_sourceHeight { 0 };
    ImageCore::AlphaUsage m_alphaUsageOverride { ImageCore::AlphaUsage::Auto };
    bool m_checkerboard { false };
    bool m_highQualitySampling { true };
    bool m_panArmed { false };
    bool m_panning { false };
    LONG m_dragStartX { 0 };
    LONG m_dragStartY { 0 };
    float m_panStartX { 0.0f };
    float m_panStartY { 0.0f };
    bool m_zoomAnimating { false };
    unsigned long long m_zoomStartMs { 0 };
    float m_zoomStart { 1.0f };
    float m_zoomTarget { 1.0f };
    float m_zoomPanStartX { 0.0f };
    float m_zoomPanStartY { 0.0f };
    float m_zoomPanTargetX { 0.0f };
    float m_zoomPanTargetY { 0.0f };
    std::function<void(const ViewState&)> m_onViewChanged;
    std::function<void()> m_onAnimationRequested;
    std::function<void(const std::wstring&, HRESULT)> m_onLoadCompleted;
    std::function<void(uint32_t, HRESULT)> m_onMipSelectionCompleted;
};

} // namespace nsk
