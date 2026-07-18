#include "ImagePresentation.h"

#include "ImageGpuResourceCache.h"

#include <Backplate.h>
#include <Util.h>

#include "ImageCore/ImageLoader.h"
#include "ImageCore/ImageRequest.h"

#include <d2d1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace nsk
{

struct ImagePresentation::LoadState
{
    std::mutex mutex;
    std::wstring path;
    std::uint64_t generation { 0 };
    ImageCore::ImageHandle handle { 0 };
    std::uint64_t cpuRetryGeneration { 0 };
    bool loadReported { true };
    bool shuttingDown { false };
    std::weak_ptr<ImagePresentation> presentation;
};

ImagePresentation::ImagePresentation(const std::wstring& name)
    : FD2D::Image(name)
    , m_loadState(std::make_shared<LoadState>())
{
}

ImagePresentation::~ImagePresentation()
{
    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        m_loadState->shuttingDown = true;
        ++m_loadState->generation;
        m_loadState->path.clear();
        handle = std::exchange(m_loadState->handle, 0);
        m_loadState->presentation.reset();
    }

    if (handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(handle);
    }
}

bool ImagePresentation::Load(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    ImageCore::ImageHandle oldHandle = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (m_loadState->shuttingDown)
        {
            return false;
        }

        oldHandle = std::exchange(m_loadState->handle, 0);
        m_loadState->path = path;
        generation = ++m_loadState->generation;
        m_loadState->cpuRetryGeneration = 0;
        m_loadState->loadReported = false;
        m_loadState->presentation =
            std::static_pointer_cast<ImagePresentation>(
                FD2D::Wnd::shared_from_this());
    }

    if (oldHandle)
    {
        ImageCore::ImageLoader::Instance().Cancel(oldHandle);
    }

    {
        std::lock_guard<std::mutex> lock(m_payloadMutex);
        m_payload = {};
        m_payloadPath.clear();
        m_payloadGeneration = 0;
        m_needUpload = false;
        m_failurePath.clear();
        m_failureGeneration = 0;
        m_failureResult = S_OK;
        m_hasPendingFailure = false;
    }

    m_srvAlpha = {};
    m_usingD2DBitmap = false;
    m_srvPath.clear();
    FD2D::Image::Clear();
    ResetView();
    SetAlphaUsageOverride(ImageCore::AlphaUsage::Auto);

    if (TryApplyCachedSrv(path, generation))
    {
        PublishLoadResult(path, generation, S_OK);
        Invalidate();
        return true;
    }

    FD2D::Backplate* backplate = BackplateRef();
    const bool allowGpuCompressedDDS =
        backplate && backplate->D3DDevice();
    StartDecode(path, generation, allowGpuCompressedDDS);
    Invalidate();
    return true;
}

void ImagePresentation::ClearImage()
{
    ImageCore::ImageHandle handle = 0;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        ++m_loadState->generation;
        m_loadState->path.clear();
        m_loadState->cpuRetryGeneration = 0;
        m_loadState->loadReported = true;
        handle = std::exchange(m_loadState->handle, 0);
    }

    if (handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(handle);
    }

    {
        std::lock_guard<std::mutex> lock(m_payloadMutex);
        m_payload = {};
        m_payloadPath.clear();
        m_payloadGeneration = 0;
        m_needUpload = false;
        m_failurePath.clear();
        m_failureGeneration = 0;
        m_failureResult = S_OK;
        m_hasPendingFailure = false;
    }

    m_srvAlpha = {};
    m_usingD2DBitmap = false;
    m_srvPath.clear();
    FD2D::Image::Clear();
    Invalidate();
}

std::wstring ImagePresentation::CurrentPath() const
{
    std::lock_guard<std::mutex> lock(m_loadState->mutex);
    return m_loadState->path;
}

bool ImagePresentation::TryApplyCachedSrv(
    const std::wstring& path,
    std::uint64_t generation)
{
    FD2D::Backplate* backplate = BackplateRef();
    if (!backplate || !backplate->D3DDevice())
    {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ImageAlphaInfo alpha {};
    if (!ImageGpuResourceCache::Instance().TryGet(
            path,
            srv,
            width,
            height,
            format,
            alpha,
            backplate->GetGraphicsGeneration().device))
    {
        return false;
    }

    // Cache lookup is not acceptance: a newer Load/Clear may have won while
    // the process-wide cache lock was held.
    if (!IsCurrent(path, generation))
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_payloadMutex);
        m_payload = {};
        m_payloadPath.clear();
        m_payloadGeneration = 0;
        m_needUpload = false;
    }

    SetShaderResource(srv);
    m_usingD2DBitmap = false;
    m_srvAlpha = alpha;
    m_srvPath = path;
    PushDrawState();
    return true;
}

