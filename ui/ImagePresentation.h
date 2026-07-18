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
    void ResetView();

    void SetAlphaUsageOverride(ImageCore::AlphaUsage usage);
    ImageCore::AlphaUsage AlphaUsageOverride() const;
    ImageCore::AlphaUsage EffectiveAlphaUsage() const;

    ViewState GetState() const;
    void SetState(const ViewState& state);
    void SetOnViewChanged(std::function<void(const ViewState&)> handler);
    void SetOnLoadCompleted(
        std::function<void(const std::wstring&, HRESULT)> handler);

    void OnAttached(FD2D::Backplate& backplate) override;
    void OnGraphicsInvalidated(
        FD2D::GraphicsInvalidationReason reason,
        const FD2D::GraphicsGeneration& generation) override;
    void OnRenderD3D(ID3D11DeviceContext* context) override;
    void OnRender(ID2D1RenderTarget* target) override;
    bool OnInputEvent(const FD2D::InputEvent& event) override;

private:
    struct LoadState;

    enum class TakeMode
    {
        AnyForGpu,
        UncompressedForD2D
    };

    bool TryApplyCachedSrv(
        const std::wstring& path,
        std::uint64_t generation);
    void StartDecode(
        const std::wstring& path,
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
    ImageCore::AlphaUsage m_alphaUsageOverride { ImageCore::AlphaUsage::Auto };
    bool m_checkerboard { false };
    bool m_panning { false };
    LONG m_dragStartX { 0 };
    LONG m_dragStartY { 0 };
    float m_panStartX { 0.0f };
    float m_panStartY { 0.0f };
    std::function<void(const ViewState&)> m_onViewChanged;
    std::function<void(const std::wstring&, HRESULT)> m_onLoadCompleted;
};

} // namespace nsk