void ImagePresentation::StartDecode(
    const std::wstring& path,
    std::uint64_t generation,
    bool allowGpuCompressedDDS)
{
    ImageCore::ImageRequest request(
        path,
        ImageCore::ImagePurpose::FullResolution);
    request.allowGpuCompressedDDS = allowGpuCompressedDDS;
    request.srgb = true;

    std::shared_ptr<LoadState> state = m_loadState;
    const ImageCore::ImageHandle handle =
        ImageCore::ImageLoader::Instance().RequestDecoded(
            request,
            [state, path, generation](
                HRESULT result,
                ImageCore::DecodedImage image)
            {
                std::shared_ptr<ImagePresentation> presentation;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->shuttingDown ||
                        state->generation != generation ||
                        state->path != path)
                    {
                        return;
                    }
                    presentation = state->presentation.lock();
                }

                if (!presentation)
                {
                    return;
                }

                if (FAILED(result) ||
                    !image.blocks ||
                    image.blocks->empty())
                {
                    presentation->StageLoadFailure(
                        path,
                        generation,
                        FAILED(result) ? result : E_FAIL);
                    return;
                }

                presentation->StagePayload(
                    std::move(image),
                    path,
                    generation);
            });

    bool cancel = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->shuttingDown ||
            state->generation != generation ||
            state->path != path)
        {
            cancel = true;
        }
        else
        {
            state->handle = handle;
        }
    }

    if (cancel && handle)
    {
        ImageCore::ImageLoader::Instance().Cancel(handle);
    }
}

void ImagePresentation::ReloadCurrent(bool allowGpuCompressedDDS)
{
    ImageCore::ImageHandle oldHandle = 0;
    std::wstring path;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (m_loadState->shuttingDown || m_loadState->path.empty())
        {
            return;
        }

        oldHandle = std::exchange(m_loadState->handle, 0);
        path = m_loadState->path;
        generation = ++m_loadState->generation;
        m_loadState->cpuRetryGeneration = 0;
        m_loadState->presentation =
            std::static_pointer_cast<ImagePresentation>(
                FD2D::Wnd::shared_from_this());
    }

    if (oldHandle)
    {
        ImageCore::ImageLoader::Instance().Cancel(oldHandle);
    }

    {
        std::lock_guard<std::mutex> lock(m_payloadMutex);
        m_payload = {};
        m_payloadPath.clear();
        m_payloadGeneration = 0;
        m_needUpload = false;
    }

    StartDecode(path, generation, allowGpuCompressedDDS);
}

void ImagePresentation::RequestCpuRetry(
    const std::wstring& path,
    std::uint64_t generation)
{
    ImageCore::ImageHandle oldHandle = 0;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (m_loadState->shuttingDown ||
            m_loadState->generation != generation ||
            m_loadState->path != path ||
            m_loadState->cpuRetryGeneration == generation)
        {
            return;
        }

        m_loadState->cpuRetryGeneration = generation;
        oldHandle = std::exchange(m_loadState->handle, 0);
    }

    if (oldHandle)
    {
        ImageCore::ImageLoader::Instance().Cancel(oldHandle);
    }

    StartDecode(path, generation, false);
}

void ImagePresentation::StagePayload(
    ImageCore::DecodedImage image,
    const std::wstring& path,
    std::uint64_t generation)
{
    std::shared_ptr<FD2D::AsyncRedrawToken> redraw;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (m_loadState->shuttingDown ||
            m_loadState->generation != generation ||
            m_loadState->path != path)
        {
            return;
        }

        // Keep the current-state lock through staging. Load/Clear therefore
        // either supersedes this completion before it is accepted, or clears
        // the accepted payload afterward; stale data cannot be reintroduced.
        std::lock_guard<std::mutex> payloadLock(m_payloadMutex);
        m_payload = std::move(image);
        m_payloadPath = path;
        m_payloadGeneration = generation;
        m_needUpload =
            m_payload.blocks &&
            !m_payload.blocks->empty();
        redraw = m_redraw;
    }

    if (redraw)
    {
        redraw->RequestAsyncRedraw();
    }
}

void ImagePresentation::StageLoadFailure(
    const std::wstring& path,
    std::uint64_t generation,
    HRESULT result)
{
    std::shared_ptr<FD2D::AsyncRedrawToken> redraw;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (m_loadState->shuttingDown ||
            m_loadState->generation != generation ||
            m_loadState->path != path ||
            m_loadState->loadReported)
        {
            return;
        }

        std::lock_guard<std::mutex> payloadLock(m_payloadMutex);
        m_payload = {};
        m_payloadPath.clear();
        m_payloadGeneration = 0;
        m_needUpload = false;
        m_failurePath = path;
        m_failureGeneration = generation;
        m_failureResult = FAILED(result) ? result : E_FAIL;
        m_hasPendingFailure = true;
        redraw = m_redraw;
    }

    if (redraw)
    {
        redraw->RequestAsyncRedraw();
    }
}

void ImagePresentation::PublishLoadResult(
    const std::wstring& path,
    std::uint64_t generation,
    HRESULT result)
{
    std::function<void(const std::wstring&, HRESULT)> handler;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (m_loadState->shuttingDown ||
            m_loadState->generation != generation ||
            m_loadState->path != path ||
            m_loadState->loadReported)
        {
            return;
        }

        m_loadState->loadReported = true;
        m_loadState->handle = 0;
        if (FAILED(result))
        {
            m_loadState->path.clear();
        }
        handler = m_onLoadCompleted;
    }

    if (handler)
    {
        handler(path, result);
    }
}

void ImagePresentation::PublishPendingFailure()
{
    std::wstring path;
    std::uint64_t generation = 0;
    HRESULT result = S_OK;
    {
        std::lock_guard<std::mutex> lock(m_payloadMutex);
        if (!m_hasPendingFailure)
        {
            return;
        }

        path = std::move(m_failurePath);
        generation = m_failureGeneration;
        result = m_failureResult;
        m_failureGeneration = 0;
        m_failureResult = S_OK;
        m_hasPendingFailure = false;
    }

    PublishLoadResult(path, generation, result);
}

bool ImagePresentation::IsCurrent(
    const std::wstring& path,
    std::uint64_t generation) const
{
    std::lock_guard<std::mutex> lock(m_loadState->mutex);
    return !m_loadState->shuttingDown &&
        m_loadState->generation == generation &&
        m_loadState->path == path;
}

void ImagePresentation::OnAttached(FD2D::Backplate& backplate)
{
    FD2D::Image::OnAttached(backplate);

    std::shared_ptr<FD2D::AsyncRedrawToken> redraw;
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(m_loadState->mutex);
        if (!m_loadState->shuttingDown)
        {
            std::lock_guard<std::mutex> payloadLock(m_payloadMutex);
            m_redraw = backplate.GetAsyncRedrawToken();
            m_loadState->presentation =
                std::static_pointer_cast<ImagePresentation>(
                    FD2D::Wnd::shared_from_this());
            redraw = m_redraw;
            requestRedraw = m_needUpload || m_hasPendingFailure;
        }
    }

    if (requestRedraw && redraw)
    {
        redraw->RequestAsyncRedraw();
    }
}

void ImagePresentation::OnGraphicsInvalidated(
    FD2D::GraphicsInvalidationReason reason,
    const FD2D::GraphicsGeneration& generation)
{
    if (reason == FD2D::GraphicsInvalidationReason::Shutdown)
    {
        ImageCore::ImageHandle handle = 0;
        {
            std::lock_guard<std::mutex> lock(m_loadState->mutex);
            m_loadState->shuttingDown = true;
            ++m_loadState->generation;
            handle = std::exchange(m_loadState->handle, 0);
        }
        if (handle)
        {
            ImageCore::ImageLoader::Instance().Cancel(handle);
        }

        {
            std::lock_guard<std::mutex> lock(m_payloadMutex);
            m_redraw.reset();
        }
        FD2D::Image::OnGraphicsInvalidated(reason, generation);
        return;
    }

    const bool wasUsingD2DBitmap = m_usingD2DBitmap;
    FD2D::Image::OnGraphicsInvalidated(reason, generation);

    if (reason == FD2D::GraphicsInvalidationReason::Resize)
    {
        return;
    }

    m_usingD2DBitmap = false;

    if (reason == FD2D::GraphicsInvalidationReason::TargetRecreated)
    {
        if (wasUsingD2DBitmap)
        {
            std::lock_guard<std::mutex> lock(m_payloadMutex);
            m_needUpload =
                m_payload.blocks &&
                !m_payload.blocks->empty();
        }
        return;
    }

    bool hasPayload = false;
    bool payloadCompressed = false;
    {
        std::lock_guard<std::mutex> lock(m_payloadMutex);
        hasPayload =
            m_payload.blocks &&
            !m_payload.blocks->empty();
        payloadCompressed =
            hasPayload &&
            IsBlockCompressed(m_payload.dxgiFormat);
        m_needUpload = hasPayload;
    }

    FD2D::Backplate* backplate = BackplateRef();
    const bool hasD3D = backplate && backplate->D3DDevice();
    if (reason == FD2D::GraphicsInvalidationReason::RendererFallback &&
        payloadCompressed)
    {
        ReloadCurrent(false);
    }
    else if (!hasPayload && !m_srvPath.empty())
    {
        ReloadCurrent(hasD3D);
    }
}

void ImagePresentation::OnRenderD3D(ID3D11DeviceContext* context)
{
    PublishPendingFailure();

    ImageCore::DecodedImage image;
    std::wstring path;
    std::uint64_t generation = 0;
    if (context &&
        TakePending(
            image,
            path,
            generation,
            TakeMode::AnyForGpu))
    {
        const bool compressed = IsBlockCompressed(image.dxgiFormat);
        bool uploaded = false;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device && IsCurrent(path, generation))
        {
            const DXGI_FORMAT textureFormat =
                compressed
                    ? image.dxgiFormat
                    : DXGI_FORMAT_B8G8R8A8_UNORM;

            D3D11_TEXTURE2D_DESC textureDesc {};
            textureDesc.Width = image.width;
            textureDesc.Height = image.height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = textureFormat;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            D3D11_SUBRESOURCE_DATA data {};
            data.pSysMem = image.blocks->data();
            data.SysMemPitch =
                image.rowPitchBytes
                    ? image.rowPitchBytes
                    : (compressed ? 0u : image.width * 4);

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(device->CreateTexture2D(
                    &textureDesc,
                    &data,
                    &texture)))
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
                srvDesc.Format = TypedSrvFormat(textureFormat);
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = 1;

                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                if (SUCCEEDED(device->CreateShaderResourceView(
                        texture.Get(),
                        &srvDesc,
                        &srv)) &&
                    IsCurrent(path, generation))
                {
                    SetShaderResource(srv);
                    m_usingD2DBitmap = false;
                    m_srvAlpha = AlphaInfoFromDecodedImage(image);
                    m_srvPath = path;
                    PushDrawState();

                    std::uint64_t deviceGeneration = 0;
                    if (FD2D::Backplate* backplate = BackplateRef())
                    {
                        deviceGeneration =
                            backplate->GetGraphicsGeneration().device;
                    }
                    ImageGpuResourceCache::Instance().Put(
                        path,
                        srv,
                        image.width,
                        image.height,
                        srvDesc.Format,
                        m_srvAlpha,
                        deviceGeneration);
                    uploaded = true;
                    PublishLoadResult(
                        path,
                        generation,
                        S_OK);
                }
            }
        }

        if (!uploaded && IsCurrent(path, generation))
        {
            if (compressed)
            {
                RequestCpuRetry(path, generation);
            }
            else
            {
                RestagePayload(path, generation);
            }
        }
    }

    FD2D::Image::OnRenderD3D(context);
}

void ImagePresentation::OnRender(ID2D1RenderTarget* target)
{
    PublishPendingFailure();

    ImageCore::DecodedImage image;
    std::wstring path;
    std::uint64_t generation = 0;
    if (target &&
        TakePending(
            image,
            path,
            generation,
            TakeMode::UncompressedForD2D) &&
        IsCurrent(path, generation))
    {
        m_srvAlpha = AlphaInfoFromDecodedImage(image);
        const ImageCore::AlphaUsage usage =
            ResolveAlphaUsage(
                m_srvAlpha,
                m_alphaUsageOverride);
        const std::vector<std::uint8_t> presentation =
            BuildBgra8Presentation(image, usage);

        const std::uint32_t pitch =
            image.rowPitchBytes
                ? image.rowPitchBytes
                : image.width * 4;
        const D2D1_SIZE_U size {
            image.width,
            image.height
        };
        const auto properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        HRESULT bitmapResult = E_FAIL;
        if (!presentation.empty())
        {
            bitmapResult = target->CreateBitmap(
                size,
                presentation.data(),
                pitch,
                properties,
                &bitmap);
        }
        if (SUCCEEDED(bitmapResult) &&
            IsCurrent(path, generation))
        {
            SetBitmap(bitmap);
            m_usingD2DBitmap = true;
            m_srvPath = path;
            PublishLoadResult(
                path,
                generation,
                S_OK);
        }
        else if (FAILED(bitmapResult) &&
                 IsCurrent(path, generation))
        {
            PublishLoadResult(
                path,
                generation,
                bitmapResult);
        }
    }

    FD2D::Image::OnRender(target);
}

void ImagePresentation::ResetView()
{
    m_zoom = 1.0f;
    m_panX = 0.0f;
    m_panY = 0.0f;
    m_rotation = 0;
    m_channelMode = 0;
    PushDrawState();
}

void ImagePresentation::RotateCW()
{
    m_rotation = (m_rotation + 1) & 3;
    ApplyDrawState();
}

void ImagePresentation::RotateCCW()
{
    m_rotation = (m_rotation + 3) & 3;
    ApplyDrawState();
}

void ImagePresentation::ToggleCheckerboard()
{
    m_checkerboard = !m_checkerboard;
    ApplyDrawState();
}

void ImagePresentation::SetChannelMode(int mode)
{
    m_channelMode =
        m_channelMode == mode
            ? 0
            : mode;
    ApplyDrawState();
}

ImagePresentation::ViewState ImagePresentation::GetState() const
{
    ViewState state;
    state.zoom = m_zoom;
    state.panX = m_panX;
    state.panY = m_panY;
    state.rotation = m_rotation;
    state.channelMode = m_channelMode;
    state.checkerboard = m_checkerboard;
    return state;
}

void ImagePresentation::SetState(const ViewState& state)
{
    m_zoom = state.zoom;
    m_panX = state.panX;
    m_panY = state.panY;
    m_rotation = state.rotation;
    m_channelMode = state.channelMode;
    m_checkerboard = state.checkerboard;
    PushDrawState();
}

void ImagePresentation::SetOnViewChanged(
    std::function<void(const ViewState&)> handler)
{
    m_onViewChanged = std::move(handler);
}

void ImagePresentation::SetOnLoadCompleted(
    std::function<void(const std::wstring&, HRESULT)> handler)
{
    m_onLoadCompleted = std::move(handler);
}

bool ImagePresentation::OnInputEvent(
    const FD2D::InputEvent& event)
{
    switch (event.type)
    {
    case FD2D::InputEventType::MouseWheel:
        if (event.hasPoint &&
            FD2D::Util::RectContainsPoint(
                LayoutRect(),
                event.point))
        {
            const float oldZoom = m_zoom;
            const float notches =
                static_cast<float>(event.wheelDelta) / 120.0f;
            m_zoom = std::clamp(
                m_zoom * std::pow(1.2f, notches),
                kMinZoom,
                kMaxZoom);

            const D2D1_RECT_F rect = LayoutRect();
            const float centerX =
                (rect.left + rect.right) * 0.5f;
            const float centerY =
                (rect.top + rect.bottom) * 0.5f;
            const float relativeX =
                static_cast<float>(event.point.x) -
                centerX -
                m_panX;
            const float relativeY =
                static_cast<float>(event.point.y) -
                centerY -
                m_panY;
            const float scale = 1.0f - m_zoom / oldZoom;
            m_panX += relativeX * scale;
            m_panY += relativeY * scale;
            ApplyDrawState();
            return true;
        }
        return false;

    case FD2D::InputEventType::MouseDown:
        if (event.button == FD2D::MouseButton::Left &&
            event.hasPoint &&
            FD2D::Util::RectContainsPoint(
                LayoutRect(),
                event.point))
        {
            m_panning = true;
            m_dragStartX = event.point.x;
            m_dragStartY = event.point.y;
            m_panStartX = m_panX;
            m_panStartY = m_panY;
            return true;
        }
        return false;

    case FD2D::InputEventType::MouseMove:
        if (m_panning)
        {
            if (!event.modifiers.leftButton)
            {
                m_panning = false;
                return false;
            }
            m_panX =
                m_panStartX +
                static_cast<float>(
                    event.point.x - m_dragStartX);
            m_panY =
                m_panStartY +
                static_cast<float>(
                    event.point.y - m_dragStartY);
            ApplyDrawState();
            return true;
        }
        return false;

    case FD2D::InputEventType::MouseUp:
        if (event.button == FD2D::MouseButton::Left &&
            m_panning)
        {
            m_panning = false;
            return true;
        }
        return false;

    case FD2D::InputEventType::MouseDoubleClick:
        if (event.hasPoint &&
            FD2D::Util::RectContainsPoint(
                LayoutRect(),
                event.point))
        {
            ResetView();
            if (m_onViewChanged)
            {
                m_onViewChanged(GetState());
            }
            return true;
        }
        return false;

    default:
        return false;
    }
}

void ImagePresentation::ApplyDrawState()
{
    PushDrawState();
    if (m_onViewChanged)
    {
        m_onViewChanged(GetState());
    }
}

void ImagePresentation::PushDrawState()
{
    FD2D::Image::DrawState drawState;
    drawState.zoomScale = m_zoom;
    drawState.panX = m_panX;
    drawState.panY = m_panY;
    drawState.rotationQuarters = m_rotation;
    drawState.highQualitySampling = true;
    drawState.alphaCheckerboardEnabled = m_checkerboard;
    drawState.channelMode = m_channelMode;
    drawState.sourceAlphaEncoding =
        m_srvAlpha.encoding ==
            ImageCore::AlphaEncoding::Premultiplied
            ? 1
            : 0;
    drawState.sourceAlphaUsage =
        ResolveAlphaUsage(
            m_srvAlpha,
            m_alphaUsageOverride) ==
            ImageCore::AlphaUsage::Data
            ? 1
            : 0;
    SetDrawState(drawState);
    Invalidate();
}

void ImagePresentation::SetAlphaUsageOverride(
    ImageCore::AlphaUsage usage)
{
    if (m_alphaUsageOverride == usage)
    {
        return;
    }

    m_alphaUsageOverride = usage;
    PushDrawState();
    if (m_usingD2DBitmap)
    {
        std::wstring path;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(m_loadState->mutex);
            path = m_loadState->path;
            generation = m_loadState->generation;
        }
        RestagePayload(path, generation);
        if (m_redraw)
        {
            m_redraw->RequestAsyncRedraw();
        }
    }
}

ImageCore::AlphaUsage
ImagePresentation::AlphaUsageOverride() const
{
    return m_alphaUsageOverride;
}

ImageCore::AlphaUsage
ImagePresentation::EffectiveAlphaUsage() const
{
    return ResolveAlphaUsage(
        m_srvAlpha,
        m_alphaUsageOverride);
}

bool ImagePresentation::IsBlockCompressed(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;

    default:
        return false;
    }
}

DXGI_FORMAT ImagePresentation::TypedSrvFormat(
    DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_TYPELESS:
        return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS:
        return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS:
        return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS:
        return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS:
        return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC6H_TYPELESS:
        return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC7_TYPELESS:
        return DXGI_FORMAT_BC7_UNORM;
    default:
        return format;
    }
}

bool ImagePresentation::TakePending(
    ImageCore::DecodedImage& out,
    std::wstring& outPath,
    std::uint64_t& outGeneration,
    TakeMode mode)
{
    std::lock_guard<std::mutex> lock(m_payloadMutex);
    if (!m_needUpload ||
        !m_payload.blocks ||
        m_payload.blocks->empty())
    {
        return false;
    }
    if (mode == TakeMode::UncompressedForD2D &&
        IsBlockCompressed(m_payload.dxgiFormat))
    {
        return false;
    }

    out = m_payload;
    outPath = m_payloadPath;
    outGeneration = m_payloadGeneration;
    m_needUpload = false;
    return true;
}

void ImagePresentation::RestagePayload(
    const std::wstring& path,
    std::uint64_t generation)
{
    if (!IsCurrent(path, generation))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_payloadMutex);
    if (m_payloadPath == path &&
        m_payloadGeneration == generation &&
        m_payload.blocks &&
        !m_payload.blocks->empty())
    {
        m_needUpload = true;
    }
}

} // namespace nsk
